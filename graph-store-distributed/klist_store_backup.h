#pragma once
#include <stdint.h> //uint64_t
#include <vector>
#include "rdma_resource.h"
#include "request.h" //para_in, para_out, para_all 
#include "global_cfg.h"
#include "ingress.h"

#include <iostream>
#include <pthread.h>
#include <boost/unordered_set.hpp>
#include <tbb/concurrent_hash_map.h>


struct edge_triple{
	uint64_t s;
	uint64_t p;
	uint64_t o;
	edge_triple(uint64_t _s,uint64_t _p, uint64_t _o):
		s(_s),p(_p),o(_o){
	}
	edge_triple():
		s(-1),p(-1),o(-1){
	}
};
struct edge_sort_by_spo
{
    inline bool operator() (const edge_triple& struct1, const edge_triple& struct2)
    {
        if(struct1.s < struct2.s){  
			return true;  
		} else if(struct1.s == struct2.s){
			if(struct1.p < struct2.p){
				return true; 
			} else if(struct1.p == struct2.p && struct1.o < struct2.o){
				return true;
			}
		}
		//otherwise 
		return false;  
	}
};
struct edge_sort_by_ops
{
    inline bool operator() (const edge_triple& struct1, const edge_triple& struct2)
    {
        if(struct1.o < struct2.o){  
			return true;  
		} else if(struct1.o == struct2.o){
			if(struct1.p < struct2.p){
				return true; 
			} else if(struct1.p == struct2.p && struct1.s < struct2.s){
				return true;
			}
		}
		//otherwise 
		return false;  
	}
};

struct edge_row{
	edge_row(uint64_t p,uint64_t v){
		predict=p;
		vid=v;
	}
	edge_row(){
		predict=-1;
		vid=-1;
	}
	uint64_t predict;
	uint64_t vid;
	bool operator < (edge_row const& _A) const {  
			if(predict < _A.predict)  
				return true;  
			if(predict == _A.predict) 
				return vid< _A.vid;  
			return false;  
	}
};
struct vertex_row{
	vector<edge_row> in_edges;
	vector<edge_row> out_edges;
};

struct vertex{
	uint64_t id;
	uint64_t in_edge_ptr;
	uint64_t out_edge_ptr;
	int in_degree;
	int out_degree;
	
	void print(){
		std::cout<<"("<<id<<","<<in_degree<<","<<out_degree
				<<","<<in_edge_ptr<<","<<out_edge_ptr<<")"<<std::endl;
	}
	vertex():id(-1){
	}
};

struct vertex_v2{
	//struct of key
	const static int bits_dir=1;
	const static int bits_predict=15;
	const static int bits_id=48;
	//struct of val
	const static int bits_size=24;
	const static int bits_ptr=40;

	uint64_t key;
	uint64_t val;	
	vertex_v2():key(-1),val(-1){
	}
	uint64_t make_key(uint64_t id,uint64_t dir,uint64_t predict){
		//	1 bit for dir
		//	15 bit for predict
		//	48 bit for id
		assert(dir<= (1<<bits_dir)-1 );
		assert(predict<=(1<<bits_predict)-1);
		uint64_t r=0;
		r+=id;
		r<<=bits_predict;
		r+=predict;
		r<<bits_id;
		r+=id;
		return r;
	}
	uint64_t make_val(uint64_t size,uint64_t ptr){
		uint64_t one=1;
		assert(size<= (one<<bits_size)-1 );
		assert(ptr<=(one<<bits_ptr)-1);
		uint64_t r=0;
		r+=size;
		r<<=bits_ptr;
		r+=ptr;
		return r;
	}
};

uint64_t hash64(uint64_t key) 
{ 
	key = (~key) + (key << 21); // key = (key << 21) - key - 1; 
	key = key ^ (key >> 24); 
	key = (key + (key << 3)) + (key << 8); // key * 265 
	key = key ^ (key >> 14); 
	key = (key + (key << 2)) + (key << 4); // key * 21 
	key = key ^ (key >> 28); 
	key = key + (key << 31); 
	return key; 
}

class loc_cache{
	struct loc_cache_entry{
		pthread_spinlock_t lock;
		vertex v;
	};
	loc_cache_entry * limited_cache;
	uint64_t cache_num;
	uint64_t p_num; 
public:
	loc_cache(uint64_t num,uint64_t partition_num){
		cache_num=num;
		p_num=partition_num;
		limited_cache = new loc_cache_entry[cache_num];
		for(uint64_t i=0;i<cache_num;i++){
			pthread_spin_init(&limited_cache[i].lock,0);
		}
	}
	vertex loc_cache_lookup(uint64_t id){
		uint64_t i=hash64(id)%cache_num;
		vertex v;
		pthread_spin_lock(&limited_cache[i].lock);
		if(limited_cache[i].v.id==id)
			v=limited_cache[i].v;
		pthread_spin_unlock(&limited_cache[i].lock);
		return v;
	}
	void loc_cache_insert(vertex v){
		uint64_t i=hash64(v.id)%cache_num;
		pthread_spin_lock(&limited_cache[i].lock);
		limited_cache[i].v=v;
		pthread_spin_unlock(&limited_cache[i].lock);
	}
};


class klist_store{
	// key to edge-lists
	vertex* vertex_addr;
	edge_row* edge_addr;
	RdmaResource* rdma;
	
	
	uint64_t v_num;
	uint64_t p_num;
	uint64_t p_id;
	loc_cache* location_cache; 
	pthread_spinlock_t allocation_lock;
	static const int num_locks=1024;
	pthread_spinlock_t fine_grain_locks[num_locks];

	
public:
	uint64_t num_try;
	
	uint64_t used_v_num;
	uint64_t used_indirect_num;
	uint64_t max_edge_ptr;
	uint64_t new_edge_ptr;
	klist_store(){
		pthread_spin_init(&allocation_lock,0);
		for(int i=0;i<num_locks;i++){
			pthread_spin_init(&fine_grain_locks[i],0);
		}
	};
	edge_row* getEdgeArray(uint64_t edgeptr){
		return &(edge_addr[edgeptr]);
	}
	uint64_t getEdgeOffset(uint64_t edgeptr){
		return v_num*sizeof(vertex)+sizeof(edge_row)*edgeptr;
	}

	void batch_readGlobal_predict(int tid,const vector<int>& id_vec,int direction,int predict,
				vector<edge_row*>& edge_ptr_vec,vector<int>& size_vec){
		char *local_buffer = rdma->GetMsgAddr(tid);
		char *local_buffer_end=local_buffer+rdma->get_slotsize();
		//getVertex_remote may use some of the buffer, so we should reserve space for it
		local_buffer+=sizeof(vertex)*10;
		vector<uint64_t> edge_offset_vec;
		int local_count=0;
		for(int i=0;i<id_vec.size();i++){
			if(ingress::vid2mid(id_vec[i],p_num) ==p_id){
				local_count++;
				int size=0;
				edge_row* ptr=readLocal_predict(tid,id_vec[i],direction,predict,&size);
				edge_offset_vec.push_back(0);
				edge_ptr_vec.push_back(ptr);
				size_vec.push_back(size);
			} else {
				uint64_t start_addr;
				uint64_t read_length;
				vertex v=getVertex_remote(tid,id_vec[i]);
				if(direction == para_in ){
					edge_offset_vec.push_back(getEdgeOffset(v.in_edge_ptr));
					edge_ptr_vec.push_back((edge_row*)local_buffer);
					size_vec.push_back(v.in_degree);
					local_buffer+=sizeof(edge_row)*v.in_degree;
				}
				if(direction == para_out ){
					edge_offset_vec.push_back(getEdgeOffset(v.out_edge_ptr));
					edge_ptr_vec.push_back((edge_row*)local_buffer);
					size_vec.push_back(v.out_degree);					
					local_buffer+=sizeof(edge_row)*v.out_degree;
				}
			}
		}
		//cout<<"local_rate"<<local_count<<"/"<<id_vec.size()<<":"
		//	<<local_count*1.0/(id_vec.size())<<endl;
		assert(local_buffer<local_buffer_end);

		vector<int>batch_counter_vec;
		batch_counter_vec.resize(p_num);

		for(int i=0;i<id_vec.size();i++){
			int target_mid=ingress::vid2mid(id_vec[i],p_num);
			if(target_mid ==p_id){
				continue;
			}
			batch_counter_vec[target_mid]++;
			rdma->post(tid,target_mid,(char *)(edge_ptr_vec[i]),
					sizeof(edge_row)*size_vec[i],edge_offset_vec[i],IBV_WR_RDMA_READ);
			if(batch_counter_vec[target_mid]==32){
				while(batch_counter_vec[target_mid]>0){
					batch_counter_vec[target_mid]--;
					rdma->poll(tid,target_mid);
				}
			}
		}
		for(int i=0;i<p_num;i++){
			while(batch_counter_vec[i]>0){
				batch_counter_vec[i]--;
				rdma->poll(tid,i);
			}
		}
		for(int i=0;i<id_vec.size();i++){
			int target_mid=ingress::vid2mid(id_vec[i],p_num);
			if(target_mid ==p_id){
				continue;
			}
			int size;
			edge_ptr_vec[i]=find_predict(edge_ptr_vec[i],size_vec[i],predict,&size);
			size_vec[i]=size;
		}
	}
	edge_row* find_predict(edge_row* edge_ptr,int edge_num,int predict,int* size){
		int i=0;
		while(i<edge_num){
			assert(edge_ptr[i].predict==-1);
			if(edge_ptr[i+1].predict<predict){
				i=i+1+edge_ptr[i].vid;
			} else if(edge_ptr[i+1].predict==predict){
				*size=edge_ptr[i].vid;
				return &(edge_ptr[i+1]);
			} else {
				*size=0;
				return NULL;
			}
		}
		return edge_ptr;
	}
	edge_row* readGlobal_predict(int tid,uint64_t id,int direction,int predict,int* size){
		int edge_num=0;
		edge_row* edge_ptr=readGlobal(tid,id,direction,&edge_num);
		edge_ptr=find_predict(edge_ptr,edge_num,predict,size);
		return edge_ptr;
	}
	edge_row* readLocal_predict(int tid,uint64_t id,int direction,int predict,int* size){
		assert(ingress::vid2mid(id,p_num) ==p_id);
		return readGlobal_predict(tid,id,direction,predict,size); 
	}
	edge_row* readLocal(int tid,uint64_t id,int direction,int* size){
		assert(ingress::vid2mid(id,p_num) ==p_id);
		vertex v=getVertex_local(id);
		if(direction == para_in){
			edge_row* edge_ptr=getEdgeArray(v.in_edge_ptr);
			*size=v.in_degree;
			return edge_ptr;
		}
		if(direction == para_out){
			edge_row* edge_ptr=getEdgeArray(v.out_edge_ptr);
			*size=v.out_degree;
			return edge_ptr;
		}
		if(direction == para_all){
			cout<<"not support para_all now"<<endl;
			assert(false);
		}
		return NULL;
	}
	edge_row* readGlobal(int tid,uint64_t id,int direction,int* size){
		if( ingress::vid2mid(id,p_num) ==p_id){
			return readLocal(tid,id,direction,size);
		} else {
			//read vertex data first
			char *local_buffer = rdma->GetMsgAddr(tid);
			// uint64_t start_addr=sizeof(vertex)*(id/p_num);
			// uint64_t read_length=sizeof(vertex);
			uint64_t start_addr;
			uint64_t read_length;
			vertex v;
			if(global_use_loc_cache){
				 v=location_cache->loc_cache_lookup(id);
				 if(v.id!=id){
				 	v=getVertex_remote(tid,id);
				 	location_cache->loc_cache_insert(v);
				 }				
			} else {
				v=getVertex_remote(tid,id);
			}
			//read edge data
			*size=0;
			if(direction == para_all){
				cout<<"not support para_all now"<<endl;
			}
			if(direction == para_in ){
				start_addr=getEdgeOffset(v.in_edge_ptr);
				read_length=sizeof(edge_row)*v.in_degree;
				rdma->RdmaRead(tid,ingress::vid2mid(id,p_num),(char *)local_buffer,read_length,start_addr);
				*size=*size+v.in_degree;
			}
			if(direction == para_out ){
				start_addr=getEdgeOffset(v.out_edge_ptr);
				read_length=sizeof(edge_row)*v.out_degree;
				rdma->RdmaRead(tid,ingress::vid2mid(id,p_num),
									local_buffer+(*size)*sizeof(edge_row),read_length,start_addr);
				*size=*size+v.out_degree;
			}
			edge_row* edge_ptr=(edge_row*)local_buffer;
			return edge_ptr;
		}
	}
	void init(RdmaResource* _rdma,uint64_t vertex_num,uint64_t partition_num,uint64_t partition_id){
		rdma=_rdma;
		v_num=vertex_num;
		used_v_num=0;
		used_indirect_num=0;
		num_try=0;
		p_num=partition_num;
		p_id=partition_id;
		vertex_addr=(vertex*)(rdma->get_buffer());
		edge_addr=(edge_row*)(rdma->get_buffer()+v_num*sizeof(vertex));
		
		new_edge_ptr=0;
		max_edge_ptr=(rdma->get_memorystore_size()-v_num*sizeof(vertex))/sizeof(edge_row);
		
		for(uint64_t i=0;i<v_num;i++){
			vertex_addr[i].id=-1;
		}
		if(global_use_loc_cache){
			location_cache=new loc_cache(100000,p_num);
		}
	}

	uint64_t atomic_alloc_edges(uint64_t num_edge){
		uint64_t curr_edge_ptr;
		pthread_spin_lock(&allocation_lock);
		curr_edge_ptr=new_edge_ptr;
		new_edge_ptr+=num_edge;
		pthread_spin_unlock(&allocation_lock);
		if(new_edge_ptr>=max_edge_ptr){
			cout<<"atomic_alloc_edges out of memory !!!! "<<endl;
			assert(false);
		}
		return curr_edge_ptr;
	}


	int add_pivot(edge_row* edge_list,int size){
		//return new_size
		uint64_t last;
		uint64_t old_size;
		uint64_t new_size;
		uint64_t count;
		last=-1;
		old_size=size;
		new_size=old_size;
		count=0;
		for(uint64_t i=0;i<old_size;i++){
			if(edge_list[i].predict!=last){
				new_size++;
				last=edge_list[i].predict;
			}
		}
		int ret=new_size;
		while(new_size>0){
			edge_list[new_size-1]=edge_list[old_size-1];
			count++;
			new_size--;
			old_size--;
			if(old_size==0 || 
				edge_list[old_size-1].predict != edge_list[old_size].predict){
				edge_list[new_size-1].predict=-1;
				edge_list[new_size-1].vid=count;
				count=0;
				new_size--;
			}
		}
		return ret;
	}


	vertex getVertex_local(uint64_t id){
		uint64_t header_num=(v_num/4)/5*4;
		uint64_t indirect_num=(v_num/4)/5*1;
		uint64_t bucket_id=ingress::hash(id)%header_num;
		while(true){
			for(uint64_t i=0;i<3;i++){
				if(vertex_addr[bucket_id*4+i].id==id){
					//we found it
					return vertex_addr[bucket_id*4+i];
				}
			}
			if(vertex_addr[bucket_id*4+3].id!=-1){
				//next pointer
				bucket_id=vertex_addr[bucket_id*4+3].id;
				continue;
			} else {
				//we didn't found it!
				assert(false);
			}
		}
	}
	vertex getVertex_remote(int tid,uint64_t id){
		char *local_buffer = rdma->GetMsgAddr(tid);
		uint64_t header_num=(v_num/4)/5*4;
		uint64_t indirect_num=(v_num/4)/5*1;
		uint64_t bucket_id=ingress::hash(id)%header_num;
		while(true){
			uint64_t start_addr=sizeof(vertex) * bucket_id *4;
			uint64_t read_length=sizeof(vertex) * 4;
			rdma->RdmaRead(tid,ingress::vid2mid(id,p_num),(char *)local_buffer,read_length,start_addr);
			vertex* ptr=(vertex*)local_buffer;
			for(uint64_t i=0;i<3;i++){
				if(ptr[i].id==id){
					//we found it
					return ptr[i];
				}
			}
			if(ptr[3].id!=-1){
				//next pointer
				bucket_id=ptr[3].id;
				continue;
			} else {
				//we didn't found it!
				assert(false);
			}
		}
	}
	uint64_t insert_or_get_vertex(uint64_t id){
		uint64_t vertex_ptr;
		//4-associate
		uint64_t header_num=(v_num/4)/5*4;
		uint64_t indirect_num=(v_num/4)/5*1;
		uint64_t bucket_id=ingress::hash(id)%header_num;
		uint64_t lock_id=bucket_id% num_locks;
		int slot_id=0;
		bool found=false;
		pthread_spin_lock(&fine_grain_locks[lock_id]);
		//last slot is used as next pointer
		while(!found){
			for(uint64_t i=0;i<3;i++){
				if(vertex_addr[bucket_id*4+i].id==id){
					//we already insert this vertex
					//TODO , if there is a hole in the link-list
					//e.g. we insert and then delete a element
					//It may become a  problem
					found=true;
					slot_id=i;
					break;
				}
				if(vertex_addr[bucket_id*4+i].id==-1){
					vertex_addr[bucket_id*4+i].id=id;
					found=true;
					slot_id=i;
					break;
				}
			}
			if(found){
				break;
			} else if(vertex_addr[bucket_id*4+3].id!=-1){
				//next pointer
				bucket_id=vertex_addr[bucket_id*4+3].id;
				continue;
			} else {
				//need alloc
				pthread_spin_lock(&allocation_lock);
				if(used_indirect_num>=indirect_num){
					assert(false);
				}
				vertex_addr[bucket_id*4+3].id=header_num+used_indirect_num;
				used_indirect_num++;
				pthread_spin_unlock(&allocation_lock);
				bucket_id=vertex_addr[bucket_id*4+3].id;
				vertex_addr[bucket_id*4+0].id=id;
				slot_id=0;
				break;
			}
		}
		pthread_spin_unlock(&fine_grain_locks[lock_id]);
		vertex_ptr = bucket_id*4+slot_id;
		assert(vertex_addr[vertex_ptr].id==id);
		return vertex_ptr;
	}
	void batch_insert(vector<edge_triple>& vec_spo,
						vector<edge_triple>& vec_ops,uint64_t curr_edge_ptr){
		uint64_t start;
		start=0;
		while(start<vec_spo.size()){
			uint64_t end=start+1;
			while(end<vec_spo.size() && vec_spo[start].s==vec_spo[end].s){
				end++;
			}
			uint64_t vertex_ptr=insert_or_get_vertex(vec_spo[start].s);
			vertex_addr[vertex_ptr].out_edge_ptr=curr_edge_ptr;
			uint64_t last_pivot_ptr;
			for(uint64_t i=start;i<end;i++){
				if(i==start || vec_spo[i].p != vec_spo[i-1].p){
					last_pivot_ptr=curr_edge_ptr;
					edge_addr[last_pivot_ptr].predict=-1;
					edge_addr[last_pivot_ptr].vid=0;
					curr_edge_ptr++;
				}
				edge_addr[last_pivot_ptr].vid++;
				edge_addr[curr_edge_ptr].predict=vec_spo[i].p;
				edge_addr[curr_edge_ptr].vid=vec_spo[i].o;
				curr_edge_ptr++;
			}
			vertex_addr[vertex_ptr].out_degree=curr_edge_ptr-vertex_addr[vertex_ptr].out_edge_ptr;
			start=end;
		}
		start=0;
		while(start<vec_ops.size()){
			uint64_t end=start+1;
			while(end<vec_ops.size() && vec_ops[start].o==vec_ops[end].o){
				end++;
			}
			uint64_t vertex_ptr=insert_or_get_vertex(vec_ops[start].o);
			vertex_addr[vertex_ptr].in_edge_ptr=curr_edge_ptr;
			uint64_t last_pivot_ptr;
			for(uint64_t i=start;i<end;i++){
				if(i==start || vec_ops[i].p != vec_ops[i-1].p){
					last_pivot_ptr=curr_edge_ptr;
					edge_addr[last_pivot_ptr].predict=-1;
					edge_addr[last_pivot_ptr].vid=0;
					curr_edge_ptr++;
				}
				edge_addr[last_pivot_ptr].vid++;
				edge_addr[curr_edge_ptr].predict=vec_ops[i].p;
				edge_addr[curr_edge_ptr].vid=vec_ops[i].s;
				curr_edge_ptr++;
			}
			vertex_addr[vertex_ptr].in_degree=curr_edge_ptr-vertex_addr[vertex_ptr].in_edge_ptr;
			start=end;
		}

	}


	void calculate_edge_cut(){
		cout<<"average try= "<<num_try*1.0/used_v_num<<endl;
		uint64_t local_num=0;
		uint64_t remote_num=0;
		for(int i=0;i<v_num;i++){
			if(vertex_addr[i].id!=-1){
				uint64_t degree;
				uint64_t edge_ptr;
				degree  =vertex_addr[i].in_degree;
				edge_ptr=vertex_addr[i].in_edge_ptr;
				for(uint64_t j=0;j<degree;j++){
					if(edge_addr[edge_ptr+j].predict== -1)
						continue;
					if(edge_addr[edge_ptr+j].predict== global_rdftype_id)
						continue;
					if(ingress::vid2mid(edge_addr[edge_ptr+j].vid,p_num)==p_id){
						local_num++;
					} else {
						remote_num++;
					}
				}
				degree  =vertex_addr[i].out_degree;
				edge_ptr=vertex_addr[i].out_edge_ptr;
				for(uint64_t j=0;j<degree;j++){
					if(edge_addr[edge_ptr+j].predict== -1)
						continue;
					if(edge_addr[edge_ptr+j].predict== global_rdftype_id)
						continue;
					if(ingress::vid2mid(edge_addr[edge_ptr+j].vid,p_num)==p_id){
						local_num++;
					} else {
						remote_num++;
					}
				}
			}
		}
		cout<<"edge cut rate: "	<<remote_num<<"/"<<(local_num+remote_num)<<"="
								<<remote_num*1.0/(local_num+remote_num)<<endl;
	}

	vector<boost::unordered_set<uint64_t> >predict_index_vec_in;
	vector<boost::unordered_set<uint64_t> >predict_index_vec_out;
	const boost::unordered_set<uint64_t>& get_predict_index(int predict_id,int dir){
		if(dir==para_in){
			return predict_index_vec_in[predict_id];
		} else {
			return predict_index_vec_out[predict_id];
		}
	}
	void insert_predict_index(uint64_t s, uint64_t p, uint64_t o){
		if(predict_index_vec_in.size()<=p){
			predict_index_vec_in.resize(p+1);
		}
		if(predict_index_vec_out.size()<=p){
			predict_index_vec_out.resize(p+1);
		}
		if(ingress::vid2mid(s,p_num)==p_id){
			predict_index_vec_in[p].insert(s);
		}
		if(ingress::vid2mid(o,p_num)==p_id){
			predict_index_vec_out[p].insert(o);
		}
	}
	void init_predict_index(){
		for(int i=0;i<v_num;i++){
			if(vertex_addr[i].id!=-1){
				uint64_t degree;
				uint64_t edge_ptr;
				degree  =vertex_addr[i].in_degree;
				edge_ptr=vertex_addr[i].in_edge_ptr;
				for(uint64_t j=0;j<degree;j++){
					if(edge_addr[edge_ptr+j].predict== -1)
						continue;
					if(edge_addr[edge_ptr+j].predict== global_rdftype_id)
						continue;
					insert_predict_index(edge_addr[edge_ptr+j].vid,
							edge_addr[edge_ptr+j].predict,vertex_addr[i].id);
				}
				degree  =vertex_addr[i].out_degree;
				edge_ptr=vertex_addr[i].out_edge_ptr;
				for(uint64_t j=0;j<degree;j++){
					if(edge_addr[edge_ptr+j].predict== -1)
						continue;
					if(edge_addr[edge_ptr+j].predict== global_rdftype_id)
						continue;
					insert_predict_index(vertex_addr[i].id,
							edge_addr[edge_ptr+j].predict,edge_addr[edge_ptr+j].vid);
				}
			}
		}
	}
	typedef tbb::concurrent_hash_map<uint64_t,vector<uint64_t> > tbb_vector_table;
	tbb_vector_table type_table;
	vector<uint64_t>& get_vector(tbb_vector_table& table,uint64_t index_id){
		tbb_vector_table::accessor a;
		if (!table.find(a,index_id)){
			assert(false);
		}
		return a->second;
	}
	void insert_vector(tbb_vector_table& table,uint64_t index_id,uint64_t value_id){
		tbb_vector_table::accessor a; 
		table.insert(a,index_id); 
		a->second.push_back(value_id);
	}


	void init_index_table(){
		int count=0;
		//4-associate, 3 data and 1 next
		uint64_t header_num=(v_num/4)/5*4;
		uint64_t indirect_num=(v_num/4)/5*1;
		#pragma omp parallel for num_threads(20)
		for(int x=0;x<header_num+indirect_num;x++){
			for(int y=0;y<3;y++){
				uint64_t i=x*4+y;
				if(vertex_addr[i].id!=-1){
					uint64_t degree;
					uint64_t edge_ptr;
					degree  =vertex_addr[i].out_degree;
					edge_ptr=vertex_addr[i].out_edge_ptr;
					for(uint64_t j=0;j<degree;j++){
						uint64_t s=vertex_addr[i].id;
						uint64_t p=edge_addr[edge_ptr+j].predict;
						uint64_t o=edge_addr[edge_ptr+j].vid;
						if(p==-1){
							continue;
						} else if(p==global_rdftype_id){
							insert_vector(type_table,o,s);
						} else {
							//TODO
						}
					}
				}
			}
		}
	}
};
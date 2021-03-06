/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong
 *
 */

#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <dirent.h>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <assert.h>
#include <boost/mpi.hpp>
#include <boost/unordered_map.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include "omp.h"

#include "config.hpp"
#include "type.hpp"
#include "rdma.hpp"
#include "gstore.hpp"
#include "timer.hpp"

using namespace std;

class DGraph {
	static const int nthread_parallel_load = 16;

	int sid;
	Mem *mem;

	vector<uint64_t> num_triples;  // record #triples loaded from input data for each server

	vector<vector<triple_t>> triple_spo;
	vector<vector<triple_t>> triple_ops;

	void dedup_triples(vector<triple_t> &triples) {
		if (triples.size() <= 1)
			return;

		uint64_t n = 1;
		for (uint64_t i = 1; i < triples.size(); i++) {
			if (triples[i].s == triples[i - 1].s
			        && triples[i].p == triples[i - 1].p
			        && triples[i].o == triples[i - 1].o)
				continue;

			triples[n++] = triples[i];
		}
		triples.resize(n);
	}

	void flush_triples(int tid, int dst_sid) {
		uint64_t buf_sz = floor(mem->buffer_size() / global_num_servers - sizeof(uint64_t), sizeof(sid_t));
		uint64_t *pn = (uint64_t *)(mem->buffer(tid) + (buf_sz + sizeof(uint64_t)) * dst_sid);
		sid_t *buf = (sid_t *)(pn + 1);

		// the 1st uint64_t of buffer records #new-triples
		uint64_t n = *pn;

		// the kvstore is temporally split into #servers pieces.
		// hence, the kvstore can be directly RDMA write in parallel by all servers
		uint64_t kvs_sz = floor(mem->kvstore_size() / global_num_servers - sizeof(uint64_t), sizeof(sid_t));

		// serialize the RDMA WRITEs by multiple threads
		uint64_t exist = __sync_fetch_and_add(&num_triples[dst_sid], n);
		if ((exist * 3 + n * 3) * sizeof(uint64_t) > kvs_sz) {
			cout << "ERROR: no enough space to store input data!" << endl;
			cout << " kvstore size = " << kvs_sz
			     << " #exist-triples = " << exist
			     << " #new-triples = " << n
			     << endl;
			assert(false);
		}

		// send triples and clear the buffer
		uint64_t off = (kvs_sz + sizeof(uint64_t)) * sid
		               + sizeof(uint64_t)           // reserve the 1st uint64_t as #triples
		               + exist * 3 * sizeof(sid_t); // skip #exist-triples
		uint64_t sz = n * 3 * sizeof(sid_t);        // send #new-triples
		if (dst_sid != sid) {
			RDMA &rdma = RDMA::get_rdma();
			rdma.dev->RdmaWrite(tid, dst_sid, (char *)buf, sz, off);
		} else {
			memcpy(mem->kvstore() + off, (char *)buf, sz);
		}

		// clear the buffer
		*pn = 0;
	}

	// send_triple can be safely called by multiple threads,
	// since the buffer is exclusively used by one thread.
	void send_triple(int tid, int dst_sid, sid_t s, sid_t p, sid_t o) {
		// the RDMA buffer is first split into #threads partitions
		// each partition is further split into #servers pieces
		// each piece: #triple, tirple, triple, . . .
		uint64_t buf_sz = floor(mem->buffer_size() / global_num_servers - sizeof(uint64_t), sizeof(sid_t));
		uint64_t *pn = (uint64_t *)(mem->buffer(tid) + (buf_sz + sizeof(uint64_t)) * dst_sid);
		sid_t *buf = (sid_t *)(pn + 1);

		// the 1st entry of buffer records #triples (suppose the )
		uint64_t n = *pn;

		// flush buffer if there is no enough space to buffer a new triple
		if ((n * 3 + 3) * sizeof(sid_t) > buf_sz) {
			flush_triples(tid, dst_sid);
			n = *pn; // reset, it should be 0
			assert(n == 0);
		}

		// buffer the triple and update the counter
		buf[n * 3 + 0] = s;
		buf[n * 3 + 1] = p;
		buf[n * 3 + 2] = o;
		*pn = (n + 1);
	}

	int load_data(vector<string>& fnames) {
		uint64_t t1 = timer::get_usec();

		// ensure the file name list has the same order on all servers
		sort(fnames.begin(), fnames.end());

		// load input data and assign to different severs in parallel
		int num_files = fnames.size();
		#pragma omp parallel for num_threads(global_num_engines)
		for (int i = 0; i < num_files; i++) {
			int localtid = omp_get_thread_num();

			// each server only load a part of files
			if (i % global_num_servers != sid) continue;

			if (boost::starts_with(fnames[i], "hdfs:")) {
				// files located on HDFS
				wukong::hdfs &hdfs = wukong::hdfs::get_hdfs();
				wukong::hdfs::fstream file(hdfs, fnames[i]);
				sid_t s, p, o;
				while (file >> s >> p >> o) {
					int s_sid = mymath::hash_mod(s, global_num_servers);
					int o_sid = mymath::hash_mod(o, global_num_servers);
					if (s_sid == o_sid) {
						send_triple(localtid, s_sid, s, p, o);
					} else {
						send_triple(localtid, s_sid, s, p, o);
						send_triple(localtid, o_sid, s, p, o);
					}
				}
			} else {
				// files located on a shared filesystem (e.g., NFS)
				ifstream file(fnames[i].c_str());
				sid_t s, p, o;
				while (file >> s >> p >> o) {
					int s_sid = mymath::hash_mod(s, global_num_servers);
					int o_sid = mymath::hash_mod(o, global_num_servers);
					if (s_sid == o_sid) {
						send_triple(localtid, s_sid, s, p, o);
					} else {
						send_triple(localtid, s_sid, s, p, o);
						send_triple(localtid, o_sid, s, p, o);
					}
				}
				file.close();
			}
		}

		// flush the rest triples within each RDMA buffer
		for (int s = 0; s < global_num_servers; s++)
			for (int t = 0; t < global_num_engines; t++)
				flush_triples(t, s);

		// exchange #triples among all servers
		for (int s = 0; s < global_num_servers; s++) {
			uint64_t *buf = (uint64_t *)mem->buffer(0);
			buf[0] = num_triples[s];

			uint64_t kvs_sz = floor(mem->kvstore_size() / global_num_servers, sizeof(uint64_t));
			uint64_t offset = kvs_sz * sid;
			if (s != sid) {
				RDMA &rdma = RDMA::get_rdma();
				rdma.dev->RdmaWrite(0, s, (char*)buf, sizeof(uint64_t), offset);
			} else {
				memcpy(mem->kvstore() + offset, (char*)buf, sizeof(uint64_t));
			}
		}
		MPI_Barrier(MPI_COMM_WORLD);

		// timing
		uint64_t t2 = timer::get_usec();
		cout << (t2 - t1) / 1000 << " ms for loading RDF data files" << endl;

		return global_num_servers;
	}

	// selectively load own partitioned data from all files
	int load_data_from_allfiles(vector<string> &fnames) {
		uint64_t t1 = timer::get_usec();

		sort(fnames.begin(), fnames.end());

		int num_files = fnames.size();
		#pragma omp parallel for num_threads(global_num_engines)
		for (int i = 0; i < num_files; i++) {
			int localtid = omp_get_thread_num();
			uint64_t kvs_sz = floor(mem->kvstore_size() / global_num_engines - sizeof(uint64_t), sizeof(sid_t));
			uint64_t *pn = (uint64_t *)(mem->kvstore() + (kvs_sz + sizeof(uint64_t)) * localtid);
			sid_t *kvs = (sid_t *)(pn + 1);

			// the 1st uint64_t of kvs records #triples
			uint64_t n = *pn;

			if (boost::starts_with(fnames[i], "hdfs:")) {
				// files located on HDFS
				wukong::hdfs &hdfs = wukong::hdfs::get_hdfs();
				wukong::hdfs::fstream file(hdfs, fnames[i]);
				sid_t s, p, o;
				while (file >> s >> p >> o) {
					int s_sid = mymath::hash_mod(s, global_num_servers);
					int o_sid = mymath::hash_mod(o, global_num_servers);
					if ((s_sid == sid) || (o_sid == sid)) {
						assert((n * 3 + 3) * sizeof(uint64_t) <= kvs_sz);

						// buffer the triple and update the counter
						kvs[n * 3 + 0] = s;
						kvs[n * 3 + 1] = p;
						kvs[n * 3 + 2] = o;
						n++;
					}
				}
				file.close();
			} else {
				ifstream file(fnames[i].c_str());
				sid_t s, p, o;
				while (file >> s >> p >> o) {
					int s_sid = mymath::hash_mod(s, global_num_servers);
					int o_sid = mymath::hash_mod(o, global_num_servers);
					if ((s_sid == sid) || (o_sid == sid)) {
						assert((n * 3 + 3) * sizeof(uint64_t) <= kvs_sz);

						// buffer the triple and update the counter
						kvs[n * 3 + 0] = s;
						kvs[n * 3 + 1] = p;
						kvs[n * 3 + 2] = o;
						n++;
					}
				}
				file.close();
			}
			*pn = n;
		}

		// timing
		uint64_t t2 = timer::get_usec();
		cout << (t2 - t1) / 1000 << " ms for loading RDF data files (w/o networking)" << endl;

		return global_num_engines;
	}

	void aggregate_data(int num_partitions) {
		uint64_t t1 = timer::get_usec();

		// calculate #triples on the kvstore from all servers
		uint64_t total = 0;
		uint64_t kvs_sz = floor(mem->kvstore_size() / num_partitions - sizeof(uint64_t), sizeof(sid_t));
		for (int id = 0; id < num_partitions; id++) {
			uint64_t *pn = (uint64_t *)(mem->kvstore() + (kvs_sz + sizeof(uint64_t)) * id);
			total += *pn; // the 1st uint64_t of kvs records #triples
		}

		// pre-expand to avoid frequent reallocation (maybe imbalance)
		for (int i = 0; i < triple_spo.size(); i++) {
			triple_spo[i].reserve(total / nthread_parallel_load);
			triple_ops[i].reserve(total / nthread_parallel_load);
		}

		// each thread will scan all triples (from all servers) and pickup certain triples.
		// It ensures that the triples belong to the same vertex will be stored in the same
		// triple_spo/ops. This will simplify the deduplication and insertion to gstore.
		volatile int progress = 0;
		#pragma omp parallel for num_threads(nthread_parallel_load)
		for (int tid = 0; tid < nthread_parallel_load; tid++) {
			int cnt = 0; // per thread count for print progress
			for (int id = 0; id < num_partitions; id++) {
				uint64_t *pn = (uint64_t *)(mem->kvstore() + (kvs_sz + sizeof(uint64_t)) * id);
				sid_t *kvs = (sid_t *)(pn + 1);

				// the 1st uint64_t of kvs records #triples
				uint64_t n = *pn;
				for (uint64_t i = 0; i < n; i++) {
					sid_t s = kvs[i * 3 + 0];
					sid_t p = kvs[i * 3 + 1];
					sid_t o = kvs[i * 3 + 2];

					// out-edges
					if (mymath::hash_mod(s, global_num_servers) == sid)
						if ((s % nthread_parallel_load) == tid)
							triple_spo[tid].push_back(triple_t(s, p, o));

					// in-edges
					if (mymath::hash_mod(o, global_num_servers) == sid)
						if ((o % nthread_parallel_load) == tid)
							triple_ops[tid].push_back(triple_t(s, p, o));

					// print the progress (step = 5%) of aggregation
					if (++cnt >= total * 0.05) {
						int now = __sync_add_and_fetch(&progress, 1);
						if (now % nthread_parallel_load == 0)
							cout << "already aggregrate " << (now / nthread_parallel_load) * 5 << "%" << endl;
						cnt = 0;
					}
				}
			}

			sort(triple_spo[tid].begin(), triple_spo[tid].end(), edge_sort_by_spo());
			dedup_triples(triple_ops[tid]);

			sort(triple_ops[tid].begin(), triple_ops[tid].end(), edge_sort_by_ops());
			dedup_triples(triple_spo[tid]);
		}

		// timing
		uint64_t t2 = timer::get_usec();
		cout << (t2 - t1) / 1000 << " ms for aggregrating triples" << endl;
	}

	uint64_t inline floor(uint64_t original, uint64_t n) {
		assert(n != 0);
		return original - original % n;
	}

	uint64_t inline ceil(uint64_t original, uint64_t n) {
		assert(n != 0);
		if (original % n == 0)
			return original;
		return original - original % n + n;
	}

public:
	GStore gstore;

	DGraph(int sid, Mem *mem, string dname)
		: sid(sid), mem(mem), gstore(sid, mem) {

		num_triples.resize(global_num_servers);
		triple_spo.resize(nthread_parallel_load);
		triple_ops.resize(nthread_parallel_load);

		vector<string> files; // ID-format data files
		if (boost::starts_with(dname, "hdfs:")) {
			if (!wukong::hdfs::has_hadoop()) {
				cout << "ERROR: attempting to load data files from HDFS "
				     << "but Wukong was built without HDFS."
				     << endl;
				exit(-1);
			}

			wukong::hdfs &hdfs = wukong::hdfs::get_hdfs();
			files = hdfs.list_files(dname);
		} else {
			// files located on a shared filesystem (e.g., NFS)
			DIR *dir = opendir(dname.c_str());
			if (dir == NULL) {
				cout << "ERORR: failed to open directory (" << dname
				     << ") at server " << sid << endl;
				exit(-1);
			}

			struct dirent *ent;
			while ((ent = readdir(dir)) != NULL) {
				if (ent->d_name[0] == '.')
					continue;

				string fname(dname + ent->d_name);
				// Assume the fnames of RDF data files (ID-format) start with 'id_'.
				/// TODO: move RDF data files and metadata files to different directories
				if (boost::starts_with(fname, dname + "id_"))
					files.push_back(fname);
			}
		}

		if (files.size() == 0) {
			cout << "ERORR: no files found in directory (" << dname
			     << ") at server " << sid << endl;
			assert(false);
		}

		// load_data: load partial input files by each server and exchanges triples
		//            according to graph partitioning
		// load_data_from_allfiles: load all files by each server and select triples
		//                          according to graph partitioning
		//
		// Trade-off: load_data_from_allfiles avoids network traffic and memory,
		//            but it requires more I/O from distributed FS.
		//
		// Wukong adopts load_data_from_allfiles for slow network (w/o RDMA) and
		//        adopts load_data for fast network (w/ RDMA).
		int num_partitons = 0;

		if (global_use_rdma)
			num_partitons = load_data(files);
		else
			num_partitons = load_data_from_allfiles(files);

		// all triples are partitioned and temporarily stored in the kvstore on each server.
		// the kvstore is split into num_partitions partitions, each contains #triples and triples
		//
		// Wukong aggregates, sorts and dedups all triples before finally inserting them to gstore (kvstore)
		aggregate_data(num_partitons);

		// initiate gstore (kvstore) after loading and exchanging triples
		gstore.init();

		uint64_t t1 = timer::get_usec();
		#pragma omp parallel for num_threads(nthread_parallel_load)
		for (int t = 0; t < nthread_parallel_load; t++) {
			gstore.insert_normal(triple_spo[t], triple_ops[t]);

			// release memory
			vector<triple_t>().swap(triple_spo[t]);
			vector<triple_t>().swap(triple_ops[t]);
		}
		uint64_t t2 = timer::get_usec();
		cout << (t2 - t1) / 1000 << " ms for inserting normal data into gstore" << endl;

		gstore.insert_index();

		cout << "INFO#" << sid << ": loading DGraph is finished." << endl;
		gstore.print_mem_usage();
	}

	edge_t *get_edges_global(int tid, sid_t vid, dir_t direction, sid_t pid, uint64_t *sz) {
		return gstore.get_edges_global(tid, vid, direction, pid, sz);
	}

	edge_t *get_index_edges_local(int tid, sid_t vid, dir_t direction, uint64_t *sz) {
		return gstore.get_index_edges_local(tid, vid, direction, sz);
	}
};

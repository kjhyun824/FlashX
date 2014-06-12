#ifndef __READ_PRIVATE_H__
#define __READ_PRIVATE_H__

/**
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of SAFSlib.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "io_interface.h"
#include "file_partition.h"
#include "parameters.h"

class buffered_io: public io_interface
{
	logical_file_partition partition;
	/* the array of files that it's going to access */
	std::vector<int> fds;

	int flags;
	long remote_reads;
#ifdef STATISTICS
	long read_time; // in us
	long num_reads;
#endif
public:
	buffered_io(const logical_file_partition &partition_,
			thread *t, int flags = O_RDWR);

	virtual ~buffered_io() {
		for (unsigned i = 0; i < fds.size(); i++) {
			int ret = close(fds[i]);
			assert(ret == 0);
		}
	}

	/* get the file descriptor corresponding to the offset. */
	int get_fd(long offset) {
		if (fds.size() == 1)
			return fds[0];

		int idx = partition.map2file(offset / PAGE_SIZE);
		return fds[idx];
	}

	const std::vector<int> &get_fds() const {
		return fds;
	}

	int num_open_files() {
		return partition.get_num_files();
	}

	const logical_file_partition &get_partition() const {
		return partition;
	}

	int get_file_id() const {
		return partition.get_file_id();
	}

	int init();

	void cleanup() {
		for (size_t i = 0; i < fds.size(); i++)
			fsync(fds[i]);
	}

	io_status access(char *buf, off_t offset, ssize_t size, int access_method);

#ifdef STATISTICS
	virtual void print_stat(int nthreads) {
		static int seen_threads = 0;
		static long tot_nreads;
		static long tot_read_time;
		static long tot_remote_reads;
		tot_remote_reads += remote_reads;
		tot_nreads += num_reads;
		tot_read_time += read_time;
		seen_threads++;
		if (seen_threads == nthreads) {
			printf("there are %ld reads and takes %ldus\n", tot_nreads, tot_read_time);
		}
	}
#endif
};

#endif

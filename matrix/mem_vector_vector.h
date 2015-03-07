#ifndef __MEM_VECTOR_VECTOR_H__
#define __MEM_VECTOR_VECTOR_H__

/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
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

#include <stdlib.h>

#include <vector>

#include "generic_type.h"
#include "vector_vector.h"

namespace fm
{

class mem_vector_vector: public vector_vector
{
	struct deleter {
		void operator()(char *p) const{
			free(p);
		}
	};

	std::vector<off_t> vec_offs;

	// The start pointer to the data
	std::shared_ptr<char> data;
	// The capacity of the data array in bytes.
	size_t capacity;

	/*
	 * This method expends the data array so it has at least `min' bytes.
	 */
	void expand(size_t min);

	size_t get_num_bytes() const {
		return vec_offs.back();
	}

	char *get_end() {
		return data.get() + get_num_bytes();
	}

protected:
	mem_vector_vector(): vector_vector(0, true) {
		vec_offs.push_back(0);
		capacity = 1024;
		data = std::shared_ptr<char>((char *) malloc(capacity), deleter());
	}
public:
	typedef std::shared_ptr<mem_vector_vector> ptr;

	virtual size_t get_tot_num_entries() const {
		return get_num_bytes() / get_type().get_size();
	}

	virtual size_t get_length(off_t idx) const {
		return vec_offs[idx + 1] - vec_offs[idx];
	}
	virtual const char*get_raw_arr(off_t idx) const {
		return data.get() + vec_offs[idx];
	}

	bool append(const vector &vec);
	virtual bool append(std::vector<vector::ptr>::const_iterator vec_it,
			std::vector<vector::ptr>::const_iterator vec_end);

	virtual std::shared_ptr<vector> cat() const;
	virtual vector_vector::ptr groupby(const factor_vector &labels,
			const gr_apply_operate<sub_vector_vector> &op) const;
};

template<class T>
class type_mem_vector_vector: public mem_vector_vector
{
	type_mem_vector_vector() {
	}
public:
	static ptr create() {
		return ptr(new type_mem_vector_vector<T>());
	}

	virtual const scalar_type &get_type() const {
		return get_scalar_type<T>();
	}
};

}

#endif

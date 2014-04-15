/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SA-GraphLib.
 *
 * SA-GraphLib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SA-GraphLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SA-GraphLib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <google/profiler.h>

#include <set>
#include <vector>

#include "thread.h"
#include "io_interface.h"

#include "graph_engine.h"
#include "graph_config.h"

#include "scan_graph.h"

struct timeval graph_start;

class vertex_size_scheduler: public vertex_scheduler
{
	graph_engine *graph;
public:
	vertex_size_scheduler(graph_engine *graph) {
		this->graph = graph;
	}
	void schedule(std::vector<vertex_id_t> &vertices);
};

void vertex_size_scheduler::schedule(std::vector<vertex_id_t> &ids)
{
	std::vector<compute_vertex *> vertices(ids.size());
	for (size_t i = 0; i < ids.size(); i++)
		vertices[i] = &graph->get_vertex(ids[i]);
	class comp_size
	{
	public:
		bool operator()(const compute_vertex *v1, const compute_vertex *v2) {
			return v1->get_num_edges() > v2->get_num_edges();
		}
	};

	std::sort(vertices.begin(), vertices.end(), comp_size());
	for (size_t i = 0; i < ids.size(); i++)
		ids[i] = vertices[i]->get_id();
}

class global_max
{
	volatile size_t value;
	pthread_spinlock_t lock;
public:
	global_max() {
		value = 0;
		pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
	}

	global_max(size_t init) {
		value = init;
		pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
	}

	bool update(size_t new_v) {
		if (new_v <= value)
			return false;

		bool ret = false;
		pthread_spin_lock(&lock);
		if (new_v > value) {
			value = new_v;
			ret = true;
		}
		pthread_spin_unlock(&lock);
		return ret;
	}

	size_t get() const {
		return value;
	}
} max_scan;

typedef std::pair<vertex_id_t, size_t> vertex_scan;

/**
 * This class maintains the local scan that have been computed.
 */
class scan_collection
{
	bool sorted;
	std::vector<vertex_scan> scans;
	pthread_spinlock_t lock;

	class greater {
	public:
		bool operator()(const vertex_scan &s1, const vertex_scan &s2) {
			return s1.second > s2.second;
		}
	};
public:
	scan_collection() {
		pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
		sorted = false;
	}

	/**
	 * Get the ith largest scan.
	 */
	vertex_scan get(int idx) {
		pthread_spin_lock(&lock);
		if (!sorted) {
			// It needs to stored in the descending order on scan.
			std::sort(scans.begin(), scans.end(), greater());
			sorted = true;
		}
		vertex_scan ret = scans[idx];
		pthread_spin_unlock(&lock);
		return ret;
	}

	void add(vertex_id_t id, size_t scan) {
		pthread_spin_lock(&lock);
		sorted = false;
		scans.push_back(vertex_scan(id, scan));
		pthread_spin_unlock(&lock);
	}

	size_t get_size() {
		pthread_spin_lock(&lock);
		size_t ret = scans.size();
		pthread_spin_unlock(&lock);
		return ret;
	}
} known_scans;

class topK_scan_vertex: public scan_vertex
{
public:
	topK_scan_vertex() {
	}

	topK_scan_vertex(vertex_id_t id, const vertex_index *index): scan_vertex(
			id, index) {
	}

	bool has_est_local() const {
		return local_value.has_est_local();
	}

	size_t get_est_local_scan() const {
		return local_value.get_est_local();
	}

	size_t get_est_local_scan(graph_engine &graph, const page_vertex *vertex);

	using scan_vertex::run;
	void run(graph_engine &graph);
	void run(graph_engine &graph, const page_vertex &vertex) {
		if (vertex.get_id() == get_id())
			run_on_itself(graph, vertex);
		else
			run_on_neighbor(graph, vertex);
	}
	void run_on_itself(graph_engine &graph, const page_vertex &vertex);

	void finding_triangles_end(graph_engine &graph, runtime_data_t *data) {
		if (max_scan.update(data->local_scan)) {
			struct timeval curr;
			gettimeofday(&curr, NULL);
			printf("%d: new max scan: %ld at v%u\n",
					(int) time_diff(graph_start, curr),
					data->local_scan, get_id());
		}
		known_scans.add(get_id(), data->local_scan);
	}
};

void topK_scan_vertex::run(graph_engine &graph)
{
	bool req_itself = false;
	// If we have computed local scan on the vertex, skip the vertex.
	if (has_local_scan())
		return;
	// If we have estimated the local scan, we should use the estimated one.
	else if (has_est_local())
		req_itself = get_est_local_scan() > max_scan.get();
	else {
		// If this is the first time to compute on the vertex, we can still
		// skip a lot of vertices with this condition.
		size_t num_local_edges = get_num_edges();
		req_itself = num_local_edges * num_local_edges >= max_scan.get();
	}
	if (req_itself) {
		vertex_id_t id = get_id();
		request_vertices(&id, 1);
	}
}

size_t topK_scan_vertex::get_est_local_scan(graph_engine &graph, const page_vertex *vertex)
{
	// We have estimated the local scan of this vertex, return
	// the estimated one
	if (has_est_local())
		return get_est_local_scan();

	class skip_self {
		vertex_id_t id;
	public:
		skip_self(vertex_id_t id) {
			this->id = id;
		}

		bool operator()(vertex_id_t id) {
			return this->id == id;
		}
	};

	class merge_edge {
	public:
		vertex_id_t operator()(vertex_id_t e1, vertex_id_t e2) {
			assert(e1 == e2);
			return e1;
		}
	};

	std::vector<vertex_id_t> all_neighbors(
			vertex->get_num_edges(edge_type::BOTH_EDGES));
	size_t num_neighbors = unique_merge(
			vertex->get_neigh_begin(edge_type::IN_EDGE),
			vertex->get_neigh_end(edge_type::IN_EDGE),
			vertex->get_neigh_begin(edge_type::OUT_EDGE),
			vertex->get_neigh_end(edge_type::OUT_EDGE),
			skip_self(vertex->get_id()), merge_edge(),
			all_neighbors.begin());
	all_neighbors.resize(num_neighbors);

	size_t tot_edges = get_num_edges();
	for (size_t i = 0; i < all_neighbors.size(); i++) {
		scan_vertex &v = (scan_vertex &) graph.get_vertex(all_neighbors[i]);
		// The max number of common neighbors should be smaller than all neighbors
		// in the neighborhood, assuming there aren't duplicated edges.
		tot_edges += min(v.get_num_edges(), num_neighbors * 2);
	}
	tot_edges /= 2;
	local_value.set_est_local(tot_edges);
	return tot_edges;
}

void topK_scan_vertex::run_on_itself(graph_engine &graph, const page_vertex &vertex)
{
	size_t num_local_edges = vertex.get_num_edges(edge_type::BOTH_EDGES);
	assert(num_local_edges == get_num_edges());
	if (num_local_edges == 0)
		return;

	if (get_est_local_scan(graph, &vertex) < max_scan.get())
		return;
	scan_vertex::run_on_itself(graph, vertex);
}

void topK_finding_triangles_end(graph_engine &graph, scan_vertex &scan_v,
		runtime_data_t *data)
{
	topK_scan_vertex &topK_v = (topK_scan_vertex &) scan_v;
	topK_v.finding_triangles_end(graph, data);
}

void int_handler(int sig_num)
{
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	exit(0);
}

void print_usage()
{
	fprintf(stderr,
			"topK-scan [options] conf_file graph_file index_file\n");
	fprintf(stderr, "-c confs: add more configurations to the system\n");
	fprintf(stderr, "-p: preload the graph\n");
	graph_conf.print_help();
	params.print_help();
}

int main(int argc, char *argv[])
{
	size_t topK = 1;
	size_t min_edges = 1000;
	int opt;
	std::string confs;
	int num_opts = 0;
	bool preload = false;
	while ((opt = getopt(argc, argv, "c:p")) != -1) {
		num_opts++;
		switch (opt) {
			case 'c':
				confs = optarg;
				num_opts++;
				break;
			case 'p':
				preload = true;
				break;
			default:
				print_usage();
		}
	}
	argv += 1 + num_opts;
	argc -= 1 + num_opts;

	if (argc < 3) {
		print_usage();
		exit(-1);
	}

	std::string conf_file = argv[0];
	std::string graph_file = argv[1];
	std::string index_file = argv[2];

	config_map configs(conf_file);
	configs.add_options(confs);
	graph_conf.init(configs);
	graph_conf.print();

	signal(SIGINT, int_handler);
	init_io_system(configs);

	finding_triangles_end = topK_finding_triangles_end;

	graph_index *index = NUMA_graph_index<topK_scan_vertex>::create(
			index_file, graph_conf.get_num_threads(), params.get_num_nodes());
	graph_engine *graph = graph_engine::create(
			graph_conf.get_num_threads(), params.get_num_nodes(), graph_file,
			index);
	if (preload)
		graph->preload_graph();
	// Let's schedule the order of processing activated vertices according
	// to the size of vertices. We start with processing vertices with higher
	// degrees in the hope we can find the max scan as early as possible,
	// so that we can simple ignore the rest of vertices.
	graph->set_vertex_scheduler(new vertex_size_scheduler(graph));
	printf("scan statistics starts\n");
	printf("prof_file: %s\n", graph_conf.get_prof_file().c_str());
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());

	class remove_small_filter: public vertex_filter
	{
		size_t min;
	public:
		remove_small_filter(size_t min) {
			this->min = min;
		}

		bool keep(compute_vertex &v) {
			topK_scan_vertex &scan_v = (topK_scan_vertex &) v;
			return scan_v.get_num_edges() >= min;
		}
	};

	std::shared_ptr<vertex_filter> filter
		= std::shared_ptr<vertex_filter>(new remove_small_filter(min_edges));
	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph_start = start;
	printf("Computing local scan on at least %ld vertices\n", topK);
	while (known_scans.get_size() < topK) {
		gettimeofday(&start, NULL);
		graph->start(filter);
		graph->wait4complete();
		gettimeofday(&end, NULL);
		printf("It takes %f seconds\n", time_diff(start, end));
		printf("process %ld vertices and complete %ld vertices\n",
				num_working_vertices.get(), num_completed_vertices.get());
		printf("global max scan: %ld\n", max_scan.get());
		max_scan = global_max(0);
	}

	class remove_small_scan_filter: public vertex_filter
	{
		size_t min;
	public:
		remove_small_scan_filter(size_t min) {
			this->min = min;
		}

		bool keep(compute_vertex &v) {
			topK_scan_vertex &scan_v = (topK_scan_vertex &) v;
			size_t num_local_edges = scan_v.get_num_edges();
			return num_local_edges * num_local_edges >= min;
		}
	};

	printf("Compute local scan on %ld vertices\n", known_scans.get_size());
	printf("Looking for top %ld local scan\n", topK);
	size_t prev_topK_scan;
	do {
		prev_topK_scan = known_scans.get(topK - 1).second;
		// Let's use the topK as the max scan for unknown vertices
		// and see if we can find a new vertex that has larger local scan.
		max_scan = global_max(prev_topK_scan);

		gettimeofday(&start, NULL);
		graph->start(std::shared_ptr<vertex_filter>(
					new remove_small_scan_filter(prev_topK_scan)));
		graph->wait4complete();
		gettimeofday(&end, NULL);
		printf("It takes %f seconds\n", time_diff(start, end));
		printf("process %ld vertices and complete %ld vertices\n",
				num_working_vertices.get(), num_completed_vertices.get());
		printf("global max scan: %ld\n", max_scan.get());
		// If the previous topK is different from the current one,
		// it means we have found new local scans that are larger
		// than the previous topK. We should use the new topK and
		// try again.
	} while (prev_topK_scan != known_scans.get(topK - 1).second);

	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	if (graph_conf.get_print_io_stat())
		print_io_thread_stat();
	graph_engine::destroy(graph);
	destroy_io_system();

	assert(known_scans.get_size() >= topK);
	for (size_t i = 0; i < topK; i++) {
		vertex_scan scan = known_scans.get(i);
		printf("No. %ld: %u, %ld\n", i, scan.first, scan.second);
	}
	printf("It takes %f seconds for top %ld\n", time_diff(graph_start, end),
			topK);

#ifdef PV_STAT
	graph_index::const_iterator it = index->begin();
	graph_index::const_iterator end_it = index->end();
	size_t tot_scan_bytes = 0;
	size_t tot_rand_jumps = 0;
	for (; it != end_it; ++it) {
		const topK_scan_vertex &v = (const topK_scan_vertex &) *it;
		tot_scan_bytes += v.get_scan_bytes();
		tot_rand_jumps += v.get_rand_jumps();
	}
	printf("scan %ld bytes, %ld rand jumps\n",
			tot_scan_bytes, tot_rand_jumps);
#endif
}

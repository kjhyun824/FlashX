#include "signal.h"
#include "FGlib.h"
#include "matrix/kmeans.h"
#include "libgraph-algs/sem_kmeans.h"

using namespace fg;

static bool is_file_exist(const char *fileName) {
    std::ifstream infile(fileName);
    return infile.good();
}

static void int_handler(int sig_num) { exit(0); }
static void print_usage();

int main(int argc, char* argv[]) {

    if (argc < 5) {
        print_usage();
        exit(EXIT_FAILURE);
    }

	int opt;
    std::string datafn = std::string(argv[1]);
    unsigned nrow = atol(argv[2]);
    unsigned ncol = atol(argv[3]);
    unsigned k = atol(argv[4]);

    std::string dist_type = "eucl";
    std::string centersfn = ""; 
	unsigned max_iters=std::numeric_limits<unsigned>::max();
	std::string init = "kmeanspp";
	unsigned nthread = 1024;
	int num_opts = 0;
	double tolerance = -1;

    // Increase by 3 -- getopt ignores argv[0]
	argv += 3;
	argc -= 3;
    
	signal(SIGINT, int_handler);
	while ((opt = getopt(argc, argv, "l:i:t:T:d:C:")) != -1) {
		num_opts++;
		switch (opt) {
			case 'l':
				tolerance = atof(optarg);
				num_opts++;
				break;
			case 'i':
				max_iters = atol(optarg);
				num_opts++;
				break;
			case 't':
				init = optarg;
				num_opts++;
				break;
			case 'T':
				nthread = atoi(optarg);
				num_opts++;
				break;
			case 'd':
				dist_type = std::string(optarg);
				num_opts++;
				break;
			case 'C':
				centersfn = std::string(optarg);
				num_opts++;
				break;
			default:
				print_usage();
		}
	}

    bin_reader<double> br(datafn, nrow, ncol);
    double* p_data = new double [nrow*ncol];
    br.read(p_data);
    printf("Read data!\n");

    double* p_centers = new double [k*ncol];

    if (is_file_exist(centersfn.c_str())) {
        bin_reader<double> br2(centersfn, k, ncol);
        br2.readline(p_centers);
        printf("Read centers!\n");
    } else
        printf("No centers to read ..\n");

    unsigned* p_clust_asgns = new unsigned [nrow];
    unsigned* p_clust_asgn_cnt = new unsigned [k];

    compute_kmeans(p_data, p_centers, p_clust_asgns,
            p_clust_asgn_cnt, nrow, ncol, k, max_iters,
            nthread, init, tolerance, dist_type);

    delete [] p_clust_asgns;
    delete [] p_clust_asgn_cnt;

    delete [] p_data;
    delete [] p_centers;

    return EXIT_SUCCESS;
}

void print_usage() {
	fprintf(stderr,
        "test-kmeans data-file num-rows num-cols k [alg-options]\n");
    fprintf(stderr, "-t type: type of initialization for kmeans"
           " ['random', 'forgy', 'kmeanspp', 'none']\n");
    fprintf(stderr, "-T num_thread: The number of OMP threads to run\n");
    fprintf(stderr, "-i iters: maximum number of iterations\n");
    fprintf(stderr, "-C File with initial clusters in same format as data\n");
    fprintf(stderr, "-l tolerance for convergence (1E-6)\n");
    fprintf(stderr, "-d Distance matric [eucl,cos]\n");
}
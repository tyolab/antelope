/*
	BENCH_HNSW.CPP -- build-time benchmark for ANT_hnsw.

	Builds a graph over N synthetic vectors and reports wall-clock build time
	(and, when hnsw.cpp is compiled with -DANT_HNSW_PROFILE, the distance()
	call count broken down by phase).  Not run by `make tests`; invoke
	bin/bench_hnsw directly.  Usage: bench_hnsw [N] [dim] [M] [efConstruction]
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "../source/hnsw.h"
#include "../source/vector_store.h"

#ifdef ANT_HNSW_PROFILE
extern long long ant_hnsw_cache_hit, ant_hnsw_cache_miss;
#endif

static double now_sec(void)
{
struct timespec ts;
clock_gettime(CLOCK_MONOTONIC, &ts);
return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
long long n   = argc > 1 ? atoll(argv[1]) : 20000;
long long dim = argc > 2 ? atoll(argv[2]) : 128;
long long M   = argc > 3 ? atoll(argv[3]) : 16;
long long efc = argc > 4 ? atoll(argv[4]) : 200;
bool use_cache = argc > 5 ? atoll(argv[5]) != 0 : true;

printf("bench_hnsw: N=%lld dim=%lld M=%lld efConstruction=%lld cache=%d\n", n, dim, M, efc, (int)use_cache);

float *data = new float[n * dim];
srand(1234);
for (long long i = 0; i < n * dim; i++)
	data[i] = (float)(rand() % 2000 - 1000) / 1000.0f;		/* [-1,1] */

char path[64]; strcpy(path, "/tmp/ant_bench_XXXXXX"); int fd = mkstemp(path); if (fd >= 0) close(fd);
ANT_vector_store_writer w;
if (w.create(path, dim) != 0) { printf("create failed\n"); return 1; }
for (long long i = 0; i < n; i++) w.append(data + i * dim);
w.finish();
ANT_vector_store *store = ANT_vector_store::load(path, dim, n);

double t0 = now_sec();
ANT_hnsw g;
long rc = g.build(store, M, efc, ANT_vector_store::METRIC_L2, use_cache);
double t1 = now_sec();

printf("build rc=%ld nodes=%lld  time=%.3f s\n", rc, g.node_count(), t1 - t0);
#ifdef ANT_HNSW_PROFILE
long long looked = ant_hnsw_cache_hit + ant_hnsw_cache_miss;
printf("dcache: hits=%lld misses=%lld  hit-rate=%.1f%%  (actual distance() computed = misses)\n",
	ant_hnsw_cache_hit, ant_hnsw_cache_miss, looked ? 100.0*ant_hnsw_cache_hit/looked : 0.0);
#endif

delete store; delete [] data; unlink(path);
return 0;
}

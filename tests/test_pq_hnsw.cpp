/*
	TEST_PQ_HNSW.CPP -- proves ANT_hnsw can build/search directly over an
	ANT_pq_store (a quantized ANT_vector_source): the graph must drive itself
	via reconstruct()/score() alone, never get() (which returns NULL for a PQ
	store and would crash a caller that assumed a float pointer).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/hnsw.h"
#include "../source/vector_store.h"
#include "../source/pq_store.h"
#include "../source/pq_codec.h"
#include "../source/index_tombstones.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static ANT_vector_store *make_float_store(const char *path, long long dim, long long n, float *data)
{
ANT_vector_store_writer w;
long long i;
CHECK(w.create(path, dim) == 0);
for (i = 0; i < n; i++)
	CHECK(w.append(data + i * dim) == 0);
CHECK(w.finish() == 0);
return ANT_vector_store::load(path, dim, n);
}

static ANT_pq_store *make_pq_store(const char *path, long long dim, long long m, long long n, float *data)
{
ANT_pq_store_writer w;
long long i;
CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT) == 0);
for (i = 0; i < n; i++)
	CHECK(w.append(data + i * dim) == 0);
CHECK(w.finish() == 0);
return ANT_pq_store::load(path, dim, n, ANT_pq_codec::METRIC_DOT);
}

static void unit_normalize(float *v, long long dim)
{
double norm = 0.0;
long long d;
for (d = 0; d < dim; d++) norm += (double)v[d] * (double)v[d];
norm = sqrt(norm);
if (norm <= 0.0) return;
for (d = 0; d < dim; d++) v[d] = (float)((double)v[d] / norm);
}

static void brute_force_topk(const float *query, const float *data, long long dim, long long n, long metric, long long k, long long *out)
{
long long i, j;
double *score = new double[n];
for (i = 0; i < n; i++)
	score[i] = ANT_vector_store::kernel(query, data + i * dim, dim, metric);
for (j = 0; j < k; j++)
	{
	long long best = -1;
	for (i = 0; i < n; i++)
		if (score[i] > -1e300 && (best < 0 || score[i] > score[best]))
			best = i;
	out[j] = best;
	score[best] = -1e300;
	}
delete [] score;
}

static void test_pq_backed_hnsw_recall(void)
{
long long dim = 16, m = 4, n = 300, k = 10, i, d, qi;
char fpath[64]; strcpy(fpath, "/tmp/ant_pqhnsw_f_XXXXXX"); { int fd = mkstemp(fpath); if (fd >= 0) close(fd); }
char ppath[64]; strcpy(ppath, "/tmp/ant_pqhnsw_p_XXXXXX"); { int fd = mkstemp(ppath); if (fd >= 0) close(fd); }

float *data = new float[n * dim];
srand(77);
for (i = 0; i < n; i++)
	{
	for (d = 0; d < dim; d++)
		data[i * dim + d] = (float)(rand() % 2000 - 1000) / 500.0f;
	unit_normalize(data + i * dim, dim);
	}

ANT_vector_store *fstore = make_float_store(fpath, dim, n, data);
ANT_pq_store *pstore = make_pq_store(ppath, dim, m, n, data);
CHECK(pstore->document_count() == n);
CHECK(pstore->is_quantized());
CHECK(pstore->get(0) == 0);		// PQ store never hands back a float pointer

ANT_hnsw graph;
CHECK(graph.build(pstore, 16, 200, ANT_vector_store::METRIC_DOT) == 0);
CHECK(graph.node_count() == n);

ANT_index_tombstones stones(n);
long long overlap = 0, total = 0;
for (qi = 0; qi < 20; qi++)
	{
	float query[16];
	for (d = 0; d < dim; d++) query[d] = (float)(rand() % 2000 - 1000) / 500.0f;
	unit_normalize(query, dim);

	long long exact[10];
	brute_force_topk(query, data, dim, n, ANT_vector_store::METRIC_DOT, k, exact);

	long long hg[10]; double sg[10];
	long long cg = graph.search(query, ANT_vector_store::METRIC_DOT, 64, k, pstore, &stones, hg, sg, NULL);

	long long i2, j2;
	for (i2 = 0; i2 < cg; i2++)
		for (j2 = 0; j2 < k; j2++)
			if (hg[i2] == exact[j2]) { overlap++; break; }
	total += k;
	}

double recall = (double)overlap / (double)total;
CHECK(recall >= 0.8);

delete pstore; delete fstore; delete [] data;
unlink(fpath); unlink(ppath);
printf("test_pq_backed_hnsw_recall OK (recall=%.3f, m=%lld)\n", recall, m);
}

int main(void)
{
test_pq_backed_hnsw_recall();
printf("test_pq_hnsw PASSED\n");
return 0;
}

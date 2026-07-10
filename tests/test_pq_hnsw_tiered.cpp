#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/pq_store.h"
#include "../source/pq_codec.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)

static const char *DIR = "/tmp/test_pq_hnsw_tiered_idx";

static void test_score_stack_cap(void)
{
	/* dim 64, m 32 -> m*K = 8192 doubles = 64KB table, above the stack cap -> heap path. */
	long long dim = 64, m = 32, n = 20, i, d;
	const char *path = "/tmp/test_pq_hnsw_tiered.pq";
	remove(path);
	float *data = new float[n*dim];
	srand(3); for (i = 0; i < n*dim; i++) data[i] = (float)(rand()%200-100)/100.0f;
	ANT_pq_store_writer w;
	CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT) == 0);
	for (d = 0; d < n; d++) CHECK(w.append(data + d*dim) == 0);
	CHECK(w.finish() == 0);
	ANT_pq_store *pq = ANT_pq_store::load(path, dim, n, ANT_pq_codec::METRIC_DOT);
	CHECK(pq->document_count() == n);

	float q[64]; for (i = 0; i < dim; i++) q[i] = (float)(rand()%200-100)/100.0f;
	double *table = new double[m*256];
	ANT_pq_codec::adc_table(q, dim, m, pq->get_codebook(), ANT_pq_codec::METRIC_DOT, table);
	double ref = ANT_pq_codec::adc_score(pq->codes_for(0), m, table);
	double got = pq->score(0, q, ANT_pq_codec::METRIC_DOT);
	CHECK(fabs(got - ref) < 1e-6);		/* heap-path score == brute force */
	delete [] table; delete pq; delete [] data;
	remove(path);
}

int main(void)
{
	test_score_stack_cap();
	printf("test_pq_hnsw_tiered PASSED\n");
	return 0;
}

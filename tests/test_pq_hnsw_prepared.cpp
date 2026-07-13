#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../source/pq_store.h"
#include "../source/pq_codec.h"
#include "../source/vector_store.h"
#include "../source/hnsw.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)

/* Build a PQ store of n random dim-vectors with m subspaces; caller deletes + removes path. */
static ANT_pq_store *make_pq(long long dim, long long m, long long n, long metric, const char *path)
{
	remove(path);
	float *data = new float[n*dim];
	srand(7); for (long long i=0;i<n*dim;i++) data[i]=(float)(rand()%200-100)/100.0f;
	ANT_pq_store_writer w;
	CHECK(w.create(path, dim, m, metric, 0) == 0);
	for (long long d=0; d<n; d++) CHECK(w.append(data + d*dim) == 0);
	CHECK(w.finish() == 0);
	delete [] data;
	ANT_pq_store *pq = ANT_pq_store::load(path, dim, n, metric);
	CHECK(pq != 0 && pq->document_count() == n);
	return pq;
}

/* prepared score == per-call score, and the NULL-ctx fallback == score(). */
static void test_prepared_equivalence(void)
{
	const char *path = "/tmp/test_pq_prepared_eq.pq";
	long long dim=32, m=8, n=60;
	ANT_pq_store *pq = make_pq(dim, m, n, ANT_pq_codec::METRIC_COSINE, path);
	float q[32]; for (int i=0;i<dim;i++) q[i]=(float)(rand()%200-100)/100.0f;

	void *ctx = pq->prepare_query(q, ANT_pq_codec::METRIC_COSINE);
	CHECK(ctx != 0);
	for (long long d=0; d<n; d+=7)
		{
		double prep  = pq->score_prepared(d, q, ANT_pq_codec::METRIC_COSINE, ctx);
		double plain = pq->score(d, q, ANT_pq_codec::METRIC_COSINE);
		CHECK(fabs(prep - plain) < 1e-9);		/* prepared table gives identical math */
		}
	CHECK(fabs(pq->score_prepared(3, q, ANT_pq_codec::METRIC_COSINE, NULL)
	         - pq->score(3, q, ANT_pq_codec::METRIC_COSINE)) < 1e-9);	/* NULL ctx -> fallback */
	pq->free_query(ctx);
	pq->free_query(NULL);		/* delete[] NULL: no-op, clean under ASan */
	delete pq; remove(path);
	printf("test_prepared_equivalence PASSED\n");
}

/* adc_table_builds: +1 per prepare_query, +1 per score(), +0 per prepared reuse. */
static void test_counter_contract(void)
{
	const char *path = "/tmp/test_pq_prepared_ct.pq";
	long long dim=32, m=8, n=30;
	ANT_pq_store *pq = make_pq(dim, m, n, ANT_pq_codec::METRIC_COSINE, path);
	float q[32]; for (int i=0;i<dim;i++) q[i]=(float)(rand()%200-100)/100.0f;

	long long b = pq->adc_table_builds;
	for (long long d=0; d<10; d++) pq->score(d, q, ANT_pq_codec::METRIC_COSINE);
	CHECK(pq->adc_table_builds - b == 10);		/* each score() builds the table once */

	long long b2 = pq->adc_table_builds;
	void *ctx = pq->prepare_query(q, ANT_pq_codec::METRIC_COSINE);
	CHECK(pq->adc_table_builds - b2 == 1);		/* prepare builds exactly once */
	for (long long d=0; d<10; d++) pq->score_prepared(d, q, ANT_pq_codec::METRIC_COSINE, ctx);
	CHECK(pq->adc_table_builds - b2 == 1);		/* prepared reuse: no new builds */
	pq->free_query(ctx);
	delete pq; remove(path);
	printf("test_counter_contract PASSED\n");
}

/* A non-overriding source (ANT_vector_store) inherits the no-op default. */
static void test_default_source_noop(void)
{
	const char *path = "/tmp/test_pq_prepared_def.vec";
	long long dim=16, n=20;
	remove(path);
	float *data = new float[n*dim];
	srand(11); for (long long i=0;i<n*dim;i++) data[i]=(float)(rand()%200-100)/100.0f;
	ANT_vector_store_writer w;
	CHECK(w.create(path, dim) == 0);
	for (long long d=0; d<n; d++) CHECK(w.append(data + d*dim) == 0);
	CHECK(w.finish() == 0);
	ANT_vector_store *vs = ANT_vector_store::load(path, dim, n);
	CHECK(vs != 0);
	float q[16]; for (int i=0;i<dim;i++) q[i]=data[i];		/* query == doc 0 */
	void *ctx = vs->prepare_query(q, ANT_vector_store::METRIC_COSINE);
	CHECK(ctx == 0);		/* default: no per-query structure */
	CHECK(fabs(vs->score_prepared(2, q, ANT_vector_store::METRIC_COSINE, ctx)
	         - vs->score(2, q, ANT_vector_store::METRIC_COSINE)) < 1e-9);
	vs->free_query(ctx);	/* no-op on NULL */
	delete vs; delete [] data; remove(path);
	printf("test_default_source_noop PASSED\n");
}

/* End-to-end: one search over a PQ-code graph builds the ADC table exactly ONCE
   (not per visited node), and the prepared-path result recalls the true ADC argmax. */
void test_hnsw_prepares_once(void)
{
	const char *path = "/tmp/test_pq_prepared_hnsw.pq";
	long long dim=32, m=8, n=60;
	ANT_pq_store *pq = make_pq(dim, m, n, ANT_pq_codec::METRIC_COSINE, path);
	ANT_hnsw graph;
	CHECK(graph.build(pq, 16, 100, ANT_pq_codec::METRIC_COSINE) == 0);

	float q[32]; for (int i=0;i<dim;i++) q[i]=(float)(rand()%200-100)/100.0f;
	long long ids[10]; double sc[10];
	long long before = pq->adc_table_builds;
	long long got = graph.search(q, ANT_pq_codec::METRIC_COSINE, 50, 10, pq, NULL, ids, sc, NULL);
	CHECK(got > 1);								/* traversal returned many nodes -> visited many */
	CHECK(pq->adc_table_builds - before == 1);	/* ADC table built ONCE per search, not per node */

	/* brute-force ADC argmax over all docs; assert the graph returned it among top-k */
	double *table = new double[m*256];
	ANT_pq_codec::adc_table(q, dim, m, pq->get_codebook(), ANT_pq_codec::METRIC_COSINE, table);
	long long best_d = -1; double best_s = -1e30;
	for (long long d=0; d<n; d++)
		{ double s = ANT_pq_codec::adc_score(pq->codes_for(d), m, table); if (s > best_s){best_s=s;best_d=d;} }
	delete [] table;
	int found = 0; for (long long h=0; h<got; h++) if (ids[h]==best_d) found=1;
	CHECK(found);								/* prepared-path search recalls the true ADC nearest */
	delete pq; remove(path);
	printf("test_hnsw_prepares_once PASSED\n");
}

int main(void)
{
	test_prepared_equivalence();
	test_counter_contract();
	test_default_source_noop();
	test_hnsw_prepares_once();
	printf("ALL test_pq_hnsw_prepared PASSED\n");
	return 0;
}

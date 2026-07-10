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

/* Build a dim-16 COSINE index at the given tier, 40 docs, flush, build_pq, build_hnsw. */
static ATIRE_segment_index *build_tier_hnsw(long tier, long posture, long long *gen_out)
{
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);	/* PRE-open */
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_hnsw_config(16, 100) == 0);				/* POST-open */
	CHECK(idx->set_pq_config(4, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_pq_resident_tier(tier) == 0);
	float v[16];
	for (int d = 0; d < 40; d++)
		{ char name[64]; snprintf(name,sizeof(name),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f; CHECK(idx->add_document(name,"body words here",v)>=0); }
	CHECK(idx->flush() == 0);
	*gen_out = idx->disk_segment_generation(0);
	CHECK(idx->build_pq() == 0);
	if (tier == ATIRE_segment_index::PQ_TIER_NONE)
		{ char vecp[2048]; snprintf(vecp,sizeof(vecp),"%s/seg_%06lld.vec",DIR,(long long)*gen_out); char bak[2100]; snprintf(bak,sizeof(bak),"%s.bak",vecp); rename(vecp,bak); }
	CHECK(idx->build_hnsw() == 0);
	if (tier == ATIRE_segment_index::PQ_TIER_NONE)
		{ char vecp[2048]; snprintf(vecp,sizeof(vecp),"%s/seg_%06lld.vec",DIR,(long long)*gen_out); char bak[2100]; snprintf(bak,sizeof(bak),"%s.bak",vecp); rename(bak,vecp); }	/* restore for later compaction */
	return idx;
}

static void test_build_hnsw_over_tier_source(void)
{
	long long gen;
	ATIRE_segment_index *n = build_tier_hnsw(ATIRE_segment_index::PQ_TIER_NONE, ATIRE_segment_index::PQ_POSTURE_REPLACE, &gen);
	CHECK(n->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_NONE);	/* no float resident */
	CHECK(n->disk_segment_has_pq(0) == 1);
	CHECK(n->disk_segment_has_hnsw(0) == 1);			/* graph built over pq_vectors (disk .vec was absent) */
	delete n;
	ATIRE_segment_index *i8 = build_tier_hnsw(ATIRE_segment_index::PQ_TIER_INT8, ATIRE_segment_index::PQ_POSTURE_RERANK, &gen);
	CHECK(i8->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_INT8);
	CHECK(i8->disk_segment_has_hnsw(0) == 1);			/* graph built over int8 .pqr */
	delete i8;
	ATIRE_segment_index *f = build_tier_hnsw(ATIRE_segment_index::PQ_TIER_FLOAT, ATIRE_segment_index::PQ_POSTURE_RERANK, &gen);
	CHECK(f->disk_segment_has_hnsw(0) == 1);
	delete f;
}

int main(void)
{
	test_score_stack_cap();
	test_build_hnsw_over_tier_source();
	printf("test_pq_hnsw_tiered PASSED\n");
	return 0;
}

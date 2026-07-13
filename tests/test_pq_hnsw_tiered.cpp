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
	CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT, 0) == 0);
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

static double recall10(ATIRE_segment_index *idx, const float *q, const long *planted, int np)
{
	long long n = idx->search_vector_hnsw(q, 10);
	int hit = 0;
	for (int i = 0; i < np; i++)
		{ char want[64]; snprintf(want,sizeof(want),"doc%ld",planted[i]);
		  for (long long h=0; h<n && h<10; h++) if (strcmp(idx->get_hit(h)->filename, want)==0){hit++;break;} }
	return (double)hit/np;
}

static void test_none_tier_hnsw_search(void)
{
	long long gen;
	ATIRE_segment_index *idx = build_tier_hnsw(ATIRE_segment_index::PQ_TIER_NONE, ATIRE_segment_index::PQ_POSTURE_REPLACE, &gen);
	CHECK(idx->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_NONE);
	CHECK(idx->disk_segment_has_hnsw(0) == 1);
	float q[16]; for (int j=0;j<16;j++) q[j]=(float)((7*7+j*3)%13-6)/6.0f;	/* near doc7 */
	long planted[1] = {7};
	long long n = idx->search_vector_hnsw(q, 10);
	CHECK(n >= 1);						/* graph engaged under NONE (no NULL-skip) */
	CHECK(recall10(idx, q, planted, 1) >= 1.0 - 1e-9);	/* planted nearest recalled via ADC graph */
	delete idx;
}

static void test_float_tier_hnsw_byte_identical(void)
{
	/* Non-PQ float HNSW reference vs PQ+FLOAT+HNSW over identical data -> identical top-k. */
	const char *DA = "/tmp/test_pqhnsw_float_a", *DB = "/tmp/test_pqhnsw_float_b";
	char cmd[4096]; snprintf(cmd,sizeof(cmd),"rm -rf %s %s && mkdir -p %s %s",DA,DB,DA,DB); system(cmd);
	ATIRE_segment_index *a = new ATIRE_segment_index();
	CHECK(a->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE)==0);
	CHECK(a->open(DA)==0); CHECK(a->set_hnsw_config(16,100)==0);
	ATIRE_segment_index *b = new ATIRE_segment_index();
	CHECK(b->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE)==0);
	CHECK(b->open(DB)==0); CHECK(b->set_hnsw_config(16,100)==0);
	CHECK(b->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0);
	CHECK(b->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_FLOAT)==0);
	float v[16];
	for (int d=0; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f;
		CHECK(a->add_document(nm,"body words here",v)>=0); CHECK(b->add_document(nm,"body words here",v)>=0); }
	CHECK(a->flush()==0); CHECK(a->build_hnsw()==0);
	CHECK(b->flush()==0); CHECK(b->build_pq()==0); CHECK(b->build_hnsw()==0);
	float q[16]; for (int j=0;j<16;j++) q[j]=(float)((5*7+j*3)%13-6)/6.0f;
	long long na = a->search_vector_hnsw(q,10), nb = b->search_vector_hnsw(q,10);
	CHECK(na == nb);
	for (long long i=0;i<na;i++)
		{ ATIRE_segment_index::hit *ha=a->get_hit(i), *hb=b->get_hit(i);
		  CHECK(strcmp(ha->filename, hb->filename)==0); CHECK(ha->docid==hb->docid); CHECK(ha->score==hb->score); }
	delete a; delete b;
}

static void test_compaction_hnsw_over_tier(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE)==0);
	CHECK(idx->open(DIR)==0); CHECK(idx->set_hnsw_config(16,100)==0);		/* set_hnsw_config POST-open */
	CHECK(idx->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_NONE)==0);
	float v[16];
	for (int d=0; d<20; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f; CHECK(idx->add_document(nm,"body words here",v)>=0);}
	CHECK(idx->flush()==0);
	for (int d=20; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f; CHECK(idx->add_document(nm,"body words here",v)>=0);}
	CHECK(idx->flush()==0);
	CHECK(idx->build_pq()==0); CHECK(idx->build_hnsw()==0);
	long long gens[2] = { idx->disk_segment_generation(0), idx->disk_segment_generation(1) };
	CHECK(idx->compact(gens, 2)==0);
	CHECK(idx->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_NONE);
	CHECK(idx->disk_segment_has_hnsw(0) == 1);					/* merged graph rebuilt over pq_vectors */
	float q[16]; for (int j=0;j<16;j++) q[j]=(float)((7*7+j*3)%13-6)/6.0f;
	long planted[1] = {7};
	CHECK(recall10(idx, q, planted, 1) >= 1.0 - 1e-9);			/* planted doc recalled after merge */
	delete idx;
}

static void fill_vec32(long long seed, float *v)
{
	unsigned long long s = seed*2654435761ULL+1;
	for (int j=0;j<32;j++)
		{ s = s*6364136223846793005ULL+1442695040888963407ULL; v[j] = (float)((double)((s>>33)&0x7fffffff)/(double)0x7fffffff*2.0-1.0); }
}

/* build a dim-32 index at `tier`/`posture` with 200 random docs + 3 planted near q (docs 50,120,190). */
static ATIRE_segment_index *build_recall_idx(long tier, long posture, const float *q)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(32, ATIRE_segment_index::VECTOR_METRIC_COSINE)==0);
	CHECK(idx->open(DIR)==0); CHECK(idx->set_hnsw_config(16,200)==0);
	CHECK(idx->set_pq_config(0, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0);	/* default m */
	CHECK(idx->set_pq_resident_tier(tier)==0);
	float v[32];
	for (int d=0; d<200; d++)
		{
		char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d);
		if (d==50||d==120||d==190){ for(int j=0;j<32;j++) v[j]=q[j]+0.001f*((d%3)-1); }
		else fill_vec32(d, v);
		CHECK(idx->add_document(nm,"body words here",v)>=0);
		}
	CHECK(idx->flush()==0); CHECK(idx->build_pq()==0); CHECK(idx->build_hnsw()==0);
	return idx;
}

static void test_cross_tier_recall(void)
{
	float q[32]; fill_vec32(999999, q);
	long planted[3] = {50,120,190};
	ATIRE_segment_index *f = build_recall_idx(ATIRE_segment_index::PQ_TIER_FLOAT, ATIRE_segment_index::PQ_POSTURE_RERANK, q);
	double rf = recall10(f, q, planted, 3); delete f;
	ATIRE_segment_index *i8 = build_recall_idx(ATIRE_segment_index::PQ_TIER_INT8, ATIRE_segment_index::PQ_POSTURE_RERANK, q);
	double ri = recall10(i8, q, planted, 3); delete i8;
	ATIRE_segment_index *n = build_recall_idx(ATIRE_segment_index::PQ_TIER_NONE, ATIRE_segment_index::PQ_POSTURE_REPLACE, q);
	double rn = recall10(n, q, planted, 3); delete n;
	printf("hnsw recall: float=%.3f int8=%.3f none=%.3f\n", rf, ri, rn);
	CHECK(rf >= 1.0 - 1e-9);
	CHECK(rn >= 0.8);
	CHECK(ri >= rn - 1e-9);
	CHECK(ri <= rf + 1e-9);
}

/*
	Holistic-review Important: a FLOAT->NONE tier tightening after the graph was
	built over float must invalidate the stale float-geometry .hnsw, so the next
	build_hnsw() rebuilds it over pq_vectors (build-source == search-source).
*/
static void test_tier_change_invalidates_stale_graph(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE)==0);
	CHECK(idx->open(DIR)==0); CHECK(idx->set_hnsw_config(16,100)==0);
	CHECK(idx->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0);
	/* tier defaults FLOAT */
	float v[16];
	for (int d=0; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f; CHECK(idx->add_document(nm,"body words here",v)>=0);}
	CHECK(idx->flush()==0); CHECK(idx->build_pq()==0); CHECK(idx->build_hnsw()==0);
	CHECK(idx->disk_segment_has_hnsw(0)==1);				/* FLOAT-built graph present */

	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_NONE)==0);	/* tighten FLOAT->NONE */
	CHECK(idx->disk_segment_has_hnsw(0)==0);				/* stale float graph invalidated */

	CHECK(idx->build_pq()==0); CHECK(idx->build_hnsw()==0);	/* rebuild over pq_vectors */
	CHECK(idx->disk_segment_resident_tier(0)==ATIRE_segment_index::PQ_TIER_NONE);
	CHECK(idx->disk_segment_has_hnsw(0)==1);				/* rebuilt over the NONE tier source */
	float q[16]; for (int j=0;j<16;j++) q[j]=(float)((7*7+j*3)%13-6)/6.0f;
	long planted[1] = {7};
	CHECK(recall10(idx, q, planted, 1) >= 1.0 - 1e-9);		/* correct after rebuild */
	delete idx;
}

int main(void)
{
	test_score_stack_cap();
	test_build_hnsw_over_tier_source();
	test_none_tier_hnsw_search();
	test_float_tier_hnsw_byte_identical();
	test_compaction_hnsw_over_tier();
	test_cross_tier_recall();
	test_tier_change_invalidates_stale_graph();
	printf("test_pq_hnsw_tiered PASSED\n");
	return 0;
}

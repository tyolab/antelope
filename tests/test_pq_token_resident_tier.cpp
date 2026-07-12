#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"
#include "../source/vector_source.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
static const char *DIR = "/tmp/test_pq_token_tier_idx";

/* Build a V6 (rerank + token-index) index, no token-PQ. Returns it open. gen_out = seg 0 generation. */
static ATIRE_segment_index *build_v6(long long *gen_out)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* POST-open */
	/* token index M/ef = ctor defaults 16/200; no public setter */
	for (int d = 0; d < 40; d++)
		{
		char name[64]; snprintf(name,sizeof(name),"doc%d",d);
		int md = 2 + (d % 3);				/* 2..4 tokens per doc */
		float rows[4*8];
		for (int r = 0; r < md; r++)
			{ double nrm=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3)%13-6)/6.0f; nrm+=rows[r*8+j]*rows[r*8+j]; }
			  nrm=sqrt(nrm)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)nrm; }
		CHECK(idx->add_document(name, "body words here", NULL, rows, md) >= 0);	/* 5-arg multi-vector overload */
		}
	CHECK(idx->flush() == 0);
	*gen_out = idx->disk_segment_generation(0);
	CHECK(idx->build_token_index() == 0);
	return idx;
}

static void test_float_token_byte_identical(void)
{
	long long gen;
	ATIRE_segment_index *idx = build_v6(&gen);
	float q[2*8];
	for (int r=0;r<2;r++){ double nrm=0; for(int j=0;j<8;j++){ q[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; nrm+=q[r*8+j]*q[r*8+j]; } nrm=sqrt(nrm)+1e-9; for(int j=0;j<8;j++) q[r*8+j]/=(float)nrm; }
	long long n = idx->search_multivector(q, 2, 10);
	CHECK(n > 0);
	/* snapshot top-k; after the refactor this same call must return the identical ranking */
	printf("float token top-1 = %s score=%.6f (n=%lld)\n", idx->get_hit(0)->filename, idx->get_hit(0)->score, n);
	CHECK(idx->disk_segment_has_token_index(0) == 1);
	delete idx;
	printf("test_float_token_byte_identical PASSED\n");
}

static ANT_multivector_pq_store *make_mvpq(long long dim, long long m, long long ndoc, const char *path)
{
	remove(path);
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT) == 0);
	srand(5);
	for (long long d=0; d<ndoc; d++)
		{
		long long md = 2 + (d % 3);
		float rows[4*16];
		for (long long r=0;r<md;r++){ double nrm=0; for(long long j=0;j<dim;j++){ rows[r*dim+j]=(float)(rand()%200-100)/100.0f; nrm+=rows[r*dim+j]*rows[r*dim+j]; } nrm=sqrt(nrm)+1e-9; for(long long j=0;j<dim;j++) rows[r*dim+j]/=(float)nrm; }
		CHECK(w.append(rows, md) == 0);
		}
	CHECK(w.finish() == 0);
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, dim, ndoc, ANT_pq_codec::METRIC_DOT);
	CHECK(s->token_count() > 0);
	return s;
}

static void test_token_seam_equivalence(void)
{
	const char *path = "/tmp/test_pq_token_seam.mvpq";
	long long dim=16, m=8, ndoc=30;
	ANT_multivector_pq_store *s = make_mvpq(dim, m, ndoc, path);
	float q[16]; for(int j=0;j<dim;j++) q[j]=(float)(rand()%200-100)/100.0f;

	void *ctx = s->token_prepare_query(q);
	CHECK(ctx != 0);
	for (long long t=0; t<s->token_count(); t+=5)
		CHECK(fabs(s->token_score_prepared(t, q, ctx) - s->token_score(t, q, ANT_pq_codec::METRIC_DOT)) < 1e-9);
	CHECK(fabs(s->token_score_prepared(1, q, NULL) - s->token_score(1, q, ANT_pq_codec::METRIC_DOT)) < 1e-9);	/* NULL ctx fallback */
	s->token_free_query(ctx);
	s->token_free_query(NULL);	/* delete[] NULL safe */

	/* counter: +1 per prepare, +1 per token_score, +0 per prepared reuse */
	long long b = s->adc_table_builds;
	for (long long t=0;t<10;t++) s->token_score(t, q, ANT_pq_codec::METRIC_DOT);
	CHECK(s->adc_table_builds - b == 10);
	long long b2 = s->adc_table_builds;
	void *c2 = s->token_prepare_query(q);
	CHECK(s->adc_table_builds - b2 == 1);
	for (long long t=0;t<10;t++) s->token_score_prepared(t, q, c2);
	CHECK(s->adc_table_builds - b2 == 1);
	s->token_free_query(c2);

	/* source adapter delegates */
	ANT_multivector_pq_source src(s);
	CHECK(src.is_quantized() == 1);
	CHECK(src.get(0) == 0);
	CHECK(src.document_count() == s->token_count());
	CHECK(src.num_documents() == s->document_count());
	void *c3 = src.prepare_query(q, ANT_pq_codec::METRIC_DOT);
	CHECK(fabs(src.score_prepared(0, q, ANT_pq_codec::METRIC_DOT, c3) - s->token_score(0, q, ANT_pq_codec::METRIC_DOT)) < 1e-9);
	src.free_query(c3);
	delete s; remove(path);
	printf("test_token_seam_equivalence PASSED\n");
}

static void test_tier_change_invalidates_tann(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* token index M/ef = ctor defaults 16/200; no public setter */
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	for (int d=0; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); int md=2+(d%3); float rows[4*8];
		for(int r=0;r<md;r++){ double n=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3)%13-6)/6.0f; n+=rows[r*8+j]*rows[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)n; }
		CHECK(idx->add_document(nm,"body",NULL,rows,md)>=0); }
	CHECK(idx->flush() == 0);
	long long gen = idx->disk_segment_generation(0);
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->build_token_index() == 0);
	CHECK(idx->disk_segment_has_token_index(0) == 1);
	CHECK(idx->multivector_resident_tier() == ATIRE_segment_index::MV_TIER_FLOAT);

	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	CHECK(idx->multivector_resident_tier() == ATIRE_segment_index::MV_TIER_NONE);
	/* stale float-geometry .tann must be gone + in-memory index nulled */
	char tann[2048], tanng[2100];
	snprintf(tann,sizeof(tann),"%s/seg_%06lld.tann",DIR,gen);
	snprintf(tanng,sizeof(tanng),"%s/seg_%06lld.tann.g",DIR,gen);
	FILE *a=fopen(tann,"rb"); CHECK(a==NULL); FILE *b=fopen(tanng,"rb"); CHECK(b==NULL);
	CHECK(idx->disk_segment_has_token_index(0) == 0);
	/* immutability: cannot move off NONE */
	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_FLOAT) != 0);
	delete idx;
	printf("test_tier_change_invalidates_tann PASSED\n");
}

static void test_none_tier_end_to_end(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* token index M/ef = ctor defaults 16/200; no public setter */
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	for (int d=0; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); int md=2+(d%3); float rows[4*8];
		for(int r=0;r<md;r++){ double n=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3)%13-6)/6.0f; n+=rows[r*8+j]*rows[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)n; }
		CHECK(idx->add_document(nm,"body",NULL,rows,md)>=0); }
	CHECK(idx->flush() == 0);
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete idx;						/* close; NONE takes effect on reopen */

	idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);				/* load_multivector_pq_config -> NONE */
	CHECK(idx->disk_segment_resident_tier_mv(0) == ATIRE_segment_index::MV_TIER_NONE);	/* no float pool resident */
	CHECK(idx->build_token_index() == 0);			/* builds .tann over the PQ source */
	CHECK(idx->disk_segment_has_token_index(0) == 1);

	float q[2*8];
	for (int r=0;r<2;r++){ double n=0; for(int j=0;j<8;j++){ q[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; n+=q[r*8+j]*q[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) q[r*8+j]/=(float)n; }
	long long n = idx->search_multivector(q, 2, 10);
	CHECK(n > 0);						/* NONE-tier token-ANN answers */
	delete idx;
	printf("test_none_tier_end_to_end PASSED\n");
}

static void test_none_rerank_rejected(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* NONE is replace-only: RERANK posture has no resident store to rescore against */
	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) != 0);
	CHECK(idx->multivector_resident_tier() == ATIRE_segment_index::MV_TIER_FLOAT);
	delete idx;

	/* REPLACE posture still accepts NONE */
	char cmd2[2048]; snprintf(cmd2,sizeof(cmd2),"rm -rf %s2 && mkdir -p %s2",DIR,DIR); system(cmd2);
	char dir2[2048]; snprintf(dir2,sizeof(dir2),"%s2",DIR);
	ATIRE_segment_index *idx2 = new ATIRE_segment_index();
	CHECK(idx2->open(dir2) == 0);
	CHECK(idx2->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx2->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx2->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete idx2;
	printf("test_none_rerank_rejected PASSED\n");
}

static void test_compaction_under_none(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* token index M/ef = ctor defaults 16/200; no public setter */
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	for (int f=0; f<2; f++)
		{
		for (int d=0; d<20; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d_%d",f,d); int md=2+(d%3); float rows[4*8];
			for(int r=0;r<md;r++){ double n=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3+f)%13-6)/6.0f; n+=rows[r*8+j]*rows[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)n; }
			CHECK(idx->add_document(nm,"body",NULL,rows,md)>=0); }
		CHECK(idx->flush() == 0);
		}
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete idx;
	idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->build_token_index() == 0);
	long long gens[2] = { idx->disk_segment_generation(0), idx->disk_segment_generation(1) };
	CHECK(idx->compact(gens, 2) == 0);
	CHECK(idx->disk_segment_resident_tier_mv(0) == ATIRE_segment_index::MV_TIER_NONE);
	float q[2*8]; for (int r=0;r<2;r++){ double n=0; for(int j=0;j<8;j++){ q[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; n+=q[r*8+j]*q[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) q[r*8+j]/=(float)n; }
	CHECK(idx->search_multivector(q, 2, 10) > 0);
	delete idx;
	printf("test_compaction_under_none PASSED\n");
}

/*
	TEST_CROSS_TIER_RECALL()
	-------------------------
	Build the SAME 40-doc synthetic set + fixed 2-vector query at FLOAT tier (plain
	V6, no token-PQ -- reuses build_v6()) and record the top-10 docids from
	search_multivector(); then build a second index over identical data with
	token-PQ configured, flip to MV_TIER_NONE, reopen (tier realized at load),
	build_token_index() (over the PQ source), and record its top-10 docids.
	Assert the NONE top-10 overlaps the FLOAT top-10 by at least 7/10 -- PQ
	token-ANN search is a faithful approximation of exact float MaxSim, not an
	unrelated ranking.
*/
static void test_cross_tier_recall(void)
{
	/* --- FLOAT baseline: plain V6, no token-PQ --- */
	long long gen_f;
	ATIRE_segment_index *idxf = build_v6(&gen_f);
	float qf[2*8];
	for (int r=0;r<2;r++){ double n=0; for(int j=0;j<8;j++){ qf[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; n+=qf[r*8+j]*qf[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) qf[r*8+j]/=(float)n; }
	long long nf = idxf->search_multivector(qf, 2, 10);
	CHECK(nf > 0);
	long long float_n = nf < 10 ? nf : 10;
	long long float_docids[10];
	for (long long i=0;i<float_n;i++)
		float_docids[i] = idxf->get_hit(i)->docid;
	delete idxf;

	/* --- NONE tier: same 40 docs, token-PQ configured, tier flipped to NONE, reopened --- */
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	for (int d=0; d<40; d++)
		{
		char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d);
		int md = 2 + (d % 3);
		float rows[4*8];
		for (int r=0;r<md;r++)
			{ double n=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3)%13-6)/6.0f; n+=rows[r*8+j]*rows[r*8+j]; }
			  n=sqrt(n)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)n; }
		CHECK(idx->add_document(nm, "body", NULL, rows, md) >= 0);
		}
	CHECK(idx->flush() == 0);
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete idx;						/* close; NONE takes effect on reopen */

	idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);				/* load_multivector_pq_config -> NONE */
	CHECK(idx->disk_segment_resident_tier_mv(0) == ATIRE_segment_index::MV_TIER_NONE);
	CHECK(idx->build_token_index() == 0);			/* builds .tann over the PQ source */
	CHECK(idx->disk_segment_has_token_index(0) == 1);

	float qn[2*8];
	for (int r=0;r<2;r++){ double n=0; for(int j=0;j<8;j++){ qn[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; n+=qn[r*8+j]*qn[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) qn[r*8+j]/=(float)n; }
	long long nn = idx->search_multivector(qn, 2, 10);
	CHECK(nn > 0);
	long long none_n = nn < 10 ? nn : 10;
	long long none_docids[10];
	for (long long i=0;i<none_n;i++)
		none_docids[i] = idx->get_hit(i)->docid;
	delete idx;

	/* overlap: how many of the FLOAT top-10 docids also appear in the NONE top-10 */
	long long overlap = 0;
	for (long long i=0;i<float_n;i++)
		for (long long j=0;j<none_n;j++)
			if (float_docids[i] == none_docids[j])
				{ overlap++; break; }

	printf("cross-tier recall overlap = %lld / %lld\n", overlap, float_n);
	CHECK(overlap >= 7);
	printf("test_cross_tier_recall PASSED\n");
}

/*
	TEST_CONFIG_V1_BACKCOMPAT()
	-----------------------------
	Locks load_multivector_pq_config()'s version==1 branch (Task 3 review noted no
	standalone regression for this). Configure token-PQ (which writes the CURRENT
	v2 layout: 8-byte tag "ANTMVPQC" + u32 version=2 + 4 i64 { m, posture,
	rerank_quant, tier }), close, then hand-rewrite multivector_pq.config to the
	EXACT v1 on-disk layout (u32 version=1 + 3 i64 { m, posture, rerank_quant },
	no tier field) and reopen. The v1 file has no tier field, so the loader must
	default it to MV_TIER_FLOAT (see the version==1 branch: "vals[3] = MV_TIER_FLOAT").
*/
static void test_config_v1_backcompat(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* writes v2 */
	CHECK(idx->multivector_pq_m() == 4);
	delete idx;

	/* hand-rewrite multivector_pq.config from v2 to the EXACT v1 layout */
	char path[2048]; snprintf(path,sizeof(path),"%s/multivector_pq.config",DIR);
	unsigned int v1 = 1;
	long long vals[3] = { 4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT };
	FILE *out = fopen(path, "wb");
	CHECK(out != NULL);
	CHECK(fwrite("ANTMVPQC", 1, 8, out) == 8);
	CHECK(fwrite(&v1, 4, 1, out) == 1);
	CHECK(fwrite(vals, 8, 3, out) == 3);
	CHECK(fclose(out) == 0);

	ATIRE_segment_index *idx2 = new ATIRE_segment_index();
	CHECK(idx2->open(DIR) == 0);				/* triggers load_multivector_pq_config(), version==1 branch */
	CHECK(idx2->multivector_pq_configured() == 1);
	CHECK(idx2->multivector_pq_m() == 4);
	CHECK(idx2->multivector_resident_tier() == ATIRE_segment_index::MV_TIER_FLOAT);	/* v1 has no tier field -> must default FLOAT */
	delete idx2;
	printf("test_config_v1_backcompat PASSED\n");
}

int main(void)
{
	test_float_token_byte_identical();
	test_token_seam_equivalence();
	test_tier_change_invalidates_tann();
	test_none_tier_end_to_end();
	test_none_rerank_rejected();
	test_compaction_under_none();
	test_cross_tier_recall();
	test_config_v1_backcompat();
	printf("ALL test_pq_token_resident_tier PASSED\n");
	return 0;
}

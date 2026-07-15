/*
	TEST_MVPQ_OPQ_CONFIG.CPP -- set_multivector_pq_opq persists to
	multivector_pq.config v3, reopen restores it, immutability + idempotence
	hold, a v2 config (no opq) loads as 0, and build under OPQ works.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
static const char *DIR = "/tmp/test_mvpq_opq_config_idx";

/* fresh empty index dir */
static void make_dir(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
}

/* add 40 normalized multi-vector docs to an open, rerank-configured index */
static void add_docs(ATIRE_segment_index *idx)
{
	for (int d = 0; d < 40; d++)
		{
		char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d);
		int md = 2 + (d % 3);
		float rows[4*8];
		for (int r=0;r<md;r++)
			{ double n=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3)%13-6)/6.0f; n+=rows[r*8+j]*rows[r*8+j]; }
			  n=sqrt(n)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)n; }
		CHECK(idx->add_document(nm,"body words here",NULL,rows,md) >= 0);
		}
	CHECK(idx->flush() == 0);
}

static void test_opq_config_roundtrip(void)
{
	make_dir();
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->multivector_pq_opq() == 0);				/* default off */
	CHECK(idx->set_multivector_pq_opq(1) == 0);
	CHECK(idx->multivector_pq_opq() == 1);
	add_docs(idx);
	CHECK(idx->build_multivector_pq() == 0);			/* trains R under OPQ */
	CHECK(idx->build_token_index() == 0);
	float q[2*8];
	for (int r=0;r<2;r++){ double n=0; for(int j=0;j<8;j++){ q[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; n+=q[r*8+j]*q[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) q[r*8+j]/=(float)n; }
	CHECK(idx->search_multivector(q, 2, 10) > 0);
	delete idx;

	/* reopen -> multivector_pq.config v3 restores opq */
	idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->multivector_pq_configured() == 1);
	CHECK(idx->multivector_pq_opq() == 1);				/* persisted v3 */
	CHECK(idx->build_token_index() == 0);
	CHECK(idx->search_multivector(q, 2, 10) > 0);
	delete idx;
	printf("test_opq_config_roundtrip PASSED\n");
}

static void test_opq_immutable_idempotent(void)
{
	make_dir();
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* requires token-PQ configured */
	CHECK(idx->set_multivector_pq_opq(1) != 0);
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* idempotent same-value while off */
	CHECK(idx->set_multivector_pq_opq(0) == 0);
	CHECK(idx->multivector_pq_opq() == 0);
	/* enable, then idempotent same-value while on */
	CHECK(idx->set_multivector_pq_opq(1) == 0);
	CHECK(idx->set_multivector_pq_opq(1) == 0);
	CHECK(idx->multivector_pq_opq() == 1);
	/* immutable once on: cannot flip back off */
	CHECK(idx->set_multivector_pq_opq(0) != 0);
	CHECK(idx->multivector_pq_opq() == 1);
	delete idx;
	printf("test_opq_immutable_idempotent PASSED\n");
}

static void test_v2_config_loads_opq_zero(void)
{
	make_dir();
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	delete idx;

	/* hand-rewrite multivector_pq.config to the EXACT v2 layout (4 vals, no opq) */
	char path[2048]; snprintf(path,sizeof(path),"%s/multivector_pq.config",DIR);
	unsigned int v2 = 2;
	long long vals[4] = { 4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT, ATIRE_segment_index::MV_TIER_FLOAT };
	FILE *out = fopen(path, "wb");
	CHECK(out != NULL);
	CHECK(fwrite("ANTMVPQC", 1, 8, out) == 8);
	CHECK(fwrite(&v2, 4, 1, out) == 1);
	CHECK(fwrite(vals, 8, 4, out) == 4);
	CHECK(fclose(out) == 0);

	idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);					/* load_multivector_pq_config, version==2 branch */
	CHECK(idx->multivector_pq_configured() == 1);
	CHECK(idx->multivector_pq_m() == 4);
	CHECK(idx->multivector_pq_opq() == 0);				/* v2 has no opq -> default 0 */
	delete idx;
	printf("test_v2_config_loads_opq_zero PASSED\n");
}

int main(void)
{
	test_opq_config_roundtrip();
	test_opq_immutable_idempotent();
	test_v2_config_loads_opq_zero();
	printf("ALL test_mvpq_opq_config PASSED\n");
	return 0;
}

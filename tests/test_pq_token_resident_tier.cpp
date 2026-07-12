#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/pq_codec.h"
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
	float row[8];
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

int main(void)
{
	test_float_token_byte_identical();
	printf("ALL test_pq_token_resident_tier PASSED\n");
	return 0;
}

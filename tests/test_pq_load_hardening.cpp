#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/pq_codec.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
static const char *DIR = "/tmp/test_pq_load_hardening_idx";

static void reset_dir(void){ char c[2048]; snprintf(c,sizeof(c),"rm -rf %s && mkdir -p %s",DIR,DIR); system(c); }

/* Overwrite pq.config with a chosen (possibly invalid) v2 record. */
static void write_pq_config(long long m, long long posture, long long rq, long long tier)
{
	char path[4096]; snprintf(path,sizeof(path),"%s/pq.config",DIR);
	FILE *f = fopen(path,"wb"); CHECK(f != NULL);
	unsigned long long magic; memcpy(&magic,"ANTPQCF1",8);
	unsigned int version = 2;
	CHECK(fwrite(&magic,8,1,f)==1 && fwrite(&version,4,1,f)==1
		&& fwrite(&m,8,1,f)==1 && fwrite(&posture,8,1,f)==1 && fwrite(&rq,8,1,f)==1 && fwrite(&tier,8,1,f)==1);
	fclose(f);
}

/* Overwrite multivector_pq.config with a chosen (possibly invalid) v2 record. */
static void write_mvpq_config(long long m, long long posture, long long rq, long long tier)
{
	char path[4096]; snprintf(path,sizeof(path),"%s/multivector_pq.config",DIR);
	FILE *f = fopen(path,"wb"); CHECK(f != NULL);
	unsigned int version = 2;
	long long vals[4] = { m, posture, rq, tier };
	CHECK(fwrite("ANTMVPQC",1,8,f)==8 && fwrite(&version,4,1,f)==1 && fwrite(vals,8,4,f)==4);
	fclose(f);
}

/* dense: a config whose m does not divide the vector dimension leaves PQ unconfigured on reopen */
static void test_dense_config_bad_m_rejected(void)
{
	reset_dir();
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);	/* PRE-open */
	CHECK(ix->open(DIR) == 0);
	float v[16]; for(int j=0;j<16;j++) v[j]=(float)((j*3)%7-3)/3.0f;
	CHECK(ix->add_document("d0","body",v) >= 0);
	CHECK(ix->flush() == 0);					/* writes vector.config on disk */
	delete ix;
	write_pq_config(3, 0, 0, 0);					/* 16 % 3 != 0 -> must be rejected */
	ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->pq_configured() == 0);				/* bad-m config left PQ unconfigured */
	delete ix;
	printf("test_dense_config_bad_m_rejected PASSED\n");
}

/* dense: out-of-range posture rejected */
static void test_dense_config_bad_posture_rejected(void)
{
	reset_dir();
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	float v[16]; for(int j=0;j<16;j++) v[j]=(float)((j*3)%7-3)/3.0f;
	CHECK(ix->add_document("d0","body",v) >= 0);
	CHECK(ix->flush() == 0);
	delete ix;
	write_pq_config(4, 7, 0, 0);					/* posture 7 out of {0,1} */
	ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->pq_configured() == 0);
	delete ix;
	printf("test_dense_config_bad_posture_rejected PASSED\n");
}

/* token: a config whose m does not divide the rerank dimension leaves token-PQ unconfigured on reopen */
static void test_token_config_bad_m_rejected(void)
{
	reset_dir();
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* POST-open; writes rerank config */
	float rows[2*16]; for(int r=0;r<2;r++){ double n=0; for(int j=0;j<16;j++){ rows[r*16+j]=(float)((r*5+j*3)%7-3)/3.0f; n+=rows[r*16+j]*rows[r*16+j]; } n=sqrt(n)+1e-9; for(int j=0;j<16;j++) rows[r*16+j]/=(float)n; }
	CHECK(ix->add_document("d0","body",NULL,rows,2) >= 0);
	CHECK(ix->flush() == 0);
	delete ix;
	write_mvpq_config(3, 0, 0, 0);					/* 16 % 3 != 0 -> rejected */
	ix = new ATIRE_segment_index();
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->multivector_pq_configured() == 0);			/* bad-m token config left token-PQ unconfigured */
	delete ix;
	printf("test_token_config_bad_m_rejected PASSED\n");
}

/* regression: a VALID config still loads and reports configured (guard doesn't reject good files) */
static void test_valid_config_still_loads(void)
{
	reset_dir();
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	float v[16]; for(int j=0;j<16;j++) v[j]=(float)((j*3)%7-3)/3.0f;
	CHECK(ix->add_document("d0","body",v) >= 0);
	CHECK(ix->flush() == 0);
	delete ix;
	ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->pq_configured() == 1);				/* valid m=4 | dim=16 loads fine */
	delete ix;
	printf("test_valid_config_still_loads PASSED\n");
}

int main(void)
{
	test_dense_config_bad_m_rejected();
	test_dense_config_bad_posture_rejected();
	test_token_config_bad_m_rejected();
	test_valid_config_still_loads();
	printf("ALL test_pq_load_hardening PASSED\n");
	return 0;
}

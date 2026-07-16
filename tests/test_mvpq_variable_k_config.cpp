/*
	TEST_MVPQ_VARIABLE_K_CONFIG.CPP -- token epic 3/4 Task 2: engine set_multivector_pq_k.
	set_multivector_pq_k validates power-of-two, is immutable-once, persists in
	multivector_pq.config v5 (survives reopen), and a config without the k field
	loads as k=256. A build under k=16 writes a v3 .mvpq.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define RD 8
#define MM 4

static char *mkdir_tmp(const char *tmpl)
{ char b[64]; strcpy(b, tmpl); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r, d); return r; }

static void fill(long long seed, float *v)
{ double n=0; for (int j=0;j<RD;j++){v[j]=(float)(((seed*7+j*3)%13)-6)/6.0f;n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<RD;j++)v[j]/=(float)n; }

static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{
	for (long long i = lo; i < hi; i++)
		{ float rows[3*RD]; for (int r=0;r<3;r++) fill(i*5+r, rows+r*RD);
		  char key[32]; snprintf(key,sizeof(key),"d-%lld",i);
		  CHECK(ix->add_document(key, "body words", NULL, rows, 3) >= 0); }
}

static void test_setter_validate_and_immutable(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkc1_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->multivector_pq_k() == 256);				/* default */
	CHECK(ix->set_multivector_pq_k(17) != 0);			/* not a power of two -> reject */
	CHECK(ix->set_multivector_pq_k(512) != 0);			/* out of [2,256] -> reject */
	CHECK(ix->set_multivector_pq_k(16) == 0);			/* ok */
	CHECK(ix->multivector_pq_k() == 16);
	CHECK(ix->set_multivector_pq_k(16) == 0);			/* idempotent */
	CHECK(ix->set_multivector_pq_k(32) != 0);			/* immutable once changed */
	CHECK(ix->multivector_pq_k() == 16);
	delete ix; delete [] dir;
	printf("test_setter_validate_and_immutable OK\n");
}

static void test_persist_and_build_v3(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkc2_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	add_docs(ix, 0, 12);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
	long long gen = ix->disk_segment_generation(0);
	delete ix;

	/* the .mvpq is v3 (k=16) */
	char mp[4096]; snprintf(mp, sizeof(mp), "%s/seg_%06lld.mvpq", dir, gen);
	FILE *fp = fopen(mp, "rb"); CHECK(fp != NULL);
	unsigned int version; fseek(fp, 8, SEEK_SET); CHECK(fread(&version,4,1,fp)==1); CHECK(version == 3u); fclose(fp);

	/* reopen restores k=16 from config v5 */
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	CHECK(re->multivector_pq_k() == 16);
	CHECK(re->build_token_index() == 0);
	float q[2*RD]; fill(1, q); fill(2, q+RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);
	delete re; delete [] dir;
	printf("test_persist_and_build_v3 OK\n");
}

int main(void)
{
	test_setter_validate_and_immutable();
	test_persist_and_build_v3();
	printf("ALL test_mvpq_variable_k_config PASSED\n");
	return 0;
}

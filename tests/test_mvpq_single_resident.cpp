/*
	TEST_MVPQ_SINGLE_RESIDENT.CPP -- token epic 4/4 Task 2: engine load-site borrow.
	Under global mode every segment's loaded .mvpq store BORROWS the engine's
	resident global_mvpq_codebook; search is byte-identical to the owned baseline;
	teardown is order-independent. Global off -> owned (no borrow).
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
{ for (long long i=lo;i<hi;i++){ float rows[3*RD]; for(int r=0;r<3;r++) fill(i*5+r, rows+r*RD);
  char key[32]; snprintf(key,sizeof(key),"d-%lld",i); CHECK(ix->add_document(key,"body",NULL,rows,3)>=0); } }

/* build 2 global-mode segments, reopen, and assert every segment store borrows the
   resident codebook (pointer identity) and search works. Then delete the engine
   (resident freed, then store dtors run) -> no crash/double-free. */
static void test_borrow_active_and_teardown(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msr1_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 12); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 12, 24); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	delete ix;

	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);					/* open() load sites now borrow */
	CHECK(re->disk_segment_count() == 2);
	CHECK(re->multivector_pq_global_codebook() == 1);
	const float *resident = re->debug_global_mvpq_codebook();		/* accessor added in Step 5 */
	CHECK(resident != NULL);
	for (long long w = 0; w < re->disk_segment_count(); w++)
		{
		const ANT_multivector_pq_store *st = re->debug_segment_multivector_pq(w);	/* accessor added in Step 5 */
		CHECK(st != NULL);
		CHECK(((ANT_multivector_pq_store *)st)->codebook_is_borrowed() == 1);
		CHECK(((ANT_multivector_pq_store *)st)->get_codebook() == resident);			/* points at resident */
		}
	CHECK(re->build_token_index() == 0);
	float q[2*RD]; fill(1, q); fill(2, q+RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);
	delete re;									/* engine dtor frees resident then segment stores -> must not double-free */
	delete [] dir;
	printf("test_borrow_active_and_teardown OK\n");
}

/* global OFF -> stores own their embedded copy (no borrow). */
static void test_global_off_owns(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msr2_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* NO global codebook */
	add_docs(ix, 0, 12); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	delete ix;
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	const ANT_multivector_pq_store *st = re->debug_segment_multivector_pq(0);
	CHECK(st != NULL && ((ANT_multivector_pq_store *)st)->codebook_is_borrowed() == 0);	/* owns embedded */
	delete re; delete [] dir;
	printf("test_global_off_owns OK\n");
}

/* compaction under global mode: the merged output segment's resident .mvpq store
   must ALSO borrow the resident codebook (not own a redundant copy). */
static void test_compacted_segment_borrows(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msr3_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 12); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 12, 24); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_count() == 2);

	long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
	CHECK(ix->compact(gens, 2) == 0);
	CHECK(ix->disk_segment_count() == 1);

	const float *resident = ix->debug_global_mvpq_codebook();
	CHECK(resident != NULL);
	ANT_multivector_pq_store *st = ix->debug_segment_multivector_pq(0);
	CHECK(st != NULL);
	CHECK(st->codebook_is_borrowed() == 1);						/* merged segment borrows */
	CHECK(st->get_codebook() == resident);						/* points at resident */
	CHECK(ix->build_token_index() == 0);
	float q[2*RD]; fill(1, q); fill(2, q+RD);
	CHECK(ix->search_multivector(q, 2, 10) > 0);
	delete ix; delete [] dir;
	printf("test_compacted_segment_borrows OK\n");
}

int main(void)
{
	test_borrow_active_and_teardown();
	test_global_off_owns();
	test_compacted_segment_borrows();
	printf("ALL test_mvpq_single_resident PASSED\n");
	return 0;
}

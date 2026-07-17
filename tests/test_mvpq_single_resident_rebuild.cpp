/*
	TEST_MVPQ_SINGLE_RESIDENT_REBUILD.CPP -- token epic 4/4 Task 3: rebuild under
	borrowing. rebuild_mvpq_global_codebook frees+retrains the resident; borrowing
	stores must be dropped then reloaded re-borrowing the NEW codebook (no UAF, no
	skipped re-encode); composes with k!=256 / OPQ / NONE-tier.
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
static void fill_shift(long long seed, float *v)		/* different distribution for rebuild */
{ double n=0; for (int j=0;j<RD;j++){v[j]=(float)(((seed*11+j*5)%17)-8)/8.0f * (j==0?3.0f:1.0f);n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<RD;j++)v[j]/=(float)n; }
static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi, int shift)
{ for (long long i=lo;i<hi;i++){ float rows[3*RD]; for(int r=0;r<3;r++){ if(shift) fill_shift(i*5+r,rows+r*RD); else fill(i*5+r,rows+r*RD);}
  char key[32]; snprintf(key,sizeof(key),"d-%lld",i); CHECK(ix->add_document(key,"body",NULL,rows,3)>=0); } }

/* rebuild under borrowing (global mode, default FLOAT tier), k=16, then assert every
   segment re-borrows the NEW resident codebook and search is sane (no UAF). */
static void test_rebuild_reborrow(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msrr_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 12, 0); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 12, 24, 0); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	/* a third, differently-distributed segment so rebuild actually changes the codebook */
	add_docs(ix, 100, 112, 1); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);

	CHECK(ix->rebuild_mvpq_global_codebook() == 0);
	const float *resident = ix->debug_global_mvpq_codebook();
	CHECK(resident != NULL);
	for (long long w = 0; w < ix->disk_segment_count(); w++)
		{
		const ANT_multivector_pq_store *st = ix->debug_segment_multivector_pq(w);
		CHECK(st != NULL);
		CHECK(((ANT_multivector_pq_store *)st)->codebook_is_borrowed() == 1);		/* re-borrowed */
		CHECK(((ANT_multivector_pq_store *)st)->get_codebook() == resident);			/* the NEW resident */
		}
	CHECK(ix->build_token_index() == 0);
	float q[2*RD]; fill(3, q); fill(7, q+RD);
	CHECK(ix->search_multivector(q, 2, 10) > 0);					/* no UAF, sane */
	delete ix; delete [] dir;
	printf("test_rebuild_reborrow OK\n");
}

/* compose: borrow + OPQ + NONE-tier + rebuild (the memory-heaviest corner). */
static void test_borrow_opq_none_tier_rebuild(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msron_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_opq(1) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 14, 0); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 100, 114, 1); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);

	/* switch to NONE tier and reopen so token_source wraps borrowing .mvpq stores */
	CHECK(ix->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete ix;
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	/* under OPQ+global the stores borrow BOTH codebook and rotation */
	const ANT_multivector_pq_store *st0 = re->debug_segment_multivector_pq(0);
	CHECK(st0 != NULL && ((ANT_multivector_pq_store *)st0)->codebook_is_borrowed() == 1);
	CHECK(re->build_token_index() == 0);
	float q[2*RD]; fill(1, q); fill(2, q+RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);					/* graph path, borrowed rotation used */
	CHECK(re->rebuild_mvpq_global_codebook() == 0);					/* rebuild under NONE-tier borrowing */
	for (long long w = 0; w < re->disk_segment_count(); w++)
		CHECK(re->disk_segment_has_token_index(w) == 0);			/* T3 UAF guard: token_index invalidated */
	CHECK(re->build_token_index() == 0);
	CHECK(re->search_multivector(q, 2, 10) > 0);					/* sane after rebuild */
	delete re; delete [] dir;
	printf("test_borrow_opq_none_tier_rebuild OK\n");
}

int main(void)
{
	test_rebuild_reborrow();
	test_borrow_opq_none_tier_rebuild();
	printf("ALL test_mvpq_single_resident_rebuild PASSED\n");
	return 0;
}

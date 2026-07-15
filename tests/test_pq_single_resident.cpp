/*
	TEST_PQ_SINGLE_RESIDENT.CPP -- Approach A engine wiring: under global mode
	every resident segment store BORROWS the one engine codebook (same pointer,
	not N copies), search results are unchanged, and default (non-global) mode
	still owns a per-segment copy.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define GDIM 16
#define GM 4

static char *make_dir(const char *tmpl)
{ char b[64]; strcpy(b, tmpl); char *d = mkdtemp(b); if (!d) exit(printf("mkdtemp\n"));
  char *r = new char[strlen(d)+1]; strcpy(r, d); return r; }
static void gvec(long long i, float *v)
{ for (int d=0; d<GDIM; d++) v[d]=0.02f*(float)(((i*7+d)%5)-2); v[i%GDIM]+=3.0f; }
static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{ float v[GDIM]; char k[32], b[64];
  for (long long i=lo;i<hi;i++){ gvec(i,v); sprintf(k,"d-%lld",i); sprintf(b,"<DOC>t%lld z</DOC>",i);
    CHECK(ix->add_document(k,b,v)>=0);} }

/* global mode: all resident segment stores share ONE codebook pointer (the engine's). */
static void test_all_segments_borrow_one_codebook(void)
{
	char *dir = make_dir("/tmp/ant_sr_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);
	for (int s = 0; s < 3; s++) { add_docs(ix, s*10, s*10+10); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0); }
	CHECK(ix->disk_segment_count() == 3);

	const float *resident = ix->resident_pq_codebook();
	CHECK(resident != NULL);
	for (long i = 0; i < ix->disk_segment_count(); i++)
		{
		CHECK(ix->disk_segment_pq_codebook(i) == resident);		// borrows the SAME buffer
		CHECK(ix->disk_segment_pq_borrowed(i) == 1);
		}
	float q[GDIM]; gvec(5, q);
	CHECK(ix->search_vector(q, 5) >= 1);						// borrowed codebook search works
	delete ix; delete [] dir;
	printf("test_all_segments_borrow_one_codebook OK\n");
}

/* default (non-global) mode: each segment owns its own codebook (distinct pointers). */
static void test_non_global_owns_per_segment(void)
{
	char *dir = make_dir("/tmp/ant_sr2_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	// NO set_pq_global_codebook
	for (int s = 0; s < 2; s++) { add_docs(ix, s*10, s*10+10); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0); }
	CHECK(ix->resident_pq_codebook() == NULL);					// no global codebook resident
	CHECK(ix->disk_segment_pq_borrowed(0) == 0);				// owns its embedded copy
	CHECK(ix->disk_segment_pq_borrowed(1) == 0);
	CHECK(ix->disk_segment_pq_codebook(0) != ix->disk_segment_pq_codebook(1));	// distinct buffers
	delete ix; delete [] dir;
	printf("test_non_global_owns_per_segment OK\n");
}

int main(void)
{
	test_all_segments_borrow_one_codebook();
	test_non_global_owns_per_segment();
	printf("ALL test_pq_single_resident PASSED\n");
	return 0;
}

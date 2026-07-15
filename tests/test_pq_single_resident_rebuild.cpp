/*
	TEST_PQ_SINGLE_RESIDENT_REBUILD.CPP -- Approach A: after
	rebuild_pq_global_codebook() reallocates the resident codebook, every
	segment store re-borrows the NEW pointer (no dangling old one), search
	stays correct, and engine teardown frees exactly once (no double-free).
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

static void test_reborrow_after_rebuild(void)
{
	char *dir = make_dir("/tmp/ant_srr_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);
	for (int s = 0; s < 3; s++) { add_docs(ix, s*10, s*10+10); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0); }

	const float *before = ix->resident_pq_codebook();
	CHECK(ix->rebuild_pq_global_codebook() == 0);
	const float *after = ix->resident_pq_codebook();
	CHECK(after != NULL);
	// every segment now borrows the CURRENT resident pointer (whatever it is post-rebuild).
	for (long i = 0; i < ix->disk_segment_count(); i++)
		{
		CHECK(ix->disk_segment_pq_borrowed(i) == 1);
		CHECK(ix->disk_segment_pq_codebook(i) == after);	// re-borrowed the new buffer, not a dangling `before`
		}
	float q[GDIM]; gvec(7, q);
	CHECK(ix->search_vector(q, 5) >= 1);					// search correct through the new borrowed codebook
	(void)before;
	delete ix;												// teardown: resident freed once; borrowing stores don't free/deref it
	delete [] dir;
	printf("test_reborrow_after_rebuild OK\n");
}

int main(void)
{
	test_reborrow_after_rebuild();
	printf("ALL test_pq_single_resident_rebuild PASSED\n");
	return 0;
}

/*
	TEST_MVPQ_BORROW.CPP -- token epic 4/4 Task 1: single-resident borrow seam.
	A store loaded with a hand-supplied borrowed codebook points AT it (skips the
	embedded copy), reports is_borrowed, reconstructs identically to the owned
	load, and its dtor frees nothing shared. Declines (owns embedded) on opq
	mismatch.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 8
#define MM 4

static void fill(long long seed, float *v)
{ double n=0; for (int j=0;j<DIM;j++){v[j]=(float)(((seed*7+j*3)%13)-6)/6.0f;n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<DIM;j++)v[j]/=(float)n; }

/* build a non-OPQ k=256 .mvpq over ndoc docs (2 tokens each) */
static void build(const char *path, long long ndoc)
{
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, DIM, MM, 256, ANT_pq_codec::METRIC_DOT, 0) == 0);
	for (long long d = 0; d < ndoc; d++)
		{ float rows[2*DIM]; fill(d*3, rows); fill(d*3+1, rows+DIM); CHECK(w.append(rows, 2) == 0); }
	CHECK(w.finish() == 0);
}

static void test_borrow_points_at_supplied_codebook(void)
{
	char path[] = "/tmp/ant_mvb_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	const long long ndoc = 16;
	build(path, ndoc);

	/* owned load: get the canonical codebook + a reconstruction */
	ANT_multivector_pq_store *owned = ANT_multivector_pq_store::load(path, DIM, ndoc, ANT_pq_codec::METRIC_DOT);
	CHECK(owned != NULL && owned->token_count() == 2*ndoc);
	CHECK(owned->codebook_is_borrowed() == 0);						/* default = owns */
	long long cb_floats = 256 * DIM;
	float *resident = new float[cb_floats];
	memcpy(resident, owned->get_codebook(), (size_t)cb_floats * sizeof(float));
	float rec_owned[DIM]; owned->token_reconstruct(0, rec_owned);
	delete owned;

	/* borrowed load: hand it the resident copy, non-OPQ (rotation NULL) */
	ANT_multivector_pq_store *bor = ANT_multivector_pq_store::load(path, DIM, ndoc, ANT_pq_codec::METRIC_DOT, resident, NULL);
	CHECK(bor != NULL && bor->token_count() == 2*ndoc);
	CHECK(bor->codebook_is_borrowed() == 1);						/* borrowed */
	CHECK(bor->get_codebook() == resident);							/* points AT the supplied buffer */
	float rec_bor[DIM]; bor->token_reconstruct(0, rec_bor);
	for (int j = 0; j < DIM; j++) CHECK(fabs(rec_bor[j] - rec_owned[j]) < 1e-6);	/* identical to owned */
	/* snapshot the resident BEFORE the borrowing store's dtor so we can prove
	   the dtor neither freed nor mutated the shared buffer (teardown-independence). */
	double sum_before = 0; for (long long i = 0; i < cb_floats; i++) sum_before += resident[i];
	delete bor;														/* must NOT free/mutate `resident` */
	double sum_after = 0; for (long long i = 0; i < cb_floats; i++) sum_after += resident[i];
	CHECK(sum_after == sum_before);									/* resident intact after borrower dtor */
	delete [] resident;
	remove(path);
	printf("test_borrow_points_at_supplied_codebook OK\n");
}

/* opq mismatch: file is non-OPQ (opq==0) but caller supplies a rotation -> decline, own embedded. */
static void test_borrow_declines_on_opq_mismatch(void)
{
	char path[] = "/tmp/ant_mvbm_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	const long long ndoc = 10;
	build(path, ndoc);							/* opq off */
	float dummy_cb[256*DIM] = {0};
	float dummy_rot[DIM*DIM] = {0};
	/* supplying a rotation for a non-OPQ file -> (stored_opq==1)==(rot!=NULL) is (0)==(1) = false -> no borrow */
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, DIM, ndoc, ANT_pq_codec::METRIC_DOT, dummy_cb, dummy_rot);
	CHECK(s != NULL && s->token_count() == 2*ndoc);
	CHECK(s->codebook_is_borrowed() == 0);		/* declined -> owns its embedded copy */
	CHECK(s->get_codebook() != dummy_cb);		/* did NOT point at the supplied buffer */
	delete s;
	remove(path);
	printf("test_borrow_declines_on_opq_mismatch OK\n");
}

int main(void)
{
	test_borrow_points_at_supplied_codebook();
	test_borrow_declines_on_opq_mismatch();
	printf("ALL test_mvpq_borrow PASSED\n");
	return 0;
}

/*
	TEST_PQ_BORROW.CPP -- Approach A store borrow seam (#22.2 RAM follow-up):
	a store loaded with a borrowed codebook points at the supplied buffer,
	does NOT own it, and reconstruct/scan match an owned-copy store byte for
	byte; a header-inconsistent borrow falls back to owning; the 4-arg load
	is unchanged.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void fill(float *v, long long n, unsigned seed)
{
	for (long long i = 0; i < n; i++) v[i] = (float)(((i * 131 + seed * 17) % 197) - 98) / 40.0f;
}

/* Build a .pq at (D,m,k=256), opq on/off, and return its path (caller removes). */
static void build_store_opq(char *path, long long D, long long m, long long n, const float *vecs, long opq)
{
	strcpy(path, "/tmp/ant_borrow_XXXXXX");
	CHECK(mkstemp(path) >= 0);
	ANT_pq_store_writer w;
	CHECK(w.create(path, D, m, 256, ANT_pq_codec::METRIC_L2, opq) == 0);
	for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i * D) == 0);
	CHECK(w.finish() == 0);
}

/* Build a non-OPQ .pq at (D,m,k=256) and return its path (caller removes). */
static void build_store(char *path, long long D, long long m, long long n, const float *vecs)
{
	build_store_opq(path, D, m, n, vecs, 0);
}

static void test_borrow_points_at_supplied_and_matches(void)
{
	const long long D = 8, m = 4, n = 40;
	float *vecs = new float[n * D]; fill(vecs, n * D, 3);
	char path[64]; build_store(path, D, m, n, vecs);

	// Owned load (Approach B) gives us the reference codebook + reference scores.
	ANT_pq_store *owned = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2);
	CHECK(owned != NULL && owned->document_count() == n);
	CHECK(owned->codebook_is_borrowed() == 0);
	const float *ref_cb = owned->get_codebook();
	long long cb_floats = m * 256 * (D / m);
	float *shared_cb = new float[cb_floats];
	memcpy(shared_cb, ref_cb, (size_t)cb_floats * sizeof(float));

	// Borrowed load: same file, but hand it shared_cb (no OPQ -> rotation NULL).
	ANT_pq_store *borrowed = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2, shared_cb, NULL);
	CHECK(borrowed != NULL && borrowed->document_count() == n);
	CHECK(borrowed->codebook_is_borrowed() == 1);
	CHECK(borrowed->get_codebook() == shared_cb);			// points AT the supplied buffer

	// reconstruct must be byte-identical between owned and borrowed.
	float ro[8], rb[8];
	for (long long doc = 0; doc < n; doc++)
		{
		owned->reconstruct(doc, ro);
		borrowed->reconstruct(doc, rb);
		CHECK(memcmp(ro, rb, (size_t)D * sizeof(float)) == 0);
		}
	// score must match too.
	float q[8]; fill(q, D, 9);
	for (long long doc = 0; doc < n; doc++)
		CHECK(owned->score(doc, q, ANT_pq_codec::METRIC_L2) == borrowed->score(doc, q, ANT_pq_codec::METRIC_L2));

	delete borrowed;			// must NOT free shared_cb
	// shared_cb still valid here -> use it again to prove the dtor didn't free it.
	ANT_pq_store *again = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2, shared_cb, NULL);
	CHECK(again->codebook_is_borrowed() == 1);
	delete again;
	delete owned;
	delete [] shared_cb;		// we own it; freeing here is the ONLY free
	remove(path); delete [] vecs;
	printf("test_borrow_points_at_supplied_and_matches OK\n");
}

static void test_header_mismatch_falls_back_to_owning(void)
{
	const long long D = 8, m = 4, n = 20;
	float *vecs = new float[n * D]; fill(vecs, n * D, 5);
	char path[64]; build_store(path, D, m, n, vecs);
	// Supply a borrowed codebook but load with a WRONG expected dimension:
	// header validation fails the whole load (returns empty store, no borrow).
	float dummy[1] = { 0 };
	ANT_pq_store *s = ANT_pq_store::load(path, /*wrong D*/ 16, n, ANT_pq_codec::METRIC_L2, dummy, NULL);
	CHECK(s != NULL && s->document_count() == 0);		// degraded empty store (dim mismatch)
	delete s;
	// Correct load WITHOUT a borrow still owns its embedded copy.
	ANT_pq_store *own = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2);
	CHECK(own->codebook_is_borrowed() == 0);
	delete own;
	remove(path); delete [] vecs;
	printf("test_header_mismatch_falls_back_to_owning OK\n");
}

/*
	OPQ (opq==1) borrow: exercises the rotation-skip fseek and the borrowed-rotation
	assignment, and proves un-rotation through the borrowed R is correct.  Also proves
	the half-borrow decline (codebook supplied but rotation NULL on an opq==1 file).
*/
static void test_opq_borrow_rotation_and_half_borrow_decline(void)
{
	const long long D = 8, m = 4, n = 40;
	float *vecs = new float[n * D]; fill(vecs, n * D, 11);
	char path[64]; build_store_opq(path, D, m, n, vecs, /*opq*/ 1);

	// Owned load captures reference codebook + rotation (both embedded R present).
	ANT_pq_store *owned = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2);
	CHECK(owned != NULL && owned->document_count() == n);
	CHECK(owned->codebook_is_borrowed() == 0);
	CHECK(owned->get_rotation() != NULL);					// OPQ actually trained an R
	long long cb_floats = m * 256 * (D / m);
	float *shared_cb = new float[cb_floats];
	memcpy(shared_cb, owned->get_codebook(), (size_t)cb_floats * sizeof(float));
	long long rot_floats = D * D;
	float *shared_rot = new float[rot_floats];
	memcpy(shared_rot, owned->get_rotation(), (size_t)rot_floats * sizeof(float));

	// Full OPQ borrow: hand both buffers -> borrow taken, skips embedded cb + R.
	ANT_pq_store *borrowed = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2, shared_cb, shared_rot);
	CHECK(borrowed != NULL && borrowed->document_count() == n);
	CHECK(borrowed->codebook_is_borrowed() == 1);
	CHECK(borrowed->get_codebook() == shared_cb);			// points AT supplied codebook
	CHECK(borrowed->get_rotation() == shared_rot);			// points AT supplied rotation

	// reconstruct (un-rotates via borrowed R^T) + score must match the owned store.
	float ro[8], rb[8];
	for (long long doc = 0; doc < n; doc++)
		{
		owned->reconstruct(doc, ro);
		borrowed->reconstruct(doc, rb);
		CHECK(memcmp(ro, rb, (size_t)D * sizeof(float)) == 0);
		}
	float q[8]; fill(q, D, 13);
	for (long long doc = 0; doc < n; doc++)
		CHECK(owned->score(doc, q, ANT_pq_codec::METRIC_L2) == borrowed->score(doc, q, ANT_pq_codec::METRIC_L2));

	delete borrowed;			// must NOT free shared_cb / shared_rot

	// Half-borrow decline: opq==1 file but rotation NULL -> borrow refused, owns embedded copy.
	ANT_pq_store *declined = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2, shared_cb, NULL);
	CHECK(declined != NULL && declined->document_count() == n);
	CHECK(declined->codebook_is_borrowed() == 0);			// (stored_opq==1)==(rotation!=NULL) is false -> declined
	CHECK(declined->get_codebook() != shared_cb);			// owns its own embedded copy
	// declined still reconstructs correctly through its owned R.
	for (long long doc = 0; doc < n; doc++)
		{
		owned->reconstruct(doc, ro);
		declined->reconstruct(doc, rb);
		CHECK(memcmp(ro, rb, (size_t)D * sizeof(float)) == 0);
		}
	delete declined;

	delete owned;
	delete [] shared_cb;		// we own these; the ONLY frees of the borrowed buffers
	delete [] shared_rot;
	remove(path); delete [] vecs;
	printf("test_opq_borrow_rotation_and_half_borrow_decline OK\n");
}

int main(void)
{
	test_borrow_points_at_supplied_and_matches();
	test_header_mismatch_falls_back_to_owning();
	test_opq_borrow_rotation_and_half_borrow_decline();
	printf("ALL test_pq_borrow PASSED\n");
	return 0;
}

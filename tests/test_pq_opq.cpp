/*
	TEST_PQ_OPQ.CPP -- #22 OPQ rotation. Task 1 locks the pure codec helpers:
	R is orthogonal, apply/apply_transpose are inverses, the rotation preserves
	dot product, and training is deterministic. Task 2 locks the store integration:
	OPQ v2 sidecar round-trips, un-rotated reconstruct is original-space, non-OPQ
	still loads (back-compat), and the OPQ .pq is byte-deterministic.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void test_rotation_orthogonal_and_dot_preserving(void)
{
	const long long D = 8, m = 4, n = 64;
	// anisotropic data: mix dims so variance is off-axis
	float *vecs = new float[n * D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D + d] = (float)(((i * 7 + d * 13) % 17) - 8) * (d < 3 ? 4.0f : 0.2f);

	float *R = new float[D * D];
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R) == 0);

	// R R^T == I (orthogonal): check a few entries of R*R^T
	for (long long a = 0; a < D; a++)
		for (long long b = 0; b < D; b++)
			{
			double dot = 0;
			for (long long k = 0; k < D; k++) dot += (double)R[a*D+k] * (double)R[b*D+k];
			CHECK(fabs(dot - (a == b ? 1.0 : 0.0)) < 1e-4);
			}

	// apply then apply_transpose round-trips
	float q[D], rq[D], back[D];
	for (long long d = 0; d < D; d++) q[d] = (float)(d + 1) * 0.3f - 1.0f;
	ANT_pq_codec::apply_rotation(q, D, R, rq);
	ANT_pq_codec::apply_rotation_transpose(rq, D, R, back);
	for (long long d = 0; d < D; d++) CHECK(fabs(back[d] - q[d]) < 1e-4);

	// dot product preserved: dot(Rq, Rx) == dot(q, x)
	float x[D], rx[D];
	for (long long d = 0; d < D; d++) x[d] = (float)((d * 3) % 5) - 2.0f;
	ANT_pq_codec::apply_rotation(x, D, R, rx);
	double d_orig = 0, d_rot = 0;
	for (long long d = 0; d < D; d++) { d_orig += (double)q[d]*x[d]; d_rot += (double)rq[d]*rx[d]; }
	CHECK(fabs(d_orig - d_rot) < 1e-3);

	delete [] vecs; delete [] R;
	printf("test_rotation_orthogonal_and_dot_preserving OK\n");
}

static void test_rotation_deterministic(void)
{
	const long long D = 6, m = 3, n = 40;
	float *vecs = new float[n * D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D + d] = (float)(((i * 5 + d * 11) % 13) - 6);
	float *R1 = new float[D*D], *R2 = new float[D*D];
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R1) == 0);
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R2) == 0);
	CHECK(memcmp(R1, R2, (size_t)(D*D)*sizeof(float)) == 0);   // byte-identical
	delete [] vecs; delete [] R1; delete [] R2;
	printf("test_rotation_deterministic OK\n");
}

static void test_rotation_rejects_bad_args(void)
{
	float R[16];
	float v[4] = {1,2,3,4};
	CHECK(ANT_pq_codec::train_rotation(v, 4, 3, 1, R) == 1);  // 3 does not divide 4
	CHECK(ANT_pq_codec::train_rotation(v, 4, 2, 0, R) == 1);  // n == 0
	printf("test_rotation_rejects_bad_args OK\n");
}

// build a .pq at `path` from `n` D-dim rows via the writer, OPQ on/off; returns present count
static void write_pq(const char *path, const float *vecs, long long D, long long m, long long n, long metric, long opq)
{
	ANT_pq_store_writer w;
	CHECK(w.create(path, D, m, metric, opq) == 0);
	for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
	CHECK(w.finish() == 0);
}

static void test_store_opq_roundtrip_and_backcompat(void)
{
	const long long D = 8, m = 4, n = 60;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D+d] = (float)(((i*7 + d*13) % 17) - 8) * (d < 3 ? 4.0f : 0.2f);

	char p_opq[] = "/tmp/ant_opq_XXXXXX";  CHECK(mkstemp(p_opq) >= 0);
	char p_no[]  = "/tmp/ant_no_XXXXXX";   CHECK(mkstemp(p_no) >= 0);
	write_pq(p_opq, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	write_pq(p_no,  vecs, D, m, n, ANT_pq_codec::METRIC_L2, 0);

	ANT_pq_store *s_opq = ANT_pq_store::load(p_opq, D, n, ANT_pq_codec::METRIC_L2);
	ANT_pq_store *s_no  = ANT_pq_store::load(p_no,  D, n, ANT_pq_codec::METRIC_L2);
	CHECK(s_opq != NULL && s_opq->document_count() == n);   // OPQ v2 sidecar loads
	CHECK(s_no  != NULL && s_no->document_count() == n);    // non-OPQ still loads

	// reconstruct under OPQ returns an ORIGINAL-space approximation (R^T applied):
	// its error vs the true vector is comparable to the non-OPQ store (not rotated garbage).
	float recon[8], truth[8];
	double err_opq = 0, err_no = 0;
	for (long long doc = 0; doc < n; doc++)
		{
		for (long long d = 0; d < D; d++) truth[d] = vecs[doc*D+d];
		s_opq->reconstruct(doc, recon);
		for (long long d = 0; d < D; d++) err_opq += (recon[d]-truth[d])*(recon[d]-truth[d]);
		s_no->reconstruct(doc, recon);
		for (long long d = 0; d < D; d++) err_no += (recon[d]-truth[d])*(recon[d]-truth[d]);
		}
	CHECK(err_opq < err_no * 3.0 + 1.0);   // same order of magnitude, not rotated-space nonsense

	delete s_opq; delete s_no;
	remove(p_opq); remove(p_no);
	delete [] vecs;
	printf("test_store_opq_roundtrip_and_backcompat OK\n");
}

static void test_store_opq_deterministic(void)
{
	const long long D = 8, m = 4, n = 50;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*5 + d*11) % 13) - 6) * (d < 2 ? 3.0f : 0.3f);
	char a[] = "/tmp/ant_da_XXXXXX", b[] = "/tmp/ant_db_XXXXXX";
	CHECK(mkstemp(a) >= 0); CHECK(mkstemp(b) >= 0);
	write_pq(a, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	write_pq(b, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
	CHECK(fa && fb);
	fseek(fa,0,SEEK_END); fseek(fb,0,SEEK_END);
	long la = ftell(fa), lb = ftell(fb);
	CHECK(la == lb && la > 0);
	rewind(fa); rewind(fb);
	unsigned char *ba = new unsigned char[la], *bb = new unsigned char[lb];
	CHECK(fread(ba,1,la,fa)==(size_t)la && fread(bb,1,lb,fb)==(size_t)lb);
	CHECK(memcmp(ba, bb, la) == 0);   // byte-identical OPQ .pq
	fclose(fa); fclose(fb); delete[] ba; delete[] bb;
	remove(a); remove(b); delete [] vecs;
	printf("test_store_opq_deterministic OK\n");
}

int main(void)
{
	test_rotation_orthogonal_and_dot_preserving();
	test_rotation_deterministic();
	test_rotation_rejects_bad_args();
	test_store_opq_roundtrip_and_backcompat();
	test_store_opq_deterministic();
	printf("ALL TESTS PASSED\n");
	return 0;
}

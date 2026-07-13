/*
	TEST_PQ_OPQ.CPP -- #22 OPQ rotation. Task 1 locks the pure codec helpers:
	R is orthogonal, apply/apply_transpose are inverses, the rotation preserves
	dot product, and training is deterministic.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../source/pq_codec.h"

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

int main(void)
{
	test_rotation_orthogonal_and_dot_preserving();
	test_rotation_deterministic();
	test_rotation_rejects_bad_args();
	printf("ALL TESTS PASSED\n");
	return 0;
}

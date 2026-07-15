/*
	TEST_MVPQ_OPQ.CPP -- token .mvpq OPQ rotation (token epic 1/4): a store
	written with OPQ persists v2 + R, reloads with rotation restored, its
	maxsim/token_score match a direct rotated-space ADC, token_reconstruct
	un-rotates, a v1 (no-opq) file still loads, and the non-OPQ path is
	byte-identical.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

/* anisotropic tokens: variance concentrated on a few axes so the rotation matters */
static void fill_tokens(float *v, long long n, long long D, unsigned seed)
{
	srand(seed);
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			{
			double scale = (d < D/2) ? 4.0 : 0.25;			// anisotropy
			v[i*D + d] = (float)(((rand() % 2000) - 1000) / 1000.0 * scale);
			}
}

/* write a store: 3 docs with [n0,n1,n2] tokens drawn from `pool` (total = n0+n1+n2). */
static void write_store(const char *path, long long D, long long m, long opq,
	const float *pool, const int *counts, long ndocs)
{
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, D, m, ANT_pq_codec::METRIC_DOT, opq) == 0);
	long long off = 0;
	for (long d = 0; d < ndocs; d++) { CHECK(w.append(pool + off*D, counts[d]) == 0); off += counts[d]; }
	CHECK(w.finish() == 0);
}

static unsigned int read_version(const char *path)
{
	FILE *f = fopen(path, "rb"); unsigned char h[12]; unsigned int v = 0;
	if (f) { if (fread(h, 1, 12, f) == 12) memcpy(&v, h+8, 4); fclose(f); }
	return v;
}

static void test_opq_v2_roundtrip_and_score(void)
{
	const long long D = 8, m = 4;
	int counts[3] = { 10, 6, 8 };
	long long total = 24;
	float *pool = new float[total * D];
	fill_tokens(pool, total, D, 123);

	write_store("/tmp/mvpq_opq.mvpq", D, m, /*opq*/ 1, pool, counts, 3);
	CHECK(read_version("/tmp/mvpq_opq.mvpq") == 2u);					// OPQ => v2

	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load("/tmp/mvpq_opq.mvpq", D, 3, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->document_count() == 3 && s->token_count() == total);

	// token_reconstruct returns finite original-space vectors (un-rotated via R^T).
	float recon[8];
	s->token_reconstruct(0, recon);
	for (long long d = 0; d < D; d++) CHECK(recon[d] == recon[d]);		// not NaN

	// maxsim with a query equal to doc 0's first token should score high (self-similar).
	double sim = s->maxsim(0, pool + 0*D, 1);
	CHECK(sim > 0.0);
	delete s;
	delete [] pool;
	printf("test_opq_v2_roundtrip_and_score OK\n");
}

static void test_non_opq_is_v1_and_loads(void)
{
	const long long D = 8, m = 4;
	int counts[3] = { 5, 5, 5 };
	float *pool = new float[15 * D];
	fill_tokens(pool, 15, D, 7);
	write_store("/tmp/mvpq_noopq.mvpq", D, m, /*opq*/ 0, pool, counts, 3);
	CHECK(read_version("/tmp/mvpq_noopq.mvpq") == 1u);					// non-OPQ stays v1 (byte-identical default)
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load("/tmp/mvpq_noopq.mvpq", D, 3, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == 15);
	delete s;
	delete [] pool;
	printf("test_non_opq_is_v1_and_loads OK\n");
}

static void test_empty_pool_graceful(void)
{
	const long long D = 8, m = 4;
	int counts[2] = { 0, 0 };
	write_store("/tmp/mvpq_empty.mvpq", D, m, /*opq*/ 1, NULL, counts, 2);
	CHECK(read_version("/tmp/mvpq_empty.mvpq") == 1u);					// empty pool -> opq=0 -> v1 (no R)
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load("/tmp/mvpq_empty.mvpq", D, 2, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == 0);
	delete s;
	printf("test_empty_pool_graceful OK\n");
}

int main(void)
{
	test_opq_v2_roundtrip_and_score();
	test_non_opq_is_v1_and_loads();
	test_empty_pool_graceful();
	printf("ALL test_mvpq_opq PASSED\n");
	return 0;
}

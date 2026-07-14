/*
	TEST_PQ_CODEC_KWIDTH.CPP -- variable code-width codec (#22.3):
	pack/unpack round-trip across all bit-widths, bits_for_k validation,
	and k-parameter self-consistency (encode->adc_score ranks the assigned
	centroid highest; reconstruct returns it). k=256 stays byte-identical.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pq_codec.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_bits_for_k(void)
{
CHECK(ANT_pq_codec::bits_for_k(2) == 1);
CHECK(ANT_pq_codec::bits_for_k(16) == 4);
CHECK(ANT_pq_codec::bits_for_k(64) == 6);
CHECK(ANT_pq_codec::bits_for_k(256) == 8);
CHECK(ANT_pq_codec::bits_for_k(6) == -1);		// not a power of two
CHECK(ANT_pq_codec::bits_for_k(1) == -1);		// below 2
CHECK(ANT_pq_codec::bits_for_k(512) == -1);		// above 256
CHECK(ANT_pq_codec::bits_for_k(0) == -1);
}

static void test_pack_roundtrip(void)
{
long long m = 13;						// deliberately not a byte multiple
for (long bits = 1; bits <= 8; bits++)
	{
	long long k = 1LL << bits;
	unsigned char codes[13], out[13];
	for (long long s = 0; s < m; s++)
		codes[s] = (unsigned char)((s * 7 + 3) % k);	// each < 2^bits
	long long row_bytes = (m * bits + 7) / 8;
	unsigned char *packed = new unsigned char[row_bytes + 4];
	memset(packed, 0xAB, (size_t)(row_bytes + 4));		// canary tail
	ANT_pq_codec::pack_codes(codes, m, bits, packed);
	ANT_pq_codec::unpack_codes(packed, m, bits, out);
	CHECK(memcmp(codes, out, (size_t)m) == 0);
	if (bits < 8)						// trailing bits of last byte must be zero
		{
		long long used = m * bits;
		if (used % 8 != 0)
			{
			unsigned char last = packed[(used - 1) / 8];
			unsigned char mask = (unsigned char)(0xFF << (used % 8));
			CHECK((last & mask) == 0);
			}
		}
	delete [] packed;
	}
}

static void test_pack_bits8_is_memcpy(void)
{
long long m = 5;
unsigned char codes[5] = { 0, 42, 255, 7, 200 }, packed[5];
ANT_pq_codec::pack_codes(codes, m, 8, packed);
CHECK(memcmp(codes, packed, (size_t)m) == 0);		// identity at bits==8
}

static void test_k_param_self_consistent(void)
{
// 4-dim vectors, m=2 subspaces of 2 dims, k=16 centroids.
long long dim = 4, m = 2, k = 16, n = 32, sub = dim / m;
float *vectors = new float[n * dim];
srand(12345);
for (long long i = 0; i < n * dim; i++)
	vectors[i] = (float)(rand() % 1000) / 1000.0f;
float *codebook = new float[m * k * sub];
CHECK(ANT_pq_codec::train(vectors, dim, m, k, n, codebook) == 0);

for (long long i = 0; i < n; i++)
	{
	unsigned char codes[2];
	ANT_pq_codec::encode(vectors + i * dim, dim, m, k, codebook, codes);
	CHECK(codes[0] < k && codes[1] < k);
	// reconstruct returns the assigned centroids
	float recon[4];
	ANT_pq_codec::reconstruct(codes, dim, m, k, codebook, recon);
	for (long long s = 0; s < m; s++)
		{
		const float *cent = codebook + s * k * sub + codes[s] * sub;
		for (long long d = 0; d < sub; d++)
			CHECK(fabs(recon[s * sub + d] - cent[d]) < 1e-6);
		}
	// ADC (dot metric): the assigned code should score >= any other code in each subspace
	double *table = new double[m * k];
	ANT_pq_codec::adc_table(vectors + i * dim, dim, m, k, codebook, ANT_pq_codec::METRIC_L2, table);
	double best = ANT_pq_codec::adc_score(codes, m, k, table);
	for (long long s = 0; s < m; s++)
		for (long long c = 0; c < k; c++)
			CHECK(table[s * k + codes[s]] >= table[s * k + c] - 1e-9);	// nearest under L2
	(void)best;
	delete [] table;
	}
delete [] vectors;
delete [] codebook;
}

static void test_k256_matches_default(void)
{
// With k=256 the codec must behave exactly as the fixed-K codec did.
long long dim = 8, m = 4, k = ANT_pq_codec::K, n = 50, sub = dim / m;
float *vectors = new float[n * dim];
srand(999);
for (long long i = 0; i < n * dim; i++)
	vectors[i] = (float)(rand() % 2000 - 1000) / 500.0f;
float *codebook = new float[m * k * sub];
CHECK(ANT_pq_codec::train(vectors, dim, m, k, n, codebook) == 0);
unsigned char codes[4], packed[4];
ANT_pq_codec::encode(vectors, dim, m, k, codebook, codes);
ANT_pq_codec::pack_codes(codes, m, 8, packed);
CHECK(memcmp(codes, packed, (size_t)m) == 0);		// packing is identity at k=256
delete [] vectors;
delete [] codebook;
}

int main(void)
{
test_bits_for_k();
test_pack_roundtrip();
test_pack_bits8_is_memcpy();
test_k_param_self_consistent();
test_k256_matches_default();
if (failures == 0)
	printf("ALL test_pq_codec_kwidth PASSED\n");
else
	printf("%d CHECK(s) FAILED\n", failures);
return failures == 0 ? 0 : 1;
}

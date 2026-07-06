/*
	TEST_SIGNATURE.CPP -- unit tests for ANT_signature (SimHash + Hamming).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../source/signature.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void test_deterministic(void)
{
long long dim = 32, bits = 128;
ANT_signature a(dim, bits, 12345ULL), b(dim, bits, 12345ULL);
float v[32];
for (long long i = 0; i < dim; i++) v[i] = (float)sin((double)i);
unsigned char sa[16], sb[16];
a.sign(v, sa);
b.sign(v, sb);
CHECK(memcmp(sa, sb, 16) == 0);
CHECK(a.signature_bytes() == 16);
printf("test_deterministic OK\n");
}

static void test_hamming_tracks_angle(void)
{
long long dim = 64, bits = 256;
ANT_signature s(dim, bits, 99ULL);
float base[64], near_v[64], far_v[64];
for (long long i = 0; i < dim; i++) { base[i] = (float)((i % 7) - 3); near_v[i] = base[i]; far_v[i] = -base[i]; }
near_v[0] += 0.01f;
unsigned char sb[32], sn[32], sf[32];
s.sign(base, sb); s.sign(near_v, sn); s.sign(far_v, sf);
long long h_near = ANT_signature::hamming(sb, sn, 32);
long long h_far  = ANT_signature::hamming(sb, sf, 32);
CHECK(h_near < h_far);
CHECK(h_near <= 8);
printf("test_hamming_tracks_angle OK\n");
}

int main(void)
{
test_deterministic();
test_hamming_tracks_angle();
printf("PASSED\n");
return 0;
}

/*
	TEST_VECTOR_QUANTIZE.CPP -- per-dimension int8 quantize/dequantize.
*/
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../source/vector_quantize.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void test_roundtrip_error_bound(void)
{
long long dim = 32, n = 200, i, d;
float *data = new float[n * dim];
srand(3);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 2000 - 1000) / 1000.0f;	/* [-1,1] */

float *mins = new float[dim], *maxs = new float[dim];
ANT_vector_quantize::compute_ranges(data, dim, n, mins, maxs);

signed char *codes = new signed char[dim];
float *recon = new float[dim];
double worst = 0.0;
for (i = 0; i < n; i++)
	{
	ANT_vector_quantize::quantize(data + i * dim, dim, mins, maxs, codes);
	ANT_vector_quantize::reconstruct(codes, dim, mins, maxs, recon);
	for (d = 0; d < dim; d++)
		{
		double step = (maxs[d] - mins[d]) / 255.0;
		double err = fabs((double)recon[d] - (double)data[i * dim + d]);
		if (err > worst) worst = err;
		CHECK(err <= step + 1e-6);
		}
	}
CHECK(worst > 0.0);
delete [] data; delete [] mins; delete [] maxs; delete [] codes; delete [] recon;
printf("test_roundtrip_error_bound OK (worst abs err %.5f)\n", worst);
}

static void test_degenerate_dimension(void)
{
long long dim = 3, n = 4, i;
float data[12] = { 5,0,-1,  5,1,-1,  5,2,-1,  5,3,-1 };
float mins[3], maxs[3];
ANT_vector_quantize::compute_ranges(data, dim, n, mins, maxs);
signed char codes[3]; float recon[3];
for (i = 0; i < n; i++)
	{
	ANT_vector_quantize::quantize(data + i * dim, dim, mins, maxs, codes);
	ANT_vector_quantize::reconstruct(codes, dim, mins, maxs, recon);
	CHECK(recon[0] == 5.0f);
	CHECK(recon[2] == -1.0f);
	}
printf("test_degenerate_dimension OK\n");
}

static void test_determinism(void)
{
long long dim = 8, n = 50, i;
float *data = new float[n * dim];
srand(9);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 100) / 10.0f;
float a_min[8], a_max[8], b_min[8], b_max[8];
ANT_vector_quantize::compute_ranges(data, dim, n, a_min, a_max);
ANT_vector_quantize::compute_ranges(data, dim, n, b_min, b_max);
for (i = 0; i < dim; i++) { CHECK(a_min[i] == b_min[i]); CHECK(a_max[i] == b_max[i]); }
signed char ca[8], cb[8];
ANT_vector_quantize::quantize(data, dim, a_min, a_max, ca);
ANT_vector_quantize::quantize(data, dim, b_min, b_max, cb);
for (i = 0; i < dim; i++) CHECK(ca[i] == cb[i]);
delete [] data;
printf("test_determinism OK\n");
}

int main(void)
{
test_roundtrip_error_bound();
test_degenerate_dimension();
test_determinism();
printf("PASSED\n");
return 0;
}

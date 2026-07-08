#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../source/pq_codec.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)

static void test_m_must_divide(void)
{
	float cb[1]; float v[6] = {0};
	CHECK(ANT_pq_codec::train(v, 6, 4, 1, cb) == 1);	// 4 does not divide 6
}

static void test_determinism_and_recall(void)
{
	long long dim = 16, m = 4, n = 200, i;
	float *data = new float[n*dim];
	srand(7);
	for (i = 0; i < n*dim; i++) data[i] = (float)(rand()%200-100)/100.0f;
	long long cbsize = m * ANT_pq_codec::K * (dim/m);
	float *cb1 = new float[cbsize], *cb2 = new float[cbsize];
	CHECK(ANT_pq_codec::train(data, dim, m, n, cb1) == 0);
	CHECK(ANT_pq_codec::train(data, dim, m, n, cb2) == 0);
	CHECK(memcmp(cb1, cb2, cbsize*sizeof(float)) == 0);		// deterministic

	unsigned char codes[4];
	double table[4*256];
	float q[16]; for (i=0;i<dim;i++) q[i]=(float)(rand()%200-100)/100.0f;
	ANT_pq_codec::adc_table(q, dim, m, cb1, ANT_pq_codec::METRIC_DOT, table);
	double maxerr = 0;
	for (long long d = 0; d < n; d++)
		{
		ANT_pq_codec::encode(data+d*dim, dim, m, cb1, codes);
		float recon[16]; ANT_pq_codec::reconstruct(codes, dim, m, cb1, recon);
		double adc = ANT_pq_codec::adc_score(codes, m, table);
		double rdot = 0; for (i=0;i<dim;i++) rdot += (double)q[i]*recon[i];
		double e = fabs(adc - rdot); if (e > maxerr) maxerr = e;
		}
	CHECK(maxerr < 1e-3);		// ADC == reconstruct-then-dot (same centroids)
	delete[] data; delete[] cb1; delete[] cb2;
}

static void test_degenerate_subspace(void)
{
	long long dim = 4, m = 2, n = 10, i;
	float *data = new float[n*dim];
	for (i = 0; i < n*dim; i++) data[i] = 3.0f;
	long long cbsize = m*ANT_pq_codec::K*(dim/m);
	float *cb = new float[cbsize];
	CHECK(ANT_pq_codec::train(data, dim, m, n, cb) == 0);
	unsigned char codes[2]; float recon[4];
	ANT_pq_codec::encode(data, dim, m, cb, codes);
	ANT_pq_codec::reconstruct(codes, dim, m, cb, recon);
	for (i=0;i<dim;i++) CHECK(fabs(recon[i]-3.0f) < 1e-4);
	delete[] data; delete[] cb;
}

int main(void)
{
	test_m_must_divide();
	test_determinism_and_recall();
	test_degenerate_subspace();
	printf("test_pq_codec PASSED\n");
	return 0;
}

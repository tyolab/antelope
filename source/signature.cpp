/*
	SIGNATURE.CPP
*/
#include <string.h>
#include "signature.h"
#include "mersenne_twister.h"

ANT_signature::ANT_signature(long long dimension, long long bits, unsigned long long seed)
{
long long i, total;
ANT_mersenne_twister twister;

this->dimension = dimension;
this->bits = bits;
total = bits * dimension;
projection = new float[total > 0 ? total : 1];
twister.init_genrand64(seed);
for (i = 0; i < total; i++)
	projection[i] = (float)(twister.genrand64_real2() * 2.0 - 1.0);		// uniform in [-1, 1)
}

ANT_signature::~ANT_signature()
{
delete [] projection;
}

void ANT_signature::sign(const float *vector, unsigned char *out_signature)
{
long long bit, d;
const float *hyperplane;
double dot;

memset(out_signature, 0, (size_t)signature_bytes());
for (bit = 0; bit < bits; bit++)
	{
	hyperplane = projection + bit * dimension;
	dot = 0.0;
	for (d = 0; d < dimension; d++)
		dot += (double)hyperplane[d] * (double)vector[d];
	if (dot >= 0.0)
		out_signature[bit / 8] |= (unsigned char)(1 << (bit % 8));
	}
}

long long ANT_signature::hamming(const unsigned char *a, const unsigned char *b, long long bytes)
{
long long i, count = 0;
for (i = 0; i < bytes; i++)
	count += (long long)__builtin_popcount((unsigned int)(a[i] ^ b[i]));
return count;
}

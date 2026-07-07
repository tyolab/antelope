/*
	VECTOR_QUANTIZE.CPP
*/
#include <math.h>
#include "vector_quantize.h"

void ANT_vector_quantize::compute_ranges(const float *vectors, long long dimension, long long n, float *mins, float *maxs)
{
long long d, i;
for (d = 0; d < dimension; d++) { mins[d] = 0.0f; maxs[d] = 0.0f; }
if (n <= 0)
	return;
for (d = 0; d < dimension; d++) { mins[d] = vectors[d]; maxs[d] = vectors[d]; }
for (i = 1; i < n; i++)
	for (d = 0; d < dimension; d++)
		{
		float v = vectors[i * dimension + d];
		if (v < mins[d]) mins[d] = v;
		if (v > maxs[d]) maxs[d] = v;
		}
}

void ANT_vector_quantize::quantize(const float *vector, long long dimension, const float *mins, const float *maxs, signed char *codes)
{
long long d;
for (d = 0; d < dimension; d++)
	{
	float range = maxs[d] - mins[d];
	long q;
	if (range <= 0.0f)
		q = 0;
	else
		{
		double t = ((double)vector[d] - (double)mins[d]) / (double)range;
		q = (long)floor(t * 255.0 + 0.5) - 128;
		if (q < -128) q = -128;
		if (q > 127) q = 127;
		}
	codes[d] = (signed char)q;
	}
}

void ANT_vector_quantize::reconstruct(const signed char *codes, long long dimension, const float *mins, const float *maxs, float *out)
{
long long d;
for (d = 0; d < dimension; d++)
	{
	float range = maxs[d] - mins[d];
	if (range <= 0.0f)
		out[d] = mins[d];
	else
		out[d] = (float)((double)mins[d] + ((double)((int)codes[d] + 128) / 255.0) * (double)range);
	}
}

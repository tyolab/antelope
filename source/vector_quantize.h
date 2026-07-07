/*
	VECTOR_QUANTIZE.H -- per-dimension affine int8 quantization (V4).

	For each dimension d, values are mapped from [min_d, max_d] onto signed
	int8 [-128,127].  Magnitude is preserved per dimension (never per-vector),
	so dot/L2 ranking is unaffected.  A degenerate dimension (max==min)
	reconstructs to exactly min_d.  All functions are pure and deterministic.
*/
#ifndef VECTOR_QUANTIZE_H_
#define VECTOR_QUANTIZE_H_

class ANT_vector_quantize
{
public:
	static void compute_ranges(const float *vectors, long long dimension, long long n, float *mins, float *maxs);
	static void quantize(const float *vector, long long dimension, const float *mins, const float *maxs, signed char *codes);
	static void reconstruct(const signed char *codes, long long dimension, const float *mins, const float *maxs, float *out);
} ;

#endif /* VECTOR_QUANTIZE_H_ */

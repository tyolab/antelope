/*
	SIGNATURE.H -- index-wide SimHash: a fixed random-hyperplane projection
	(materialized in RAM from a seed) mapping a dense vector to a packed
	bit-signature whose Hamming distance tracks angular distance.  See
	docs/superpowers/specs/2026-07-06-vector-v2-signature-prefilter-design.md.
*/
#ifndef SIGNATURE_H_
#define SIGNATURE_H_

class ANT_signature
{
private:
	long long dimension;
	long long bits;					// signature width; multiple of 8
	float *projection;				// bits * dimension, row-major (one hyperplane per row)

public:
	ANT_signature(long long dimension, long long bits, unsigned long long seed);
	~ANT_signature();

	long long signature_bytes(void) { return (bits + 7) / 8; }
	void sign(const float *vector, unsigned char *out_signature);					// writes signature_bytes() bytes
	static long long hamming(const unsigned char *a, const unsigned char *b, long long bytes);
} ;

#endif /* SIGNATURE_H_ */

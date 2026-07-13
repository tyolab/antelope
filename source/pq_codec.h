/*
	PQ_CODEC.H -- product-quantization primitives (Phase 1). D-dim vectors split
	into m subvectors of D/m dims; per subspace a k=256 k-means codebook; a
	vector -> m code bytes. Deterministic (fixed seed + iters) so a rebuild is
	byte-identical. Pure/stateless; no file or engine dependencies.
*/
#ifndef PQ_CODEC_H_
#define PQ_CODEC_H_

class ANT_pq_codec
{
public:
	enum { K = 256, KMEANS_ITERS = 25 };
	enum { METRIC_DOT = 0, METRIC_COSINE = 1, METRIC_L2 = 2 };	// mirrors ANT_vector_store metrics

	static long train(const float *vectors, long long dimension, long long m, long long n, float *codebook);	// m must divide dimension (else 1); 0 ok
	static void encode(const float *vector, long long dimension, long long m, const float *codebook, unsigned char *codes);
	static void adc_table(const float *query, long long dimension, long long m, const float *codebook, long metric, double *table);
	static double adc_score(const unsigned char *codes, long long m, const double *table);
	static void reconstruct(const unsigned char *codes, long long dimension, long long m, const float *codebook, float *out);

	// OPQ (#22): learn an orthogonal D*D row-major rotation R (metric-preserving, no centering).
	// train_rotation returns 1 on m-does-not-divide-dimension or n==0 (caller leaves OPQ off).
	static long train_rotation(const float *vectors, long long dimension, long long m, long long n, float *R);
	static void apply_rotation(const float *vec, long long dimension, const float *R, float *out);            // out = R * vec
	static void apply_rotation_transpose(const float *vec, long long dimension, const float *R, float *out);  // out = R^T * vec
};
#endif /* PQ_CODEC_H_ */

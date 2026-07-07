/*
	MULTIVECTOR_STORE.H -- ragged per-document multi-vectors (V5 late interaction).
	Each document holds a variable count M_d >= 0 of `dimension`-D vectors.
	Vectors are L2-normalized on write; MaxSim over normalized vectors is a dot product.
	int8 pool reuses V4 ANT_vector_quantize (per-dimension ranges over the whole pool).
*/
#ifndef MULTIVECTOR_STORE_H_
#define MULTIVECTOR_STORE_H_

class ANT_multivector_store
{
private:
	long long dimension;
	long long documents;
	long long total_vectors;
	int quantized;
	int *counts;
	long long *offsets;
	float *pool_f;
	signed char *pool_q;
	float *qmin;
	float *qmax;
	ANT_multivector_store();

public:
	~ANT_multivector_store();
	static ANT_multivector_store *load(const char *filename, long long expected_dimension, long long expected_documents);
	long long get_dimension(void) { return dimension; }
	long long document_count(void) { return documents; }
	long has(long long docid) { return docid >= 0 && docid < documents && counts != NULL && counts[docid] > 0; }
	long long vector_count(long long docid) { return has(docid) ? counts[docid] : 0; }
	double maxsim(long long docid, const float *query_vecs, long long num_query_vecs);
	long long copy_vectors(long long docid, float *out);	// fills out[M_d*dim] with this doc's (reconstructed, normalized) vectors; returns M_d (0 if absent)
	long long max_vector_count(void);						// largest M_d over all docs (for sizing a buffer)
} ;

class ANT_multivector_store_writer
{
public:
	enum { QUANT_OFF = 0, QUANT_INT8 = 1 };

private:
	char *filename;
	long long dimension;
	int quant_mode;
	float *buffer;
	long long buffer_capacity;
	long long total_vectors;
	int *counts;
	long long counts_capacity;
	long long documents;

public:
	ANT_multivector_store_writer();
	~ANT_multivector_store_writer();
	long create(const char *path, long long dim);
	void set_quantization(int mode) { quant_mode = mode; }
	long append(const float *vectors, long long num_vectors);
	long finish(void);
	void abandon(void);
} ;

#endif /* MULTIVECTOR_STORE_H_ */

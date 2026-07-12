/*
	MULTIVECTOR_STORE.H -- ragged per-document multi-vectors (V5 late interaction).
	Each document holds a variable count M_d >= 0 of `dimension`-D vectors.
	Vectors are L2-normalized on write; MaxSim over normalized vectors is a dot product.
	int8 pool reuses V4 ANT_vector_quantize (per-dimension ranges over the whole pool).
*/
#ifndef MULTIVECTOR_STORE_H_
#define MULTIVECTOR_STORE_H_

#include "vector_source.h"

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

	/*
		V6: token-level accessors over the flattened pool, so ANT_hnsw can index
		individual tokens (node == token index 0..total_vectors-1).
	*/
	long long token_count(void) { return total_vectors; }
	long tokens_quantized(void) { return quantized; }
	long token_has(long long t) { return t >= 0 && t < total_vectors; }
	const float *token_get(long long t);
	void token_reconstruct(long long t, float *out);
	double token_score(long long t, const float *query, long metric);
	long long token_docid_of(long long t);
} ;

/*
	ANT_MULTIVECTOR_SOURCE
	----------------------
	Adapts an ANT_multivector_store's flattened token pool to the
	ANT_vector_source interface, so ANT_hnsw can build/search a graph over
	individual tokens rather than whole documents.  Node index == token index.
*/
class ANT_multivector_source : public ANT_token_source
{
private:
	ANT_multivector_store *store;

public:
	ANT_multivector_source(ANT_multivector_store *s) : store(s) {}
	long long document_count(void) override { return store->token_count(); }
	long long get_dimension(void) override { return store->get_dimension(); }
	long has(long long node) override { return store->token_has(node); }
	const float *get(long long node) override { return store->token_get(node); }
	long is_quantized(void) override { return store->tokens_quantized(); }
	void reconstruct(long long node, float *out) override { store->token_reconstruct(node, out); }
	double score(long long node, const float *query, long metric) override { return store->token_score(node, query, metric); }
	long long num_documents(void) override { return store->document_count(); }
	long long token_docid_of(long long t) override { return store->token_docid_of(t); }
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

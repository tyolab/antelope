/*
	MULTIVECTOR_PQ_STORE.H -- per-segment PQ-compressed token pool (seg_G.mvpq).
	Ragged, self-contained sibling of ANT_multivector_store: one k=256 codebook
	per segment over the whole token pool, m code bytes per token. Exposes the
	V6 token accessors + ADC-MaxSim scoring. Forgiving load: any validation failure
	-> degraded empty store (token_count()==0) so the segment falls back to the
	.mvec float/int8 pool.
*/
#ifndef MULTIVECTOR_PQ_STORE_H_
#define MULTIVECTOR_PQ_STORE_H_

class ANT_multivector_pq_store
{
private:
	long long dimension, documents, total_tokens, m;
	long metric;
	int *counts;			// documents ints, NULL when empty
	long long *offsets;		// documents+1 prefix sums, NULL when empty
	float *codebook;		// m*256*(dimension/m), NULL when empty
	unsigned char *codes;	// total_tokens*m, NULL when empty
	ANT_multivector_pq_store();
public:
	~ANT_multivector_pq_store();
	static ANT_multivector_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric);
	long long get_m(void) { return m; }
	long long get_dimension(void) { return dimension; }
	long long document_count(void) { return documents; }
	long long token_count(void) { return total_tokens; }
	long tokens_quantized(void) { return 1; }
	long has(long long docid) { return docid >= 0 && docid < documents && counts != 0 && counts[docid] > 0; }
	long long vector_count(long long docid) { return has(docid) ? counts[docid] : 0; }
	long long max_vector_count(void);
	long token_has(long long t) { return t >= 0 && t < total_tokens; }
	const unsigned char *token_codes(long long t) { return token_has(t) ? codes + t*m : 0; }
	void token_reconstruct(long long t, float *out);
	double token_score(long long t, const float *query, long metric_ignored);	// ADC; metric is the store's
	long long token_docid_of(long long t);
	double maxsim(long long docid, const float *query_vecs, long long num_query_vecs);
};

class ANT_multivector_pq_store_writer
{
private:
	char *filename; long long dimension, m; long metric;
	float *buffer; long long capacity, total_tokens;
	int *counts; long long counts_capacity, documents;
public:
	ANT_multivector_pq_store_writer(); ~ANT_multivector_pq_store_writer();
	long create(const char *path, long long dim, long long m, long metric);
	long append(const float *vectors, long long num_vectors);	// one doc's M_d normalized rows; append(NULL,0) for a doc with no tokens
	long finish(void);		// trains codebook over the whole pool + encodes + writes atomically
	void abandon(void);
};
#endif /* MULTIVECTOR_PQ_STORE_H_ */

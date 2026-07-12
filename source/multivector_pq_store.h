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

#include "vector_source.h"

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
	long long adc_table_builds;	// diagnostic-only, NOT thread-safe: # of ADC-table builds (token_score + token_prepare_query)

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

	void  *token_prepare_query(const float *query);						// build the m*K ADC table once; caller frees via token_free_query
	double token_score_prepared(long long t, const float *query, void *ctx);	// ADC via a prepared table; ctx==NULL -> falls back to token_score(t,query,metric)
	void   token_free_query(void *ctx);
	const float *get_codebook(void) { return codebook; }
	long get_metric(void) { return metric; }
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

/*
	ANT_MULTIVECTOR_PQ_SOURCE
	-------------------------
	Adapts an ANT_multivector_pq_store's flattened PQ-coded token pool to the
	ANT_token_source interface, so ANT_hnsw can build/search a token graph over
	PQ codes (ADC scoring) instead of resident float vectors. Node index ==
	token index. get() returns NULL (no resident float vector to hand back);
	the #26 prepare-per-query seam is wired to token_prepare_query/
	token_score_prepared/token_free_query so each query token builds its ADC
	table once per search rather than once per candidate.
*/
class ANT_multivector_pq_source : public ANT_token_source
{
private:
	ANT_multivector_pq_store *store;
public:
	ANT_multivector_pq_source(ANT_multivector_pq_store *s) : store(s) {}
	long long document_count(void) override { return store->token_count(); }
	long long get_dimension(void) override { return store->get_dimension(); }
	long has(long long node) override { return store->token_has(node); }
	const float *get(long long node) override { (void)node; return 0; }
	long is_quantized(void) override { return 1; }
	void reconstruct(long long node, float *out) override { store->token_reconstruct(node, out); }
	double score(long long node, const float *query, long metric) override { return store->token_score(node, query, metric); }
	long long num_documents(void) override { return store->document_count(); }
	long long token_docid_of(long long t) override { return store->token_docid_of(t); }
	void  *prepare_query(const float *query, long metric) override { (void)metric; return store->token_prepare_query(query); }
	double score_prepared(long long node, const float *query, long metric, void *ctx) override { (void)metric; return store->token_score_prepared(node, query, ctx); }
	void   free_query(void *ctx) override { store->token_free_query(ctx); }
} ;
#endif /* MULTIVECTOR_PQ_STORE_H_ */

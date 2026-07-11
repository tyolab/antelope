/*
	VECTOR_SOURCE.H -- read-only "N points of dimension D" abstraction that
	ANT_hnsw builds/searches over. Implemented by ANT_vector_store (node == docid)
	and (V6) ANT_multivector_source (node == token in the flattened .mvec pool).
*/
#ifndef VECTOR_SOURCE_H_
#define VECTOR_SOURCE_H_

class ANT_vector_source
{
public:
	virtual ~ANT_vector_source() {}
	virtual long long document_count(void) = 0;
	virtual long long get_dimension(void) = 0;
	virtual long has(long long node) = 0;
	virtual const float *get(long long node) = 0;
	virtual long is_quantized(void) = 0;
	virtual void reconstruct(long long node, float *out) = 0;
	virtual double score(long long node, const float *query, long metric) = 0;

	/* Optional per-query precomputation. Default: no-op — score_prepared ignores ctx and
	   calls score(). A source with a per-query-precomputable structure (PQ's ADC table)
	   overrides all three: prepare_query builds it once, score_prepared reuses it, free_query
	   releases it. Caller contract: prepare_query(q,metric) -> ctx; every score_prepared for
	   that search passes the SAME q and metric; free_query(ctx) is called exactly once. */
	virtual void  *prepare_query(const float *query, long metric) { (void)query; (void)metric; return 0; }
	virtual double score_prepared(long long node, const float *query, long metric, void *ctx)
	                    { (void)ctx; return score(node, query, metric); }
	virtual void   free_query(void *ctx) { (void)ctx; }
} ;
#endif /* VECTOR_SOURCE_H_ */

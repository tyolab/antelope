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
} ;
#endif /* VECTOR_SOURCE_H_ */

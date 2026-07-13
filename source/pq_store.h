/*
	PQ_STORE.H -- per-segment PQ-compressed dense vectors (seg_G.pq). Implements
	ANT_vector_source so ANT_hnsw and the ADC scan share one backend. Forgiving
	load: any validation failure -> degraded empty store (fallback to float/int8).
*/
#ifndef PQ_STORE_H_
#define PQ_STORE_H_
#include "vector_source.h"

class ANT_index_tombstones;
struct ANT_vector_candidate;

enum { PQ_SCORE_STACK_CAP = 8 * 256 };		// 2048 doubles (16 KB) inline in score(); heap above this

class ANT_pq_store : public ANT_vector_source
{
private:
	long long dimension, documents, m;
	long metric;
	unsigned char *presence;	// (documents+7)/8, NULL when empty
	float *codebook;			// m*K*(dimension/m), NULL when empty
	unsigned char *codes;		// documents*m, NULL when empty
	float *rotation;			// D*D OPQ rotation R (row-major), NULL when OPQ off
	ANT_pq_store();
public:
	long long adc_table_builds;	// diagnostic-only, NOT thread-safe: # of adc_table() builds (score() + prepare_query); test proves the seam engaged
	~ANT_pq_store();
	static ANT_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric);
	long long get_m(void) { return m; }
	long long document_count(void) override { return documents; }
	long long get_dimension(void) override { return dimension; }
	long has(long long docid) override { return presence != 0 && docid >= 0 && docid < documents && (presence[docid/8] & (1 << (docid%8))) != 0; }
	const float *get(long long docid) override { (void)docid; return 0; }
	long is_quantized(void) override { return 1; }
	void reconstruct(long long docid, float *out) override;
	double score(long long docid, const float *query, long metric) override;
	void  *prepare_query(const float *query, long metric) override;
	double score_prepared(long long docid, const float *query, long metric, void *ctx) override;
	void   free_query(void *ctx) override;
	const unsigned char *codes_for(long long docid) { return has(docid) ? codes + docid*m : 0; }
	const float *get_codebook(void) { return codebook; }

	// top-k docids by ADC kernel (higher=better), honoring presence, tombstones, and an
	// optional docid filter bitset. Builds the m*K ADC table once, then one pass over docs.
	void scan_adc(const float *query, long metric, ANT_index_tombstones *tombstones, long long generation,
		ANT_vector_candidate *best, long long *best_count, long long top_k, const unsigned char *filter_bits);
};

class ANT_pq_store_writer
{
private:
	char *filename; long long dimension, m; long metric; long opq;
	float *buffer; long long capacity, documents; unsigned char *presence; long long presence_capacity;
public:
	ANT_pq_store_writer(); ~ANT_pq_store_writer();
	long create(const char *path, long long dim, long long m, long metric, long opq);
	long append(const float *vector_or_null);
	long finish(void);
	void abandon(void);
};
#endif /* PQ_STORE_H_ */

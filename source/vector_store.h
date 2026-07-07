/*
	VECTOR_STORE.H
	--------------
	Per-segment dense-vector sidecar (seg_G.vec) for the segmented incremental
	index (see docs/superpowers/specs/2026-07-06-hybrid-vector-search-design.md).

	On-disk format:
		uint64  magic ("ANTVEC01")
		int64   dimension
		int64   document_count
		byte[]  presence bitmap ((document_count + 7) / 8 bytes)
		float[] vectors (document_count * dimension, dense, absent rows zeroed)

	Loads validate magic/dimension/count/file size; any failure degrades to an
	empty store (no vectors) rather than failing the segment -- the same
	forgiving posture as a missing .del file.

	Score conventions: dot and cosine return the raw dot product (cosine is dot
	on pre-normalized data, normalized by the caller); L2 returns the NEGATED
	squared distance so "higher is better" holds for every metric.
*/
#ifndef VECTOR_STORE_H_
#define VECTOR_STORE_H_

class ANT_index_tombstones;

/*
	One entry in a top-k candidate set, shared by disk-store scans and the
	coordinator's memory-buffer scan.
*/
struct ANT_vector_candidate
{
double score;
long long generation;
long long docid;
} ;

/*
	ANT_VECTOR_CANDIDATE_INSERT()
	-----------------------------
	Replace-min insertion into a fixed-capacity candidate set (O(k) per call).
*/
void ANT_vector_candidate_insert(ANT_vector_candidate *best, long long *best_count, long long top_k, double score, long long generation, long long docid);

class ANT_vector_store
{
public:
	enum { METRIC_DOT = 0, METRIC_COSINE = 1, METRIC_L2 = 2 };

private:
	long long dimension;
	long long documents;
	unsigned char *presence;		// NULL when degraded/empty
	float *vectors;					// NULL when degraded/empty
	signed char *codes;			// int8 codes when quantized; NULL otherwise
	float *qmin;				// [dimension] per-dimension min (quantized only)
	float *qmax;				// [dimension] per-dimension max (quantized only)
	int quantized;				// 1 if this store is int8-backed

private:
	ANT_vector_store();

public:
	~ANT_vector_store();

	static ANT_vector_store *load(const char *filename, long long expected_dimension, long long expected_documents);

	long long document_count(void) { return documents; }
	long long get_dimension(void) { return dimension; }
	long has(long long docid) { return presence != NULL && (presence[docid / 8] & (1 << (docid % 8))) != 0; }
	const float *get(long long docid) { return vectors + docid * dimension; }

	long is_quantized(void) { return quantized; }
	void reconstruct(long long docid, float *out);
	double score(long long docid, const float *query, long metric);

	void scan(const float *query, long metric, ANT_index_tombstones *tombstones, long long generation, ANT_vector_candidate *best, long long *best_count, long long top_k);

	static long normalize(float *vector, long long dimension);	// in place; nonzero if magnitude is zero
	static double kernel(const float *a, const float *b, long long dimension, long metric);
} ;

/*
	class ANT_VECTOR_STORE_WRITER
	-----------------------------
	Buffered writer with atomic finish (write .tmp, rename).
*/
class ANT_vector_store_writer
{
public:
	enum { QUANT_OFF = 0, QUANT_REPLACE = 1, QUANT_BOTH = 2 };	// BOTH is used later (exact mode); implement OFF and REPLACE now

private:
	char filename[4096];
	long long dimension;
	long long documents;
	long long capacity;
	unsigned char *presence;
	float *vectors;
	int quant_mode;

private:
	long grow(void);
	long write_qvec(const char *path);

public:
	ANT_vector_store_writer();
	~ANT_vector_store_writer();

	long create(const char *filename, long long dimension);		// 0 on success
	long append(const float *vector_or_null);					// 0 on success
	long finish(void);											// writes + renames; 0 on success
	long finish_qvec(const char *qvec_path) { return write_qvec(qvec_path); }		// exact mode: write the int8 sibling AFTER finish() writes the float .vec
	void abandon(void);											// discard without writing
	void set_quantization(int mode) { quant_mode = mode; }
} ;

#endif /* VECTOR_STORE_H_ */

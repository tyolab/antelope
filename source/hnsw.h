/*
	HNSW.H -- hand-rolled hierarchical navigable small-world graph over a
	segment's dense vectors.  Topology only: per-node level + per-layer
	neighbour docids in a flat CSR layout.  Vectors + distance come from an
	ANT_vector_store passed to build()/search(); the graph never owns vectors.
	Works in distance = -kernel (lower = nearer) so textbook HNSW applies; see
	docs/superpowers/specs/2026-07-07-vector-v3-hnsw-design.md.
*/
#ifndef HNSW_H_
#define HNSW_H_

class ANT_vector_source;
class ANT_index_tombstones;

#define ANT_HNSW_SEED 0x1234567890ABCDEFULL
#define ANT_HNSW_MAX_DOCUMENTS 0x7FFFFFFFLL   /* INT_MAX: docids stored int32 in neighbours[], so cap node count */
#define ANT_HNSW_DISTANCE_CACHE_MIN_DIM 192   /* below this, distance() is cheaper than a cache probe -- build() skips the memo */

class ANT_hnsw
{
private:
	long long documents;			// node slots == vector store document_count
	long long M;
	long long M0;					// 2*M, the layer-0 degree cap
	long long ef_construction;
	long long entry_point;			// docid at the top of the hierarchy; -1 if empty
	long long max_level;
	int *levels;					// [documents]; per-node top level, -1 if not in graph
	long long *offsets;				// [documents+1]; prefix sums into neighbours[] (int units)
	int *neighbours;				// CSR stream: per node, per layer 0..level: [count][docid...]

	/* build helpers (defined in the .cpp) */
	double distance(long long a, const float *query, ANT_vector_source *vectors, long metric, void *ctx = NULL);

public:
	ANT_hnsw();
	~ANT_hnsw();

	long long node_count(void) { return documents; }
	long long get_M(void) { return M; }
	long long get_ef_construction(void) { return ef_construction; }
	long empty(void) { return entry_point < 0; }

	/* Build the graph over every present vector in `vectors` (docid order,
	   deterministic).  Returns 0 on success, nonzero on allocation failure. */
	long build(ANT_vector_source *vectors, long long M, long long ef_construction, long metric, bool use_distance_cache = true);

	/* Search for the top_k highest-kernel present, non-tombstoned docids for
	   `query`.  Fills out_docids/out_scores (scores = kernel, descending),
	   returns the count (<= top_k).  ef_search is clamped to >= top_k. */
	long long search(const float *query, long metric, long long ef_search, long long top_k,
		ANT_vector_source *vectors, ANT_index_tombstones *tombstones,
		long long *out_docids, double *out_scores, const unsigned char *filter_bits = NULL);

	/* Persist the CSR to a seg_G.hnsw sidecar; 0 on success. */
	long save(const char *filename);
	/* Forgiving factory: read a sidecar back, degrading to an empty graph on ANY
	   corruption or config mismatch.  Caller owns the returned pointer (delete). */
	static ANT_hnsw *load(const char *filename, long long expected_M, long long expected_ef_construction, long long expected_documents);
} ;

#endif /* HNSW_H_ */

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

class ANT_vector_store;
class ANT_index_tombstones;

#define ANT_HNSW_SEED 0x1234567890ABCDEFULL

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
	double distance(long long a, const float *query, ANT_vector_store *vectors, long metric);

public:
	ANT_hnsw();
	~ANT_hnsw();

	long long node_count(void) { return documents; }
	long long get_M(void) { return M; }
	long long get_ef_construction(void) { return ef_construction; }
	long empty(void) { return entry_point < 0; }

	/* Build the graph over every present vector in `vectors` (docid order,
	   deterministic).  Returns 0 on success, nonzero on allocation failure. */
	long build(ANT_vector_store *vectors, long long M, long long ef_construction, long metric);

	/* Search for the top_k highest-kernel present, non-tombstoned docids for
	   `query`.  Fills out_docids/out_scores (scores = kernel, descending),
	   returns the count (<= top_k).  ef_search is clamped to >= top_k. */
	long long search(const float *query, long metric, long long ef_search, long long top_k,
		ANT_vector_store *vectors, ANT_index_tombstones *tombstones,
		long long *out_docids, double *out_scores);

	/* Task 2 will add: save()/load()/adopt of the CSR arrays. */
	friend class ANT_hnsw_serialiser;		// Task 2 hook; harmless now
} ;

#endif /* HNSW_H_ */

/*
	TOKEN_INDEX.H -- V6 token-level ANN. Owns an ANT_hnsw over a .mvec store's
	flattened token pool (nodes == tokens) plus a token->docid map, turning
	query tokens into candidate documents. See docs/superpowers/specs/2026-07-08-vector-v6-token-ann-design.md.
*/
#ifndef TOKEN_INDEX_H_
#define TOKEN_INDEX_H_

class ANT_hnsw;
class ANT_multivector_store;

class ANT_token_index
{
private:
	ANT_hnsw *graph;          // over the flattened token pool (nodes == tokens)
	int *token_docid;         // [token_count] node->docid map (int32; token_count <= INT_MAX)
	long long token_count;
	long long documents;
	long long dimension;
	long metric;
	long long M;
	long long ef_construction;
	ANT_multivector_store *store;   // borrowed (not owned) -- do NOT delete in dtor
	ANT_token_index();
public:
	~ANT_token_index();
	// Build over `store` (borrowed, not retained). 0 tokens -> returns NULL (nothing to index).
	// Returns NULL on token_count > INT_MAX cap or graph/alloc failure (caller falls back to brute force).
	static ANT_token_index *build(ANT_multivector_store *store, long long M, long long ef_construction, long metric);
	long long get_token_count(void) { return token_count; }
	long long get_documents(void) { return documents; }
	long empty(void);                        // no graph / empty graph
	long token_docid_at(long long t);        // test/debug accessor: token_docid[t], or -1 out of range

	/* Persist the token->docid map + header to `filename`, and the graph
	   topology to `filename.g` (via ANT_hnsw::save).  0 on success. */
	long save(const char *filename);
	/* Forgiving factory: read the sidecar pair back, degrading to an empty
	   index on ANY corruption or config/store mismatch.  Caller owns the
	   returned pointer (delete); `store` is borrowed and retained (not owned). */
	static ANT_token_index *load(const char *filename, ANT_multivector_store *store, long long expected_M, long long expected_ef_construction, long metric);
};
#endif /* TOKEN_INDEX_H_ */

/*
	INDEX_MERGE.H
	-------------
	Tombstone-aware N-way segment merger for the segmented incremental index
	(see docs/superpowers/specs/2026-07-06-compacting-merge-design.md).

	ANT_docid_renumberer maps (segment ordinal, old docid) to the dense new
	docid a compacted output segment assigns, or -1 for tombstoned documents.
*/
#ifndef INDEX_MERGE_H_
#define INDEX_MERGE_H_

#include "fundamental_types.h"
#include "compress.h"

class ANT_index_tombstones;
class ANT_search_engine;
class ANT_search_engine_btree_leaf;
class ANT_memory_index_hash_node;
class ANT_stats_memory_index;
class ANT_memory;
class ANT_compression_factory;
class ANT_file;

class ANT_docid_renumberer
{
private:
	long long **new_docid;			// per segment, per old docid: new docid or -1
	long long *documents;			// per segment: old document count
	long long segment_count;
	long long live_documents;

public:
	ANT_docid_renumberer(ANT_index_tombstones **tombstones, long long *document_counts, long long segment_count);
	~ANT_docid_renumberer();

	long long renumber(long long segment, long long old_docid) { return new_docid[segment][old_docid]; }
	long long total_live_documents(void) { return live_documents; }
	long long live_in_segment(long long segment);
} ;

/*
	class ANT_INDEX_MERGER
	----------------------
	In-process adaptation of atire_merge.cpp's merge_index(), specialized to
	Phase 1 segments: non-quantized, unpruned, no stored documents,
	FILENAME_INDEX + IMPACT_HEADER build.  All failures return nonzero and
	delete the partial output; no exit().  merge() is re-entrant: every
	allocation it makes is freed before it returns, so a single instance may
	drive many sequential merges (maintain() runs several per invocation).
*/
class ANT_index_merger
{
private:
	/*
		Growth buffer for compressed postings (was file-scope global +
		function-local static in atire_merge.cpp; here reusable scratch owned by
		the instance so the helpers need no globals and no cross-call statics).
	*/
	unsigned char *postings_list;
	long long postings_list_size;
	long long header_buffer_size;
	unsigned char *compressed_impact_header_buffer;

	/*
		Per-merge state (reset at the start of every merge()).
	*/
	long long longest_postings;
	int32_t longest_term;
	int64_t highest_df;
	long long terms_so_far;
	ANT_docid_renumberer *renumberer;
	ANT_compression_factory *factory;
	ANT_memory *node_memory;
	ANT_stats_memory_index *memory_stats;

private:
	/* adapted helpers -- same names as their atire_merge.cpp sources */
	ANT_memory_index_hash_node **find_end_of_node(ANT_memory_index_hash_node **start);
	ANT_memory_index_hash_node **write_node(ANT_file *file, ANT_memory_index_hash_node **start);
	ANT_memory_index_hash_node *write_postings(char *term, ANT_compressable_integer *raw, ANT_file *index, ANT_search_engine_btree_leaf *leaf, long long output_documents);
	ANT_memory_index_hash_node *write_impact_header_postings(char *term, ANT_compressable_integer *header, ANT_compressable_integer quantum_count, ANT_compressable_integer *raw, ANT_file *index, ANT_search_engine_btree_leaf *leaf, long long output_documents);
	ANT_memory_index_hash_node *write_variable(const char *name, long long value, ANT_file *index, ANT_search_engine_btree_leaf *leaf, long long output_documents);
	void grow_postings_buffer(unsigned char **postings_ptr);

public:
	ANT_index_merger();
	~ANT_index_merger();

	/*
		Merge the given open engines (with their tombstones) into a new index
		file at output_filename.  0 on success; on failure the partial output
		is removed.  Inputs are unmodified.
	*/
	long merge(ANT_search_engine **engines, ANT_index_tombstones **tombstones, long long engine_count, const char *output_filename);
} ;

#endif /* INDEX_MERGE_H_ */

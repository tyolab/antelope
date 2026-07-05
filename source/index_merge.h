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

class ANT_index_tombstones;

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

#endif /* INDEX_MERGE_H_ */

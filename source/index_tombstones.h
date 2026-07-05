/*
	INDEX_TOMBSTONES.H
	------------------
	Per-segment deletion bitmap.  Documents are marked deleted here rather than
	removed from the (immutable) postings; deleted docids are filtered from
	results at query time and physically dropped at compaction (Phase 2).
*/
#ifndef INDEX_TOMBSTONES_H_
#define INDEX_TOMBSTONES_H_

class ANT_index_tombstones
{
private:
	unsigned char *bitmap;
	long long bitmap_bytes;			// allocated size in bytes
	long long deleted_documents;

private:
	void grow_to(long long docid);

public:
	ANT_index_tombstones(long long documents);
	~ANT_index_tombstones();

	void set_deleted(long long docid);
	long is_deleted(long long docid);
	long long count(void) { return deleted_documents; }

	long save(const char *filename);							// 0 on success; write-temp + rename
	static ANT_index_tombstones *load(const char *filename, long long documents);	// empty bitmap if file absent
} ;

#endif /* INDEX_TOMBSTONES_H_ */

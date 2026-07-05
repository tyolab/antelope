/*
	ATIRE_SEGMENT_INDEX.H
	---------------------
	A growable, updatable index composed of immutable disk segments plus one
	live in-memory writing segment, per
	docs/superpowers/specs/2026-07-05-incremental-index-design.md.

	Single-threaded (like the rest of ATIRE).  Compaction is Phase 2.
*/
#ifndef ATIRE_SEGMENT_INDEX_H_
#define ATIRE_SEGMENT_INDEX_H_

class ATIRE_API;
class ATIRE_indexer;
class ANT_index_manifest;
class ANT_index_keymap;
class ANT_index_tombstones;

class ATIRE_segment_index
{
public:
	struct hit
	{
	long long generation;		// segment the document lives in
	long long docid;			// docid local to that segment
	char *filename;				// the external key; owned by the index, valid until the next search
	double score;
	} ;

	/*
		A disk segment open for searching
	*/
	struct segment
	{
	long long generation;
	ATIRE_API *engine;
	ANT_index_tombstones *tombstones;
	} ;

private:
	char *directory;
	ANT_index_manifest *manifest;
	ANT_index_keymap *keymap;

	segment *segments;
	long long segment_count, segments_allocated;

	/*
		The live in-memory writing segment
	*/
	ATIRE_indexer *writer;
	long long writer_generation;
	long long writer_documents;
	ANT_index_tombstones *writer_tombstones;
	ATIRE_API *writer_engine;			// NRT view over writer's memory index; rebuilt when stale
	long writer_engine_stale;

	long long flush_after_documents;	// 0 = only flush manually

	hit *results;
	long long results_count, results_allocated;

private:
	long start_new_writer(void);		// 0 on success, 1 if the manifest cannot be saved
	void rebuild_writer_engine(void);
	void search_one_segment(ATIRE_API *engine, ANT_index_tombstones *tombstones, long long generation, char *query, long long top_k);
	long append_segment(long long generation);
	void segment_filename(char *buffer, long long buffer_size, long long generation, const char *extension);

public:
	ATIRE_segment_index();
	~ATIRE_segment_index();

	long open(const char *directory);						// 0 on success

	long long add_document(const char *key, const char *document);		// returns handle, -1 on error
	long long update_document(const char *key, const char *document);	// upsert; returns new handle (Task 9)
	long delete_document(const char *key);								// 0 on success, 1 if key unknown (Task 9)

	long flush(void);										// memory segment -> disk segment; 0 on success (Task 7)
	void set_flush_threshold(long long documents) { flush_after_documents = documents; }

	long long search(char *query, long long top_k);			// returns number of hits stored
	hit *get_hit(long long which) { return &results[which]; }

	long long get_document_count(void);						// live (non-tombstoned) documents

	static long long make_handle(long long generation, long long docid) { return (generation << 40) | docid; }
} ;

#endif /* ATIRE_SEGMENT_INDEX_H_ */

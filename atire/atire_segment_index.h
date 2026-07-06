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
class ANT_search_engine;
class ANT_vector_store;
class ANT_write_ahead_log;
struct ANT_vector_candidate;

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
	ANT_vector_store *vectors;		// NULL when vectors are disabled for this index
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

	long long flush_after_documents;	// 0 = manual flush only; default 10000 bounds memory growth of the live segment

	long global_stats_enabled;			// 1 (default): push global N/mean into every open engine so scores match a single-segment index; 0: each segment scores with its own local statistics

	long merge_factor;					// tier trigger: a size tier with >= this many disk segments gets merged
	double tombstone_compact_ratio;	// tombstone trigger: a segment whose tombstones/documents exceeds this gets rewritten
	long auto_maintain;					// 0 = off (Phase 1 behaviour): flush() does not call maintain() unless set

	hit *results;
	long long results_count, results_allocated;

	long long vector_dimension_current;		// 0 = vectors disabled
	long vector_metric;
	long long pending_vector_dimension;		// set_vector_config before open
	long pending_vector_metric;
	long vector_config_pending;

	/* memory-segment vector buffer, parallel to the writer's docids */
	float *writer_vector_data;
	unsigned char *writer_vector_presence;
	long long writer_vector_capacity;
	long long writer_vectors_present;		// how many docs in the buffer HAVE vectors

	/*
		Durable (WAL) mode.  wal is NULL unless set_durable(1) was called
		before open().  wal_fsync_pending records a set_wal_fsync() call
		made before open() (mirrors the vector_config_pending style) so it
		can be applied once the log is actually open.  wal_replaying
		suppresses the append hooks in add_document/update_document/
		delete_document while open() is replaying the log through those
		same public methods.  wal_suppress_add suppresses the 'A' append
		that update_document's internal add_document() call would otherwise
		log, so an update logs exactly one 'U' record.  wal_truncate_pending
		defers flush()'s truncate() when a flush happens to fire WHILE a
		replay is in progress (auto-flush partway through a long replay) --
		truncating mid-replay would reopen the log out from under
		replay_next()'s file position and silently drop the untouched tail;
		instead flush() records the deferral and open() truncates once,
		after the whole replay has been consumed.
	*/
	long durable;
	long wal_fsync_pending;
	ANT_write_ahead_log *wal;
	long wal_replaying;
	long wal_suppress_add;
	long wal_truncate_pending;

private:
	long start_new_writer(void);		// 0 on success, 1 if the manifest cannot be saved
	void rebuild_writer_engine(void);
	void refresh_global_statistics(void);	// push global N/mean (or the restore sentinel) into every open engine + rebuild their ranking functions
	/*
		use_filename_index selects the accessor used to fetch each hit's external
		key: disk segments are reopened as ANT_V5 (this build always serialises
		FILENAME_INDEX-style, see atire_segment_index.cpp/append_segment) so they
		must be read with ATIRE_API::get_document_filename() (the filename-index
		accessor, into a caller-supplied buffer); the writer's NRT view is wired
		up via open_from_memory_index(), which forces ant_version = ANT_V3, so it
		must be read with get_document_filename_from_doclist() (the in-memory
		doc_list accessor) instead.
	*/
	void search_one_segment(ATIRE_API *engine, ANT_index_tombstones *tombstones, long long generation, char *query, long long top_k, long use_filename_index);
	long append_segment(long long generation);
	void segment_filename(char *buffer, long long buffer_size, long long generation, const char *extension);
	void delete_segment_files(long long generation);	// best-effort unlink of seg_G.aspt / seg_G.del
	long tombstone(long long generation, long long docid);		// 0 on success, 1 if the generation is unknown
	long rebuild_keymap(void);			// reconstruct the keymap from segments' stored filenames when keymap.log is lost; 0 on success, nonzero if a .del save fails

	long long add_document_core(const char *key, const char *document, const float *vector);	// shared body; vector may be NULL

	long load_vector_config(void);			// reads <dir>/vector.config; 0 = ok (absent is ok)
	long save_vector_config(void);			// atomic write; 0 on success
	long writer_vector_append(long long docid, const float *vector_or_null);
	void reset_writer_vectors(void);

	long long vector_candidates(const float *query, long long top_k, ANT_vector_candidate *best);
	char *resolve_hit_filename(long long generation, long long docid, char *buffer, long long buffer_size);

	void reset_results(void);			// frees results[0, results_count)'s filenames and zeroes results_count
	hit *append_result(void);			// grows results[] (doubling, initial 256) if needed, then reserves and returns the next slot

public:
	enum { VECTOR_METRIC_DOT = 0, VECTOR_METRIC_COSINE = 1, VECTOR_METRIC_L2 = 2 };

	long set_vector_config(long long dimension, long metric);		// before open(); 0 on success
	long long vector_dimension(void) { return vector_dimension_current; }

	long set_durable(long on);				// before open(); 1 if already open; 0 on success -- enables the WAL
	void set_wal_fsync(long on);			// fsync() every WAL append when on; may be called before or after open()
	long wal_healthy(void);				// 1 when healthy OR disabled (no WAL); 0 when the last append failed

	ATIRE_segment_index();
	~ATIRE_segment_index();

	long open(const char *directory);						// 0 on success

	long long add_document(const char *key, const char *document);		// returns handle, -1 on error
	long long add_document(const char *key, const char *document, const float *vector);		// returns handle, -1 on error (also on vector rejection)
	long long update_document(const char *key, const char *document);	// upsert; returns new handle
	long long update_document(const char *key, const char *document, const float *vector);	// upsert; returns new handle
	long delete_document(const char *key);								// 0 on success, 1 if key unknown

	long flush(void);										// memory segment -> disk segment; 0 on success

	long compact(long long *input_generations, long long input_count);	// merge those disk segments into one; 0 on success

	long maintain(void);								// run the tiered merge policy to quiescence; 0 = success
	void set_global_stats(long on);		// enable (default) / disable cross-segment global ranking statistics; refreshes immediately if open
	void set_merge_factor(long segments_per_tier) { merge_factor = segments_per_tier; }
	void set_tombstone_compact_ratio(double ratio) { tombstone_compact_ratio = ratio; }
	void set_auto_maintain(long on) { auto_maintain = on; }
	long long disk_segment_count(void) { return segment_count; }
	long long disk_segment_generation(long long which) { return segments[which].generation; }
	ANT_search_engine *disk_segment_engine(long long which);

	/*
		Set the auto-flush threshold: add_document() calls flush() once the
		writer holds at least this many documents.  0 disables auto-flush
		(manual flush() only).  The constructor default (10000) exists because
		NRT rebuilds (rebuild_writer_engine()) grow the writer's arena
		quadratically until the writer is flushed, so an unbounded live
		segment is a real memory-growth hazard for long-running sessions that
		never call flush() themselves.

		Auto-flush is best-effort: a failed auto-flush degrades per flush()'s
		contract -- depending on the failure point the just-added document is
		either still in the live writer, or flushed-but-not-durable (lost to
		the orphan sweep on next open()).  Callers needing certainty must call
		flush() themselves and check its return.
	*/
	void set_flush_threshold(long long documents) { flush_after_documents = documents; }

	long long search(char *query, long long top_k);			// returns number of hits stored
	long long search_vector(const float *query, long long top_k);	// exact top-k across memory buffer + disk stores
	long long search_hybrid(char *query_text, const float *query_vector, long long top_k);	// RRF fusion of lexical + vector top-k; either side may be absent
	hit *get_hit(long long which) { return &results[which]; }

	long long get_document_count(void);						// live (non-tombstoned) documents

	static long long make_handle(long long generation, long long docid) { return (generation << 40) | docid; }
} ;

#endif /* ATIRE_SEGMENT_INDEX_H_ */

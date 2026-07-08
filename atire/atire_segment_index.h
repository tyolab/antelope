/*
	ATIRE_SEGMENT_INDEX.H
	---------------------
	A growable, updatable index composed of immutable disk segments plus one
	live in-memory writing segment, per
	docs/superpowers/specs/2026-07-05-incremental-index-design.md.

	Single-threaded (like the rest of ATIRE).  Compaction is Phase 2.

	The implementation is split across atire_segment_index*.cpp by feature:
	lifecycle + write path in atire_segment_index.cpp, and compaction, vectors,
	lexical search, and WAL/global-stats in the _compaction, _vector, _search,
	and _durability siblings.
*/
#ifndef ATIRE_SEGMENT_INDEX_H_
#define ATIRE_SEGMENT_INDEX_H_

#include "../source/attribute_store.h"
#include "../source/payload_store.h"

class ATIRE_API;
class ATIRE_indexer;
class ANT_index_manifest;
class ANT_index_keymap;
class ANT_index_tombstones;
class ANT_search_engine;
class ANT_memory_index;
class ANT_vector_store;
class ANT_multivector_store;
class ANT_write_ahead_log;
class ANT_signature;
class ANT_signature_store;
class ANT_hnsw;
class ANT_filter;
class ANT_token_index;
class ANT_pq_store;
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
	const unsigned char *payload;	// opaque per-doc blob, NULL when none; borrowed, valid until the next search/flush
	long long payload_length;		// 0 when none
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
	ANT_vector_store *exact_vectors;	// float32 resident for exact-mode rerank; NULL otherwise
	ANT_signature_store *signatures;	// NULL when absent/degraded/approximate-off
	ANT_hnsw *hnsw_graph;			// NULL when HNSW is not configured for this index
	ANT_multivector_store *multivectors;	// V5 late-interaction sidecar; NULL unless rerank configured
	ANT_token_index *token_index;		// V6 token-level ANN over multivectors' flattened token pool; NULL unless built/loadable
	ANT_attribute_store *attributes;	// filter columns; NULL unless attributes configured
	ANT_payload_store *payload;			// opaque per-doc blob; NULL unless attributes configured
	ANT_pq_store *pq_vectors;			// PQ-compressed dense store (seg_G.pq); NULL unless PQ configured AND a valid .pq loaded/built
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

	long long signature_bits_current;		// 0 = approximate not configured
	unsigned long long signature_seed;
	long long candidate_multiplier;			// rerank pool = top_k * this (default 4)
	ANT_signature *query_signer;			// index-wide projection, built at open when configured (NULL otherwise)

	long long hnsw_M_current;				// 0 = HNSW not configured
	long long hnsw_ef_construction_current;
	long long hnsw_ef_search;				// query knob; default 64

	long quantization_current;				// 0 = off (QUANTIZE_OFF)

	long long pq_m_current;				// 0 = PQ off
	long pq_posture_current;				// PQ_POSTURE_REPLACE / PQ_POSTURE_RERANK
	long pq_rerank_quant_current;			// RERANK_QUANT_FLOAT / RERANK_QUANT_INT8
	long pq_eager;							// 0 ondemand (default), 1 eager

	long long rerank_dimension_current;		// 0 = rerank not configured
	long rerank_quant_current;				// RERANK_QUANT_FLOAT / RERANK_QUANT_INT8

	long long token_index_M;					// V6 token-ANN graph fan-out (build_token_index, Task 9)
	long long token_index_ef_construction;
	long long token_top_p;						// per-query-token candidate shortlist width
	long token_index_eager;						// 0 = ondemand (default, build_token_index() backfill), 1 = eager (built automatically at flush)

	ANT_attribute_schema attribute_schema_current;		// count()==0 => not configured

	/* memory-segment vector buffer, parallel to the writer's docids */
	float *writer_vector_data;
	unsigned char *writer_vector_presence;
	long long writer_vector_capacity;
	long long writer_vectors_present;		// how many docs in the buffer HAVE vectors

	/* memory-segment multi-vector buffer, parallel to the writer's docids (Task 4: capture only) */
	float *writer_multivector_data;			// growing pool of pending docs' multi-vectors (normalized)
	long long writer_multivector_capacity;	// in vectors
	long long writer_multivector_total;		// vectors buffered so far
	int *writer_multivector_counts;			// M per writer docid
	long long writer_multivector_counts_capacity;	// in docs

	/* memory-segment attribute/payload capture, parallel to the writer's docids (Task 6: capture only) */
	ANT_attribute_set **writer_attribute_sets;		// per-docid deep-clone (NULL where a doc has no attributes); NULL array until first use
	long long writer_attribute_sets_capacity;		// in docs

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
	void search_one_segment(ATIRE_API *engine, ANT_index_tombstones *tombstones, long long generation, char *query, long long top_k, long use_filename_index, const unsigned char *filter_bits = NULL);
	long append_segment(long long generation);
	void segment_filename(char *buffer, long long buffer_size, long long generation, const char *extension);
	void delete_segment_files(long long generation);	// best-effort unlink of seg_G.aspt / seg_G.del
	long tombstone(long long generation, long long docid);		// 0 on success, 1 if the generation is unknown
	long rebuild_keymap(void);			// reconstruct the keymap from segments' stored filenames when keymap.log is lost; 0 on success, nonzero if a .del save fails

	long long add_document_core(const char *key, const char *document, const float *vector, const float *multivector = NULL, long long num_vectors = 0, const ANT_attribute_set *attributes = NULL);	// shared body; vector may be NULL; multivector/num_vectors default to none (Task 4: capture only); attributes default to none (Task 6: capture only)

	long load_vector_config(void);			// reads <dir>/vector.config; 0 = ok (absent is ok)
	long save_vector_config(void);			// atomic write; 0 on success
	long load_signature_config(void);
	long save_signature_config(void);
	void rebuild_query_signer(void);
	long load_hnsw_config(void);
	long save_hnsw_config(void);
	long long vector_candidates_hnsw(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter = NULL);	// Task 7
	long long vector_candidates_pq(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter = NULL);	// PQ ADC gatherer (replace posture); falls back to float scan for segments without a valid .pq
	long writer_vector_append(long long docid, const float *vector_or_null);
	long writer_multivector_append(long long docid, const float *multivector, long long num_vectors);
	void writer_attribute_capture(long long docid, const ANT_attribute_set *attributes);	// deep-clone into the per-docid attribute buffer (Task 6: capture only)
	void reset_writer_vectors(void);

	long long vector_candidates(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter = NULL);
	long long vector_candidates_approx(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter = NULL);	// signature-prefiltered gatherer; caller guarantees metric != L2 and approximate configured
	// exact-scan the live memory buffer into best[] (shared tail of all three vector_candidates_* gatherers)
	void scan_live_buffer_exact(const float *query, ANT_vector_candidate *best, long long *best_count, long long top_k, const unsigned char *filter_bits = NULL);
	unsigned char *evaluate_filter_for_segment(long long which, const ANT_filter *filter);	// per-disk-segment match bitset (caller frees); NULL if filter==NULL
	unsigned char *evaluate_filter_for_live(const ANT_filter *filter);						// live-buffer match bitset (caller frees); NULL if filter==NULL
	double maxsim_live(long long docid, const float *query_vecs, long long num_query_vecs);	// MaxSim over the writer's live multi-vector buffer for one docid
	long long multivector_candidates(const float *qn, long long num_query_vecs, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter);	// token-ANN candidate-gen (falls back to brute-force MaxSim when no token index) -> exact MaxSim rescore
	long long search_multivector_impl(const float *query_multivector, long long num_query_vecs, long long top_k, const ANT_filter *filter);
	// candidate-gatherer selector shared by search_vector_impl()/search_hybrid_impl(); the three gatherers differ only in this dimension
	enum vector_search_mode { VECTOR_MODE_EXACT, VECTOR_MODE_APPROX, VECTOR_MODE_HNSW, VECTOR_MODE_PQ };
	// unified cores behind the public search_vector*/search_hybrid* wrappers; mode picks the gatherer, everything else (sort/fuse/publish) is identical
	long long search_vector_impl(const float *query, long long top_k, vector_search_mode mode, const ANT_filter *filter = NULL);
	long long search_hybrid_impl(char *query_text, const float *query_vector, long long top_k, vector_search_mode mode, const ANT_filter *filter = NULL);
	char *resolve_hit_filename(long long generation, long long docid, char *buffer, long long buffer_size);
	void populate_hit_payload(hit *slot);	// fills slot->payload/payload_length from the owning segment (or live buffer) by slot->generation+docid

	void reset_results(void);			// frees results[0, results_count)'s filenames and zeroes results_count
	hit *append_result(void);			// grows results[] (doubling, initial 256) if needed, then reserves and returns the next slot

public:
	enum { VECTOR_METRIC_DOT = 0, VECTOR_METRIC_COSINE = 1, VECTOR_METRIC_L2 = 2 };
	enum { QUANTIZE_OFF = 0, QUANTIZE_REPLACE = 1, QUANTIZE_EXACT = 2 };
	enum { RERANK_QUANT_FLOAT = 0, RERANK_QUANT_INT8 = 1 };
	enum { PQ_POSTURE_REPLACE = 0, PQ_POSTURE_RERANK = 1 };

	long set_vector_config(long long dimension, long metric);		// before open(); 0 on success
	long long vector_dimension(void) { return vector_dimension_current; }

	long set_approximate_config(long long bits);		// bits<=0 => default 256; persists signature.config on first enable; 0 on success
	void set_candidate_multiplier(long long n);			// clamps to >= 1
	long approximate_configured(void) { return signature_bits_current != 0; }
	long build_signatures(void);						// idempotent backfill: .vsig for every disk segment with vectors but no valid signature sidecar; 0 on success (1 if approximate unconfigured)

	long set_hnsw_config(long long M, long long ef_construction);	// M<2 => 16, ef_construction<=0 => 200; persists hnsw.config on first enable; 0 on success
	void set_ef_search(long long ef);								// clamps >= 1; default 64
	long hnsw_configured(void) { return hnsw_M_current != 0; }
	long build_hnsw(void);											// Task 5
	long build_quantized(void);						// idempotent backfill: rewrite each float .vec disk segment as int8 .qvec (replace mode); 0 on success
	long long search_vector_hnsw(const float *query, long long top_k);						// Task 7
	long long search_vector_hnsw(const float *query, long long top_k, const ANT_filter *filter);	// filtered HNSW
	long long search_hybrid_hnsw(char *query_text, const float *query_vector, long long top_k);	// Task 8

	long load_quantization_config(void);
	long save_quantization_config(void);
	long set_quantization(long mode);		// enable quantization; persists quantization.config. Idempotent for the same mode; returns nonzero if already set to a DIFFERENT mode (immutable), or if mode invalid / vectors not configured.
	long quantization_mode(void) { return quantization_current; }

	long load_pq_config(void);
	long save_pq_config(void);
	long long default_pq_m(long long dimension);	// largest divisor of dimension in [1, min(16, dimension)]
	long set_pq_config(long long m, long posture, long rerank_quant);	// enable PQ (m==0 => default_pq_m()); persists pq.config. Idempotent for the same config; nonzero if already set to a DIFFERENT config (immutable), mode invalid, m does not divide the vector dimension, vectors not configured, or V4 int8 quantization already enabled (mutually exclusive).
	long pq_configured(void) { return pq_m_current != 0; }
	long long pq_m(void) { return pq_m_current; }
	long set_pq_policy(long eager) { pq_eager = eager ? 1 : 0; return 0; }

	long load_rerank_config(void);
	long save_rerank_config(void);
	long set_rerank_config(long long dimension, long quant_mode);	// immutable once set; 0 on success
	long rerank_configured(void) { return rerank_dimension_current != 0; }
	long long rerank_dimension(void) { return rerank_dimension_current; }

	long load_attributes_config(void);
	long save_attributes_config(void);
	long set_attributes_config(const ANT_attribute_schema &schema);	// immutable once set; 0 on success
	long attributes_configured(void) { return attribute_schema_current.count() != 0; }
	long attribute_field_count(void) { return attribute_schema_current.count(); }
	int attribute_field_type(long i) { return attribute_schema_current.type(i); }
	const ANT_attribute_schema *attribute_schema(void) { return &attribute_schema_current; }

	long set_durable(long on);				// before open(); 1 if already open; 0 on success -- enables the WAL
	void set_wal_fsync(long on);			// fsync() every WAL append when on; may be called before or after open()
	long wal_healthy(void);				// 1 when healthy OR disabled (no WAL); 0 when the last append failed

	ATIRE_segment_index();
	~ATIRE_segment_index();

	long open(const char *directory);						// 0 on success

	long long add_document(const char *key, const char *document);		// returns handle, -1 on error
	long long add_document(const char *key, const char *document, const float *vector);		// returns handle, -1 on error (also on vector rejection)
	long long add_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors);	// returns handle, -1 on error; multivector may be NULL / num_vectors 0
	long long add_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors, const ANT_attribute_set *attributes);	// returns handle, -1 on error; attributes may be NULL
	long long update_document(const char *key, const char *document);	// upsert; returns new handle
	long long update_document(const char *key, const char *document, const float *vector);	// upsert; returns new handle
	long long update_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors);	// upsert; returns new handle
	long long update_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors, const ANT_attribute_set *attributes);	// upsert; returns new handle
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
	long disk_segment_has_signatures(long long which);
	long disk_segment_has_hnsw(long long which);
	ANT_search_engine *disk_segment_engine(long long which);
	ANT_memory_index *writer_memory_index_for_test(void);		// test hook: the live writer segment's memory index (NULL before open); used to observe decompress-buffer arena reuse
	long long writer_multivector_count_for_test(long long docid);	// test hook: M for docid; 0 if none; -1 if out of range
	long writer_attribute_count_for_test(long long docid);	// test hook: # present fields captured for docid; 0 if none/out of range
	long long writer_payload_len_for_test(long long docid);	// test hook: captured payload length for docid; 0 if none

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
	long long search(char *query, long long top_k, const ANT_filter *filter);	// filtered lexical; over-pulls so a selective filter never under-returns
	long long search_vector(const float *query, long long top_k);	// exact top-k across memory buffer + disk stores
	long long search_vector(const float *query, long long top_k, const ANT_filter *filter);	// filtered exact
	long long search_vector_approx(const float *query, long long top_k);	// signature-prefiltered top-k; transparently falls back to exact for L2 / unconfigured
	long long search_hybrid(char *query_text, const float *query_vector, long long top_k);	// RRF fusion of lexical + vector top-k; either side may be absent
	long long search_hybrid_approx(char *query_text, const float *query_vector, long long top_k);	// like search_hybrid(), but the vector leg is signature-prefiltered; transparently falls back to search_hybrid() for L2 / unconfigured
	long long search_rerank(char *query_text, const float *query_vector, const float *query_multivector, long long num_query_vecs, long long first_stage_n, long long top_k);	// stage 1 (lexical/vector/hybrid, whichever inputs are given) -> MaxSim rerank of the top first_stage_n over multi-vectors, publishing top_k; candidates without multi-vectors keep stage-1 order after the reranked ones
	long long search_vector_approx(const float *query, long long top_k, const ANT_filter *filter);	// filtered signature-prefiltered top-k
	long long search_hybrid(char *query_text, const float *query_vector, long long top_k, const ANT_filter *filter);	// filtered RRF fusion of lexical + vector top-k
	long long search_hybrid_approx(char *query_text, const float *query_vector, long long top_k, const ANT_filter *filter);	// filtered hybrid, signature-prefiltered vector leg
	long long search_hybrid_hnsw(char *query_text, const float *query_vector, long long top_k, const ANT_filter *filter);	// filtered hybrid, HNSW vector leg
	long long search_rerank(char *query_text, const float *query_vector, const float *query_multivector, long long num_query_vecs, long long first_stage_n, long long top_k, const ANT_filter *filter);	// filtered stage 1 -> MaxSim rerank
	long long search_multivector(const float *query_multivector, long long num_query_vecs, long long top_k);	// first-class token-ANN candidate-gen -> exact MaxSim rescore; brute-force MaxSim fallback when no token index (Task 9 builds it)
	long long search_multivector(const float *query_multivector, long long num_query_vecs, long long top_k, const ANT_filter *filter);	// filtered; best-effort over-gather on the token-ANN path under selective filters (may under-fill), exact on the brute-force fallback
	long build_token_index(void);					// per-segment: build/rewrite .tann for segments without one; 0 = success, 1 if rerank unconfigured
	long set_token_index_policy(int eager);		// 1 = eager (build at flush), 0 = ondemand (default); returns 0
	long disk_segment_has_token_index(long long which);	// test accessor: 1 if segment `which` has a non-empty token index
	long build_pq(void);								// on-demand backfill: build .pq for PQ-configured segments lacking one; 0 = success, 1 if PQ unconfigured / no dense vectors
	long disk_segment_has_pq(long long which);			// test accessor: 1 if segment `which` has a non-empty PQ store
	hit *get_hit(long long which) { return &results[which]; }

	long long get_document_count(void);						// live (non-tombstoned) documents

	static long long make_handle(long long generation, long long docid) { return (generation << 40) | docid; }
} ;

#endif /* ATIRE_SEGMENT_INDEX_H_ */

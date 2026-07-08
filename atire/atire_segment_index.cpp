/*
	ATIRE_SEGMENT_INDEX.CPP
	-----------------------
	The implementation of ATIRE_segment_index is split by feature across
	atire_segment_index*.cpp (see
	docs/superpowers/specs/2026-07-06-segment-index-file-split-design.md):
	this file holds the lifecycle and write path (open/add/update/delete/
	flush/append_segment/rebuild_keymap/rebuild_writer_engine and the ctor/
	dtor); compaction, vectors, lexical search, and WAL/global-stats live in
	_compaction, _vector, _search, and _durability respectively.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#include "atire_segment_index.h"
#include "atire_api.h"
#include "indexer.h"
#include "../source/index_manifest.h"
#include "../source/index_keymap.h"
#include "../source/index_tombstones.h"
#include "../source/search_engine.h"
#include "../source/search_engine_result.h"
#include "../source/search_engine_accumulator.h"
#include "../source/version.h"
#include "../source/index_merge.h"
#include "../source/vector_store.h"
#include "../source/multivector_store.h"
#include "../source/wal.h"
#include "../source/signature.h"
#include "../source/signature_store.h"
#include "../source/hnsw.h"
#include "../source/token_index.h"

/*
	ATIRE_SEGMENT_INDEX::ATIRE_SEGMENT_INDEX()
	-------------------------------------------
*/
ATIRE_segment_index::ATIRE_segment_index()
{
directory = NULL;
manifest = NULL;
keymap = NULL;

segments = NULL;
segment_count = 0;
segments_allocated = 0;

writer = NULL;
writer_generation = 0;
writer_documents = 0;
writer_tombstones = NULL;
writer_engine = NULL;
writer_engine_stale = 1;

flush_after_documents = 10000;		// 0 = manual flush only; bounds NRT arena growth of the live segment

global_stats_enabled = 1;			// cross-segment global ranking statistics on by default (single-segment-equivalent scores)

merge_factor = 10;					// tier trigger default (design spec section 4)
tombstone_compact_ratio = 0.25;		// tombstone trigger default (design spec section 4)
auto_maintain = 0;					// off by default: Phase 1 behaviour unchanged unless requested

results = NULL;
results_count = 0;
results_allocated = 0;

vector_dimension_current = 0;		// vectors disabled until set_vector_config()/an on-disk vector.config says otherwise
vector_metric = 0;
pending_vector_dimension = 0;
pending_vector_metric = 0;
vector_config_pending = 0;

signature_bits_current = 0;
signature_seed = 0;
candidate_multiplier = 4;
query_signer = NULL;

hnsw_M_current = 0;
hnsw_ef_construction_current = 0;
hnsw_ef_search = 64;

quantization_current = 0;

rerank_dimension_current = 0;
rerank_quant_current = 0;

token_index_M = 16;
token_index_ef_construction = 200;
token_top_p = 32;
token_index_eager = 0;

writer_vector_data = NULL;
writer_vector_presence = NULL;
writer_vector_capacity = 0;
writer_vectors_present = 0;

writer_multivector_data = NULL;
writer_multivector_capacity = 0;
writer_multivector_total = 0;
writer_multivector_counts = NULL;
writer_multivector_counts_capacity = 0;

writer_attribute_sets = NULL;
writer_attribute_sets_capacity = 0;

durable = 0;
wal_fsync_pending = 0;
wal = NULL;
wal_replaying = 0;
wal_suppress_add = 0;
wal_truncate_pending = 0;
}

/*
	ATIRE_SEGMENT_INDEX::~ATIRE_SEGMENT_INDEX()
	--------------------------------------------
*/
ATIRE_segment_index::~ATIRE_segment_index()
{
long long which;

delete writer_engine;			// non-owning wrapper; leaves the writer's index alone
delete writer;
delete writer_tombstones;
reset_writer_vectors();
delete query_signer;
delete wal;

for (which = 0; which < segment_count; which++)
	{
	delete segments[which].engine;
	delete segments[which].tombstones;
	delete segments[which].vectors;
	delete segments[which].exact_vectors;
	delete segments[which].signatures;
	delete segments[which].hnsw_graph;
	delete segments[which].multivectors;
	delete segments[which].token_index;
	delete segments[which].attributes;
	delete segments[which].payload;
	}
delete [] segments;

delete keymap;
delete manifest;
delete [] directory;

for (which = 0; which < results_count; which++)
	delete [] results[which].filename;
delete [] results;
}

/*
	ATIRE_SEGMENT_INDEX::SEGMENT_FILENAME()
	----------------------------------------
*/
void ATIRE_segment_index::segment_filename(char *buffer, long long buffer_size, long long generation, const char *extension)
{
snprintf(buffer, (size_t)buffer_size, "%s/seg_%06lld.%s", directory, generation, extension);
}

/*
	ATIRE_SEGMENT_INDEX::DELETE_SEGMENT_FILES()
	-------------------------------------------
	Best-effort: files that survive (permissions, races) are unmanifested
	orphans and are swept at the next open().
*/
void ATIRE_segment_index::delete_segment_files(long long generation)
{
char filename[4096];

segment_filename(filename, sizeof(filename), generation, "aspt");
remove(filename);
segment_filename(filename, sizeof(filename), generation, "del");
remove(filename);
segment_filename(filename, sizeof(filename), generation, "vec");
remove(filename);
segment_filename(filename, sizeof(filename), generation, "qvec");
remove(filename);
segment_filename(filename, sizeof(filename), generation, "vsig");
remove(filename);
segment_filename(filename, sizeof(filename), generation, "hnsw");
remove(filename);
segment_filename(filename, sizeof(filename), generation, "mvec");
remove(filename);
segment_filename(filename, sizeof(filename), generation, "attr");
remove(filename);
segment_filename(filename, sizeof(filename), generation, "pay");
remove(filename);
}

/*
	ATIRE_SEGMENT_INDEX::START_NEW_WRITER()
	------------------------------------------
	Take the next generation number, persist the manifest (before creating any
	file named with that generation), then open a fresh in-memory writer.
	Returns 0 on success, 1 if the manifest cannot be saved (in which case no
	writer is started: the crash-safety guarantee -- generation persisted
	before any file named with it exists -- would otherwise be lost).
*/
long ATIRE_segment_index::start_new_writer(void)
{
char index_filename[1024], doclist_filename[1024], options[2200];

writer_generation = manifest->take_generation();

/*
	Manifest must be saved before any file named with the taken generation is
	created (see index_manifest.h): the writer's init() below creates the
	doclist file.
*/
if (manifest->save() != 0)
	return 1;

segment_filename(index_filename, sizeof(index_filename), writer_generation, "aspt");
segment_filename(doclist_filename, sizeof(doclist_filename), writer_generation, "doclist");

/*
	ATIRE_indexer::init(char *options) tokenises on '+', not spaces (it
	prepends "index+" and strtok()s the result on "+" alone -- see
	nodejs/index.js's createOptionsString(), which builds its options strings
	the same way): each flag and each flag's value must be its own '+'
	separated token, or ANT_indexer_param_block::parse() never sees them and
	silently falls back to its compiled-in defaults ("index.aspt" /
	"doclist.aspt", relative to the current working directory).
*/
snprintf(options, sizeof(options), "-nologo+-findex+%s+-fdoclist+%s", index_filename, doclist_filename);

writer = new ATIRE_indexer();
writer->init(options);

writer_documents = 0;
delete writer_tombstones;
writer_tombstones = new ANT_index_tombstones(1024);
reset_writer_vectors();

delete writer_engine;
writer_engine = NULL;
writer_engine_stale = 1;

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::REBUILD_KEYMAP()
	-----------------------------------------
	The keymap is, by design, a CACHE over information the segments
	already carry themselves -- each segment stores its documents' filenames
	internally (this build always serialises FILENAME_INDEX-style, ANT_V5;
	see append_segment()'s banner) -- so if keymap.log is lost, the map can
	be reconstructed by scanning every open disk segment's stored filenames
	rather than losing the ability to look documents up by key.

	segments[] is in manifest order, which is the order generations were
	added (= append/flush order), i.e. OLDEST first.  Walking it in that
	order (and each segment's docids ascending, which within one segment is
	likewise oldest first) and simply calling keymap->add() for every live
	document means a key that appears twice (an update whose tombstone was
	itself recorded only in the lost keymap.log) ends up pointing at the
	copy processed LAST -- the newest one -- exactly the "newer copy wins"
	semantics an update provides.  But that leaves the OLDER copy silently
	still reachable through the segment's own doclist / accumulator scan
	(search_one_segment() does not consult the keymap, only tombstones), so
	it must also be explicitly tombstoned: it was already dead before the
	log was lost (its update is the very reason it is being superseded now),
	and a live-looking stale duplicate would otherwise resurface in search
	results and could later be resurrected by update_document()/
	delete_document() acting on it via a keymap collision.

	Tombstones raised here are set in memory only during the scan (a
	duplicate-heavy rebuild would otherwise rewrite the same .del file once
	per duplicate); each dirtied segment's .del is then saved exactly ONCE
	after the scan.  Returns 0 on success, nonzero if any .del save fails:
	in that case the keymap points at the new copies but the old duplicates
	would come back from the dead on the next open (their tombstones were
	never persisted), so the caller must treat the whole open() as failed
	rather than proceed with a keymap it cannot trust.
*/
long ATIRE_segment_index::rebuild_keymap(void)
{
long long which, docid, doc_count, victim;
long long old_generation, old_docid;
char filename_buffer[4096], del_name[4096];
char *filename;
long *dirty, failed;

dirty = new long[segment_count];
memset(dirty, 0, (size_t)(segment_count * sizeof(*dirty)));

for (which = 0; which < segment_count; which++)
	{
	doc_count = segments[which].engine->get_document_count();
	for (docid = 0; docid < doc_count; docid++)
		{
		if (segments[which].tombstones->is_deleted(docid))
			continue;		// already dead: never enters the map

		/*
			ATIRE_API::get_document_filename() writes into a caller-supplied
			buffer with no bounds checking (see atire_segment_index.h's
			use_filename_index comment / atire_api.cpp) -- 4096 bytes matches
			the buffer used everywhere else in this file for the same call.
		*/
		filename = segments[which].engine->get_document_filename(filename_buffer, docid);
		if (filename == NULL || filename[0] == '\0')
			continue;		// nothing to key on: skip

		/*
			If this key is already in the (partially rebuilt) map, the
			earlier copy is the OLDER one -- it was superseded by an update
			whose tombstone lived only in the log we just lost.  Tombstone it
			in memory (its .del is batch-saved below) so it stops showing up
			in search() and cannot be resurrected via the keymap.  The old
			(generation, docid) can only name a disk segment: every entry in
			the map at this point was added by this very loop, and the writer
			does not exist yet (open() rebuilds before start_new_writer()).
		*/
		if (keymap->find(filename, &old_generation, &old_docid))
			for (victim = 0; victim < segment_count; victim++)
				if (segments[victim].generation == old_generation)
					{
					segments[victim].tombstones->set_deleted(old_docid);
					dirty[victim] = 1;
					break;
					}

		keymap->add(filename, segments[which].generation, docid);
		}
	}

/*
	Persist each dirtied segment's tombstones exactly once.  Keep going past
	a failure (later segments' saves are independent and every persisted
	tombstone is one fewer resurrected duplicate) but report it.
*/
failed = 0;
for (which = 0; which < segment_count; which++)
	if (dirty[which])
		{
		segment_filename(del_name, sizeof(del_name), segments[which].generation, "del");
		if (segments[which].tombstones->save(del_name) != 0)
			failed = 1;
		}

delete [] dirty;
return failed;
}

/*
	ATIRE_SEGMENT_INDEX::OPEN()
	------------------------------
*/
long ATIRE_segment_index::open(const char *directory)
{
long long which;
char marker_name[4096];

this->directory = new char[strlen(directory) + 1];
strcpy(this->directory, directory);

if (load_vector_config() != 0)
	return 1;
load_signature_config();
rebuild_query_signer();
load_hnsw_config();
load_quantization_config();
load_rerank_config();
load_attributes_config();
if (vector_config_pending)
	{
	if (vector_dimension_current != 0)
		{
		/*
			An existing config must agree with the caller's -- silent
			dimension/metric mixups corrupt results, so fail loudly.
		*/
		if (vector_dimension_current != pending_vector_dimension || vector_metric != pending_vector_metric)
			return 1;
		}
	else
		{
		vector_dimension_current = pending_vector_dimension;
		vector_metric = pending_vector_metric;
		if (save_vector_config() != 0)
			return 1;
		}
	}

manifest = ANT_index_manifest::load(this->directory);

/*
	Must be captured BEFORE ANT_index_keymap::load(), which -- when the log
	file does not exist -- fopen("ab")s it into existence as a side effect
	of preparing to append (see index_keymap.cpp's load()).  After that call
	the file always exists, so this is the only point at which "was there
	really a keymap.log going into this open()" can still be asked.

	The "compacting" marker is checked here for the same reason: its mere
	presence means a compact() (see compact()'s banner) died somewhere
	between writing the merge output and finishing the swap, so the log
	about to be loaded may lie -- it can hold remap records for a segment
	that never became live, or be missing records for one that did.  The
	decision to distrust it is recorded now, before load() gives it any
	chance to be trusted.
*/
long had_keymap_log = ANT_index_keymap::log_exists(this->directory);
snprintf(marker_name, sizeof(marker_name), "%s/compacting", this->directory);
long compaction_died = access(marker_name, F_OK) == 0;
keymap = ANT_index_keymap::load(this->directory);

/*
	A compaction died mid-swap: the keymap.log just loaded cannot be
	trusted (see above).  Discard it entirely -- delete the in-memory
	keymap, remove the log file, and reload fresh/empty (load() recreates
	the file for appending) -- rather than reconcile it piecemeal.  The
	segments below are rebuilt from scratch into this empty keymap.
*/
if (compaction_died)
	{
	char keymap_log_name[4096];

	snprintf(keymap_log_name, sizeof(keymap_log_name), "%s/keymap.log", this->directory);
	delete keymap;
	remove(keymap_log_name);
	keymap = ANT_index_keymap::load(this->directory);		// fresh, empty; recreates the log file
	}

/*
	Reopen every disk segment the manifest still lists as live.  The manifest
	only ever records FLUSHED segments (start_new_writer() persists the
	generation *before* creating the writer's files, but add_segment() only
	happens after a successful flush), so a stray, unflushed writer file left
	behind by a session that exited without flushing is simply not in this
	list and is never opened here.
*/
for (which = 0; which < manifest->segment_count(); which++)
	if (append_segment(manifest->get_segment(which)) != 0)
		return 1;

/*
	ORPHAN SWEEP
	------------
	Remove seg_* files whose generation the manifest does not reference.
	These arise from: (a) a session that died with an unflushed writer (its
	.aspt/.doclist were created by start_new_writer()'s init() but the
	generation was never added to the manifest, since add_segment() only
	happens after a successful flush()); (b) a crash/I-O failure between
	flush() writing the segment's files and flush() saving the manifest; or
	(c) a compact() that died after writing its merge output but before that
	output was manifested (its own crash is what left the "compacting"
	marker checked above).

	In cases (b)/(c) the segment is COMPLETE on disk -- but the durability
	contract is that a segment is durable only once the manifest save that
	references it succeeds, so a segment the manifest never came to
	reference is, by definition, not durable: this sweep deletes it, and any
	documents that were only in it are lost BY DESIGN.  This must run before
	start_new_writer() below claims the next generation (so it can never
	touch the new writer's own not-yet-manifested files) and before the
	keymap-consistency decision below (so a marker-triggered rebuild only
	ever scans manifested, durable segments).
*/
DIR *directory_handle = opendir(this->directory);
if (directory_handle != NULL)
	{
	struct dirent *entry;
	while ((entry = readdir(directory_handle)) != NULL)
		if (strncmp(entry->d_name, "seg_", 4) == 0)
			{
			long long file_generation = atoll(entry->d_name + 4);
			if (file_generation > 0 && !manifest->contains(file_generation))
				{
				char victim[4096];
				snprintf(victim, sizeof(victim), "%s/%s", this->directory, entry->d_name);
				remove(victim);
				}
			}
	closedir(directory_handle);
	}

/*
	Keymap consistency.  Two mutually exclusive cases:

	1. compaction_died, or there was no keymap.log going into this open():
	   the keymap loaded above is empty (either freshly discarded, above, or
	   never existed) and every already-open segment is ground truth for it
	   -- rebuild by scanning their stored filenames.  Runs after the
	   append_segment loop (rebuild_keymap() needs segments[] populated) and
	   after the orphan sweep (so it only ever sees manifested segments --
	   in particular, a dead, unmanifested compaction OUTPUT left behind by
	   the very crash that left the marker has already been swept away, so
	   the rebuild cannot resurrect it).  The rebuilt entries are appended
	   through keymap->add(), whose log handle load() already opened for
	   append, so the rebuild persists for the next open().  A nonzero
	   return means some segment's re-raised tombstones could not be
	   persisted -- the keymap would then disagree with what the next
	   open() sees on disk (dead duplicates resurrected), so refuse to open
	   rather than serve an index we know is inconsistent.  The marker (if
	   any) is removed only once the rebuild has succeeded; on failure it is
	   left in place so the next open() retries the same recovery.  An empty
	   index (segment_count == 0) has nothing to rebuild, so the marker is
	   simply removed.

	2. Normal case: reconcile the keymap against the manifest BEFORE the new
	   writer's generation exists -- keymap entries can reference
	   generations that were added to but never flushed (a session that
	   exited without flush()ing its memory segment).  On reopen those
	   entries are lies -- the segment they point at does not exist and
	   never will -- so any live keymap entry whose generation is not one
	   of the manifest's segments must be dropped now, or delete_document()
	   would fail confusingly and update_document() would tombstone into
	   nothing.  retain_generations() must NOT run on the rebuild path
	   above: rebuild_keymap() only ever produces manifest generations by
	   construction, so there would be nothing for it to do, and the whole
	   point of discarding the log is to not trust anything about it that
	   load() handed back.
*/
if (compaction_died || (!had_keymap_log && segment_count > 0))
	{
	if (segment_count > 0 && rebuild_keymap() != 0)
		return 1;
	remove(marker_name);
	}
else
	{
	long long *manifest_generations = new long long[manifest->segment_count()];
	for (which = 0; which < manifest->segment_count(); which++)
		manifest_generations[which] = manifest->get_segment(which);
	keymap->retain_generations(manifest_generations, manifest->segment_count());
	delete [] manifest_generations;
	}

if (keymap->log_dead_ratio() > 0.5)
	keymap->compact_log();				// best-effort: ignore any failure

if (start_new_writer() != 0)
	return 1;

/*
	Every disk segment is now open; push collection-wide global ranking
	statistics into them so a reopened multi-segment index scores exactly
	like the equivalent single-segment index (no-op when disabled).
*/
refresh_global_statistics();

/*
	Durable mode: open the WAL (relaxed/non-durable indices never touch
	wal.log at all -- an existing log from a previous durable session is
	left exactly as it is, per the design's "relaxed mode ignores an
	existing WAL" rule: no replay, no deletion).  Placed at the very end
	of open() because vector_dimension_current is only known for certain
	once load_vector_config() (above) has run, and every mutation this
	replay is about to make must land in a fully-opened index (segments,
	keymap and writer all live).
*/
if (durable)
	{
	wal = ANT_write_ahead_log::open(this->directory, vector_dimension_current);
	if (wal == NULL)
		return 1;
	if (wal_fsync_pending)
		wal->set_fsync(1);

	/*
		Replay through the PUBLIC methods so every ordinary invariant
		(keymap update, vector buffer, auto-flush) is maintained exactly as
		it would have been for the original caller.  wal_replaying
		suppresses the append hooks below (replaying must not re-log what
		is already durably on record in the very file being read) and also
		tells flush() to defer any truncate() to below (see
		wal_truncate_pending's declaration in the header) -- auto-flush
		may legitimately fire partway through a long replay, and
		truncating the log while replay_next() still holds a file position
		into it would end the iteration early and silently drop the
		untouched tail.
	*/
	wal_replaying = 1;
	wal_truncate_pending = 0;
	ANT_write_ahead_log::record record;
	while (wal->replay_next(&record))
		{
		/*
			Individual replay failures are ignored by design: they mirror
			whatever the original caller already saw (e.g. a zero-term
			document that add_document() rejected the first time round
			would be rejected identically here), so the replayed state
			ends up matching the pre-crash state either way.
		*/
		if (record.op == 'A')
			add_document(record.key, record.document, record.vector);
		else if (record.op == 'U')
			update_document(record.key, record.document, record.vector);
		else if (record.op == 'D')
			delete_document(record.key);
		}
	wal_replaying = 0;

	/*
		Apply any truncate() that an auto-flush during replay deferred: the
		replayed-and-flushed content is durable now (it is sitting in a
		manifested disk segment), so the log recording it is safe to
		empty.  Best-effort, same as flush()'s own truncate() -- failure
		leaves records that will harmlessly replay again next time against
		already-durable state.
	*/
	if (wal_truncate_pending)
		{
		wal->truncate();
		wal_truncate_pending = 0;
		}
	}

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::ADD_DOCUMENT_CORE()
	-----------------------------------------
	The indexer's parser normalises tokens in place, so both the key and the
	document must be writable heap buffers (string literals fault); we copy
	both, index, then free the copies (the indexer keeps its own copies of
	whatever it needs internally, e.g. the doclist).

	Shared body for both add_document() overloads: `vector` is NULL for the
	lexical-only two-arg path.  The vector-buffer append happens BEFORE the
	auto-flush check below -- deliberately: the auto-flush may hand the
	writer a brand new generation/segment (start_new_writer()), and by that
	point this document's docid must already be recorded in ITS segment's
	vector buffer, or the flush would serialise the segment without it.
*/
long long ATIRE_segment_index::add_document_core(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors, const ANT_attribute_set *attributes)
{
char *key_copy, *doc_copy;
long long before, docid;

if (key == NULL || document == NULL)
	return -1;

/*
	No live writer: a previous flush() failed after tearing the writer down
	(see flush()).  The index is read-only over its open segments until a
	successful flush()/reopen.
*/
if (writer == NULL)
	return -1;

key_copy = new char[strlen(key) + 1];
strcpy(key_copy, key);

doc_copy = new char[strlen(document) + 1];
strcpy(doc_copy, document);

before = writer->get_docno();
writer->index_document(key_copy, doc_copy);
docid = writer->get_docno();

delete [] key_copy;
delete [] doc_copy;

/*
	A document that parses to zero terms makes the indexer roll docno back
	(docno--; WITH_EMPTY_DOCUMENT is not defined in this build) and index
	nothing: without this guard we would return a handle aliasing the
	PREVIOUS document, add a colliding keymap entry, and inflate
	writer_documents.
*/
if (docid == before)		// zero terms -> indexer rolled docno back; nothing was indexed
	return -1;

/*
	ATIRE_indexer::get_docno() is 1-based (docno is pre-incremented before
	the first document is indexed), but the search engine's accumulator
	array -- and so the docid we must hand back for get_document_filename()
	and get_hit()->docid to line up with -- is 0-based (docid = pointer
	arithmetic into a zero-based array; doc_list[] is likewise appended
	0-based).  Subtract 1 here so make_handle()'s docid matches what
	search_one_segment() will report for the same document.
*/
docid -= 1;

/*
	Vector buffer append: must happen here, before keymap->add() and well
	before the auto-flush check below, so this docid's vector (or its
	explicit absence) is already in the buffer that flush() will serialise
	for THIS segment.  NULL is a valid, meaningful append -- it records a
	lexical-only row so the buffer stays parallel to the writer's docids.
*/
if (vector_dimension_current != 0)
	writer_vector_append(docid, vector);
if (rerank_configured())
	writer_multivector_append(docid, multivector, num_vectors);
if (attributes_configured() && attributes != NULL)
	writer_attribute_capture(docid, attributes);

writer_documents++;
writer_engine_stale = 1;

keymap->add(key, writer_generation, docid);

/*
	WAL append: must happen here -- after the engine has fully committed
	this document (indexer, vector buffer and keymap all updated) but
	BEFORE the auto-flush check below.  Getting this backwards is the
	sharpest trap in this integration: if the append happened after an
	auto-flush, flush() would truncate() the log for a batch that does
	NOT yet include this document, and only then would the append land --
	so a crash after that point would replay this document a second time
	against state where it is already durable (via the disk segment
	flush() just wrote), corrupting document counts.  With the append
	here, a flush() immediately below correctly truncates a log that
	already includes this record.

	wal_replaying: suppressed during open()'s replay (the record being
	replayed is, by definition, already in the very file being read; the
	log must not be extended while it is being consumed).  wal_suppress_add:
	set by the public update_document() around its inner add_document()
	call so an upsert logs exactly one 'U', not an 'A' followed by a 'U'.
	Log the ORIGINAL key/document pointers the caller passed in -- not
	key_copy/doc_copy above, which the indexer may have mutated in place
	and which are freed by now regardless.

	COSINE note: for this 'A' hook, `vector` here is whatever add_document()
	passed down -- already normalized for COSINE-metric indexes (see
	add_document()'s banner comment) -- so the logged vector is
	POST-normalization; replay renormalizes it again on top, which is
	idempotent only to ULP scale (harmless).  The 'U' hook below, by
	contrast, logs the RAW caller-supplied vector, so its replay renormalizes
	from the same original input and reproduces the first normalization
	bit-exactly.
*/
if (wal != NULL && !wal_replaying && !wal_suppress_add)
	wal->append('A', key, document, vector);

/*
	Handle must be computed BEFORE any auto-flush below: flush() hands the
	writer a new generation (start_new_writer()), so writer_generation would
	no longer match the document we just indexed once flush() returns.
*/
long long handle = make_handle(writer_generation, docid);

/*
	Auto-flush: bound the live segment's size (and so the NRT rebuild cost,
	see set_flush_threshold()'s doc comment).  Best-effort -- a failed
	auto-flush degrades per flush()'s contract: depending on the failure
	point the just-added document is either still in the live writer
	(finish()/tombstone-save fail before the writer teardown), or
	flushed-but-not-durable (append_segment/manifest-save/start_new_writer
	fail after teardown: the batch may remain searchable in-session, but
	its segment was never manifested, so the orphan sweep deletes it on the
	next open() and the handle we return below then names a lost document).
	Callers needing certainty must call flush() themselves and check its
	return.
*/
if (flush_after_documents > 0 && writer_documents >= flush_after_documents)
	flush();

return handle;
}

/*
	ATIRE_SEGMENT_INDEX::ADD_DOCUMENT()
	--------------------------------------
	Lexical-only path: no vector row.
*/
long long ATIRE_segment_index::add_document(const char *key, const char *document)
{
return add_document_core(key, document, NULL);
}

/*
	ATIRE_SEGMENT_INDEX::ADD_DOCUMENT()  (vector overload)
	------------------------------------------------------
	Cosine normalization and the disabled-index / zero-vector rejections
	happen here, BEFORE add_document_core() is called, so a rejected vector
	never touches the indexer, the keymap, or the vector buffer.
*/
long long ATIRE_segment_index::add_document(const char *key, const char *document, const float *vector)
{
float *normalized = NULL;
long long handle;

if (vector != NULL && vector_dimension_current == 0)
	return -1;			// vectors on a non-vector index
if (vector != NULL && vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, vector, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{
		delete [] normalized;
		return -1;		// zero vector is meaningless under cosine
		}
	vector = normalized;
	}

handle = add_document_core(key, document, vector);
delete [] normalized;
return handle;
}

/*
	ATIRE_SEGMENT_INDEX::ADD_DOCUMENT()  (vector + multi-vector overload)
	------------------------------------------------------------------------
	Task 4: capture-only -- the multi-vector rows are buffered (see
	writer_multivector_append()) but not yet flushed to disk or searched
	(that comes in later tasks).  The doc-level `vector` is handled exactly
	as in the 3-arg overload above (cosine normalization / disabled-index
	and zero-vector rejection all happen here, before add_document_core()
	is called).  multivector/num_vectors are NOT rejected here -- each row
	is normalized individually inside writer_multivector_append() (called
	from add_document_core()), and a zero row there is accepted, not fatal,
	unlike the doc-level vector's cosine path.
*/
long long ATIRE_segment_index::add_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors)
{
float *normalized = NULL;
long long handle;

if (vector != NULL && vector_dimension_current == 0)
	return -1;			// vectors on a non-vector index
if (vector != NULL && vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, vector, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{
		delete [] normalized;
		return -1;		// zero vector is meaningless under cosine
		}
	vector = normalized;
	}

handle = add_document_core(key, document, vector, multivector, num_vectors);
delete [] normalized;
return handle;
}

/*
	ATIRE_SEGMENT_INDEX::ADD_DOCUMENT()  (vector + multi-vector + attributes overload)
	--------------------------------------------------------------------------------------
	Task 6: capture-only -- the caller-supplied attribute set + payload is
	deep-cloned into the writer's per-docid attribute buffer (see
	writer_attribute_capture()); flush to the .attr / payload sidecars is a
	later task.  The doc-level vector preamble (cosine normalization /
	disabled-index and zero-vector rejection) is identical to the 5-arg
	overload above; only the extra `attributes` argument is threaded through
	to add_document_core().
*/
long long ATIRE_segment_index::add_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors, const ANT_attribute_set *attributes)
{
float *normalized = NULL;
long long handle;

if (vector != NULL && vector_dimension_current == 0)
	return -1;			// vectors on a non-vector index
if (vector != NULL && vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, vector, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{
		delete [] normalized;
		return -1;		// zero vector is meaningless under cosine
		}
	vector = normalized;
	}

handle = add_document_core(key, document, vector, multivector, num_vectors, attributes);
delete [] normalized;
return handle;
}

/*
	ATIRE_SEGMENT_INDEX::TOMBSTONE()
	--------------------------------
	Mark (generation, docid) deleted.  For a disk segment the .del file is
	persisted immediately (write-temp + rename, so it is crash-atomic).
*/
long ATIRE_segment_index::tombstone(long long generation, long long docid)
{
char del_name[4096];
long long which;

if (writer != NULL && generation == writer_generation)
	{
	writer_tombstones->set_deleted(docid);
	return 0;
	}
for (which = 0; which < segment_count; which++)
	if (segments[which].generation == generation)
		{
		segments[which].tombstones->set_deleted(docid);
		segment_filename(del_name, sizeof(del_name), generation, "del");
		return segments[which].tombstones->save(del_name);
		}
return 1;			// unknown segment: keymap and manifest disagree (should be impossible after reconciliation)
}

/*
	ATIRE_SEGMENT_INDEX::UPDATE_DOCUMENT()
	-----------------------------------------
	Upsert.  Add the new content FIRST, then tombstone the old
	(generation, docid) -- in that order, so a crash between the two leaves
	a transient duplicate (harmless, filtered at compaction) rather than a
	lost document.  add_document() already repoints the keymap at the new
	copy, so by the time we tombstone the old one it is no longer reachable
	through the keymap regardless.

	If add_document() rejects the new content (returns -1, e.g. a zero-term
	document), the keymap still points at the OLD version and nothing is
	tombstoned: the update is a no-op, which is the correct failure
	semantics (preserve the old copy rather than lose the document).
*/
long long ATIRE_segment_index::update_document(const char *key, const char *document, const float *vector)
{
long long old_generation, old_docid;
long had_old = keymap->find(key, &old_generation, &old_docid);

/*
	wal_suppress_add: the inner add_document() call below goes through
	add_document_core(), whose WAL append hook would otherwise log an 'A'
	for what is really an update.  Suppress it here and log the single
	'U' record ourselves, after add_document() has succeeded, so an
	upsert produces exactly one WAL record.
*/
wal_suppress_add = 1;
long long handle = add_document(key, document, vector);		// also repoints the keymap at the new copy
wal_suppress_add = 0;
if (handle < 0)
	return -1;

if (wal != NULL && !wal_replaying)
	wal->append('U', key, document, vector);

if (had_old)
	tombstone(old_generation, old_docid);
return handle;
}

/*
	ATIRE_SEGMENT_INDEX::UPDATE_DOCUMENT()  (vector + multi-vector overload)
	----------------------------------------------------------------------------
	Upsert, as above, but also threads multivector/num_vectors through to the
	5-arg add_document() so the new copy's multi-vector rows (Task 4:
	capture-only) are buffered for its docid.  The WAL 'U' record logs only
	the doc-level vector, same as the 3-arg overload above -- multi-vector WAL
	durability is out of scope for this task.
*/
long long ATIRE_segment_index::update_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors)
{
long long old_generation, old_docid;
long had_old = keymap->find(key, &old_generation, &old_docid);

wal_suppress_add = 1;
long long handle = add_document(key, document, vector, multivector, num_vectors);		// also repoints the keymap at the new copy
wal_suppress_add = 0;
if (handle < 0)
	return -1;

if (wal != NULL && !wal_replaying)
	wal->append('U', key, document, vector);

if (had_old)
	tombstone(old_generation, old_docid);
return handle;
}

/*
	ATIRE_SEGMENT_INDEX::UPDATE_DOCUMENT()  (vector + multi-vector + attributes overload)
	-----------------------------------------------------------------------------------------
	Upsert, as above, but threads the attribute set/payload through to the
	6-arg add_document() so the new copy's captured attributes (Task 6:
	capture-only) are buffered for its docid.  WAL 'U' record logs only the
	doc-level vector, same as the other overloads -- attribute WAL durability
	is out of scope for this task.
*/
long long ATIRE_segment_index::update_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors, const ANT_attribute_set *attributes)
{
long long old_generation, old_docid;
long had_old = keymap->find(key, &old_generation, &old_docid);

wal_suppress_add = 1;
long long handle = add_document(key, document, vector, multivector, num_vectors, attributes);		// also repoints the keymap at the new copy
wal_suppress_add = 0;
if (handle < 0)
	return -1;

if (wal != NULL && !wal_replaying)
	wal->append('U', key, document, vector);

if (had_old)
	tombstone(old_generation, old_docid);
return handle;
}

/*
	ATIRE_SEGMENT_INDEX::UPDATE_DOCUMENT()
	-----------------------------------------
	Lexical-only path: one code path with the vector overload above.
*/
long long ATIRE_segment_index::update_document(const char *key, const char *document)
{
return update_document(key, document, NULL);
}

/*
	ATIRE_SEGMENT_INDEX::WRITER_ATTRIBUTE_CAPTURE()
	--------------------------------------------------
	Deep-clone the caller's attribute set into the writer's per-docid
	attribute buffer (parallel to the writer's docids), growing the pointer
	array geometrically to fit `docid` -- exactly like the counts array in
	writer_multivector_append().  Task 6: capture only; the drain to the
	.attr / payload sidecars is Task 7.
*/
void ATIRE_segment_index::writer_attribute_capture(long long docid, const ANT_attribute_set *attributes)
{
if (writer_attribute_sets == NULL)
	{
	writer_attribute_sets_capacity = 1024;
	writer_attribute_sets = new ANT_attribute_set *[writer_attribute_sets_capacity];
	for (long long i = 0; i < writer_attribute_sets_capacity; i++)
		writer_attribute_sets[i] = NULL;
	}
if (docid >= writer_attribute_sets_capacity)
	{
	long long new_capacity = writer_attribute_sets_capacity * 2;
	while (docid >= new_capacity)
		new_capacity *= 2;
	ANT_attribute_set **grown = new ANT_attribute_set *[new_capacity];
	memcpy(grown, writer_attribute_sets, (size_t)(writer_attribute_sets_capacity * sizeof(ANT_attribute_set *)));
	for (long long i = writer_attribute_sets_capacity; i < new_capacity; i++)
		grown[i] = NULL;
	delete [] writer_attribute_sets;
	writer_attribute_sets = grown;
	writer_attribute_sets_capacity = new_capacity;
	}

delete writer_attribute_sets[docid];		// a re-add of the same docid -- shouldn't happen, but be safe
writer_attribute_sets[docid] = attributes->clone();
}

/*
	ATIRE_SEGMENT_INDEX::WRITER_ATTRIBUTE_COUNT_FOR_TEST()
	----------------------------------------------------------
	Test hook (Task 6): the number of present fields captured for `docid` in
	the live writer segment; 0 if none / out of range.
*/
long ATIRE_segment_index::writer_attribute_count_for_test(long long docid)
{
docid &= (1LL << 40) - 1;		// accept either a raw docid or a make_handle() handle (docid is its low 40 bits)
if (writer_attribute_sets != NULL && docid >= 0 && docid < writer_attribute_sets_capacity && writer_attribute_sets[docid] != NULL)
	return writer_attribute_sets[docid]->present_field_count();
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::WRITER_PAYLOAD_LEN_FOR_TEST()
	------------------------------------------------------
	Test hook (Task 6): the captured payload length for `docid`; 0 if no set /
	no payload / out of range.
*/
long long ATIRE_segment_index::writer_payload_len_for_test(long long docid)
{
docid &= (1LL << 40) - 1;		// accept either a raw docid or a make_handle() handle (docid is its low 40 bits)
if (writer_attribute_sets != NULL && docid >= 0 && docid < writer_attribute_sets_capacity && writer_attribute_sets[docid] != NULL)
	{
	long long len = 0;
	writer_attribute_sets[docid]->payload_bytes(&len);
	return len;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::DELETE_DOCUMENT()
	-----------------------------------------
	Look the key up in the keymap, mark it deleted in the owning
	segment's tombstones (writer_tombstones or segments[which].tombstones),
	and remove it from the keymap.
*/
long ATIRE_segment_index::delete_document(const char *key)
{
long long generation, docid;

if (!keymap->find(key, &generation, &docid))
	return 1;		// unknown key

if (tombstone(generation, docid) != 0)
	return 1;

keymap->remove(key);

if (wal != NULL && !wal_replaying)
	wal->append('D', key, NULL, NULL);

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::FLUSH()
	-------------------------------
	Serialise the writer's memory index + tombstones to disk as an immutable
	segment, register it in the manifest, append it to segments[], then
	start_new_writer() for the next generation.

	Ordering is deliberate for crash safety: the segment's files (.aspt, and
	.del if there are any tombstones) are complete on disk BEFORE the manifest
	is updated to reference that generation, and the manifest is saved BEFORE
	start_new_writer() creates the next generation's files.  A crash at any
	point leaves either the old state (nothing changed) or the new state
	(fully written) -- never a manifest pointing at a half-written segment.

	On failure the index degrades to read-only over the already-open segments:
	once the writer has been torn down (writer == NULL, writer_documents == 0)
	add_document() returns -1 until a successful flush()/reopen, and search()
	keeps serving the disk segments (plus, before the writer teardown point,
	nothing has changed so the memory segment is still searchable).
*/
long ATIRE_segment_index::flush(void)
{
long long flushed_generation;
char del_filename[1024];

if (writer_documents == 0)
	return 0;

/*
	The writer_engine is a non-owning NRT wrapper around the writer's memory
	index (rebuild_writer_engine(), take_ownership = 0).  It must be torn
	down before writer->finish() touches the memory index (finish() calls
	ANT_memory_index::serialise(), which -- via the wrapper's quantization_bits
	save/restore -- expects to be the sole owner at that point).
*/
delete writer_engine;
writer_engine = NULL;
writer_engine_stale = 1;

if (writer->finish() == 0)		// docno <= 0 (shouldn't happen: writer_documents > 0 above) or serialise() failed
	return 1;

/*
	Persist the memory segment's vectors alongside its postings (only when
	vectors are enabled and at least one document in this segment actually
	has one).  Must run here, while writer_vector_data/writer_vector_presence
	are still alive -- the teardown below zeroes writer_documents, and
	start_new_writer() (further down) calls reset_writer_vectors(), which
	frees the buffer outright.  Locals are captured up front so this block
	does not depend on writer_generation/writer_documents surviving past this
	point.  Same crash contract as every other segment file: written fully
	before the manifest references the generation; failure here degrades per
	flush()'s existing pre-manifest contract.
*/
if (vector_dimension_current != 0 && writer_vectors_present > 0)
	{
	char vec_filename[1024];
	long long flushed_document_count = writer_documents;
	long long flushed_vector_generation = writer_generation;
	ANT_vector_store_writer vec_writer;
	long vec_failed;
	long long docid;
	const char *vext = (quantization_current == QUANTIZE_REPLACE) ? "qvec" : "vec";

	segment_filename(vec_filename, sizeof(vec_filename), flushed_vector_generation, vext);
	vec_failed = vec_writer.create(vec_filename, vector_dimension_current) != 0;
	if (!vec_failed && quantization_current == QUANTIZE_REPLACE)
		vec_writer.set_quantization(ANT_vector_store_writer::QUANT_REPLACE);
	for (docid = 0; !vec_failed && docid < flushed_document_count; docid++)
		{
		const float *row = (writer_vector_presence[docid / 8] & (1 << (docid % 8))) ? writer_vector_data + docid * vector_dimension_current : NULL;
		vec_failed = vec_writer.append(row) != 0;
		}
	if (!vec_failed)
		vec_failed = vec_writer.finish() != 0;
	if (vec_failed)
		return 1;		// pre-manifest failure: degraded per flush()'s existing contract

	if (quantization_current == QUANTIZE_EXACT)
		{
		char qvec_name[1024];
		segment_filename(qvec_name, sizeof(qvec_name), flushed_vector_generation, "qvec");
		vec_writer.finish_qvec(qvec_name);		// best-effort; float .vec is source of truth
		}

	/*
		V2: write the signature sidecar alongside the .vec just written, signing
		each present vector with the index-wide projection.  Best-effort: a
		failure here is non-fatal to the flush -- the segment is simply
		exact-scanned until build_signatures()/compaction rebuilds it.
	*/
	if (signature_bits_current != 0 && query_signer != NULL)
		{
		char vsig_filename[1024];
		ANT_signature_store_writer sig_writer;
		unsigned char *sig = new unsigned char[query_signer->signature_bytes()];
		long sig_failed;

		segment_filename(vsig_filename, sizeof(vsig_filename), flushed_vector_generation, "vsig");
		sig_failed = sig_writer.create(vsig_filename, signature_bits_current) != 0;
		for (docid = 0; !sig_failed && docid < flushed_document_count; docid++)
			{
			if (writer_vector_presence[docid / 8] & (1 << (docid % 8)))
				{
				query_signer->sign(writer_vector_data + docid * vector_dimension_current, sig);
				sig_failed = sig_writer.append(sig) != 0;
				}
			else
				sig_failed = sig_writer.append(NULL) != 0;
			}
		if (!sig_failed)
			sig_writer.finish();
		else
			sig_writer.abandon();
		delete [] sig;
		}

	/*
		V3: build the HNSW graph sidecar alongside .vec.  Non-fatal to the
		flush -- a failure leaves the segment graph-less (exact-scanned) until
		build_hnsw()/compaction.
	*/
	if (hnsw_M_current != 0)
		{
		char hnsw_name[4096], vec_reload[4096];
		segment_filename(hnsw_name, sizeof(hnsw_name), flushed_vector_generation, "hnsw");
		segment_filename(vec_reload, sizeof(vec_reload), flushed_vector_generation, vext);
		ANT_vector_store *v = ANT_vector_store::load(vec_reload, vector_dimension_current, flushed_document_count);
		if (v->document_count() == flushed_document_count && flushed_document_count > 0)
			{
			ANT_hnsw graph;
			if (graph.build(v, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0)
				graph.save(hnsw_name);
			}
		delete v;
		}
	}

/*
	V5: persist the memory segment's multi-vectors (late-interaction rerank
	sidecar) alongside .vec/.qvec/.vsig/.hnsw, when rerank is configured and at
	least one buffered vector exists.  Same crash contract as the sibling
	sidecars above: best-effort, non-fatal to the flush -- a failure simply
	leaves the segment rerank-less until the next flush/backfill.  writer_generation
	and writer_documents are still valid here (the writer teardown below has not
	run yet), so they are used directly rather than re-capturing locals.
*/
if (rerank_configured() && writer_multivector_total > 0)
	{
	char mvec_filename[1024];
	segment_filename(mvec_filename, sizeof(mvec_filename), writer_generation, "mvec");
	ANT_multivector_store_writer mvw;
	if (mvw.create(mvec_filename, rerank_dimension_current) == 0)
		{
		mvw.set_quantization(rerank_quant_current == RERANK_QUANT_INT8
		                     ? ANT_multivector_store_writer::QUANT_INT8
		                     : ANT_multivector_store_writer::QUANT_OFF);
		long long mv_offset = 0, mv_failed = 0, docid;
		for (docid = 0; !mv_failed && docid < writer_documents; docid++)
			{
			long long m = (writer_multivector_counts != NULL) ? writer_multivector_counts[docid] : 0;
			const float *rows = (m > 0) ? writer_multivector_data + mv_offset * rerank_dimension_current : NULL;
			mv_failed = mvw.append(rows, m) != 0;
			mv_offset += m;
			}
		if (!mv_failed)
			mvw.finish();		/* best-effort: a failure leaves the segment rerank-less, non-fatal to flush */
		else
			mvw.abandon();
		}
	}

/*
	Filtered ANN: persist the memory segment's captured attribute columns
	(.attr) and opaque payloads (.pay).  Both sidecars cover ALL
	writer_documents docs -- a docid whose writer_attribute_sets slot is NULL
	(or beyond capacity) yields an empty row / empty payload -- so the loaded
	stores have document_count == the segment's doc count and are NOT treated
	as degraded.  Only then do the filter's missing-field semantics (a doc
	lacking a field => leaf false, NOT => true) apply correctly.  Best-effort
	and non-fatal to the flush, exactly like the mvec block above.
*/
if (attributes_configured())
	{
	char attr_filename[1024], pay_filename[1024];
	segment_filename(attr_filename, sizeof(attr_filename), writer_generation, "attr");
	segment_filename(pay_filename, sizeof(pay_filename), writer_generation, "pay");
	ANT_attribute_store_writer attw;
	if (attw.create(attr_filename, &attribute_schema_current) == 0)
		{
		long attr_failed = 0;
		long long docid, field, i, ncols = attribute_schema_current.count();
		for (docid = 0; docid < writer_documents; docid++)
			{
			attw.begin_document();
			ANT_attribute_set *set = (writer_attribute_sets != NULL && docid < writer_attribute_sets_capacity) ? writer_attribute_sets[docid] : NULL;
			if (set != NULL)
				for (field = 0; field < ncols; field++)
					{
					if (!set->has(field)) continue;
					int type = attribute_schema_current.type(field);
					int multi = attribute_schema_current.is_multi(field);
					if (type == ANT_attribute_schema::TYPE_INT64)
						{ if (multi) for (i = 0; i < set->ints(field); i++) attw.add_int(field, set->int_get(field, i));
						  else attw.set_int(field, set->int_get(field, 0)); }
					else if (type == ANT_attribute_schema::TYPE_STRING)
						{ if (multi) for (i = 0; i < set->strings(field); i++) attw.add_string(field, set->string_get(field, i));
						  else attw.set_string(field, set->string_get(field, 0)); }
					else /* TYPE_BOOL */
						attw.set_bool(field, set->boolean(field));
					}
			attw.end_document();
			}
		if (attw.finish() != 0) attr_failed = 1;
		(void)attr_failed;	/* best-effort: a failed .attr leaves the segment filter-less for that seg; non-fatal */
		}
	ANT_payload_store_writer payw;
	if (payw.create(pay_filename) == 0)
		{
		long long docid;
		for (docid = 0; docid < writer_documents; docid++)
			{
			ANT_attribute_set *set = (writer_attribute_sets != NULL && docid < writer_attribute_sets_capacity) ? writer_attribute_sets[docid] : NULL;
			if (set != NULL) { long long len = 0; const unsigned char *p = set->payload_bytes(&len); payw.append(p, len); }
			else payw.append(NULL, 0);
			}
		if (payw.finish() != 0) { /* best-effort */ }
		}
	}

segment_filename(del_filename, sizeof(del_filename), writer_generation, "del");
if (writer_tombstones->count() > 0)
	if (writer_tombstones->save(del_filename) != 0)
		return 1;

flushed_generation = writer_generation;

/*
	The writer is gone from here on.  Zero writer_documents (and mark the
	engine not-stale: there is nothing to rebuild) so that if a later step
	fails and we return early, add_document() and rebuild_writer_engine()
	see a consistent "no writer" state instead of NULL-dereferencing --
	graceful degradation to read-only over the already-open segments.
*/
delete writer;
writer = NULL;
delete writer_tombstones;
writer_tombstones = NULL;
writer_documents = 0;
writer_engine_stale = 0;

/*
	The segment's files are now complete on disk.  Open it for searching and
	append it to segments[] before touching the manifest.
*/
if (append_segment(flushed_generation) != 0)
	return 1;			// degraded: read-only until a successful flush()/reopen

/*
	Register the now-open segment in the manifest.  Only after this save()
	succeeds is the segment considered part of the durable index.
*/
manifest->add_segment(flushed_generation);
if (manifest->save() != 0)
	return 1;			// degraded: read-only until a successful flush()/reopen

if (start_new_writer() != 0)
	return 1;

/*
	The segment set just changed (one more disk segment, a fresh empty
	writer): recompute and repush the global ranking statistics so the next
	query scores against the new collection-wide N/mean.
*/
refresh_global_statistics();

/*
	Opportunistic maintenance: only runs when the caller has opted in via
	set_auto_maintain() (default off, so Phase 1 callers are unaffected).
	Best-effort -- maintain()'s own failure is not propagated from flush():
	the flush this call is finishing has already fully succeeded (the new
	segment is durable and the next writer is live), so a maintenance
	failure here just means the index is left with more disk segments than
	the policy would like, not that anything was lost.  The next flush() (or
	an explicit maintain() call) will retry.
*/
if (auto_maintain)
	maintain();

/*
	WAL truncation: everything the log held is now durable (it is sitting
	in the disk segment just manifested above), so the log can be
	emptied.  Best-effort -- a failed truncate() leaves records that will
	harmlessly replay again next time against already-durable state (the
	comment on truncate()'s declaration in wal.h covers this).

	Deferred while a replay is in progress: open()'s replay loop calls
	through add_document()/update_document()/delete_document(), whose
	ordinary auto-flush logic may legitimately fire partway through a
	long replay.  truncate() reopens the log file and resets its read
	position to 0 -- doing that while open()'s replay_next() loop still
	holds a position into the SAME file would end that iteration early
	(replay_next() would see an empty file and report clean EOF),
	silently losing every record after this point.  wal_truncate_pending
	records the deferral; open() truncates once, after the whole replay
	has been consumed.
*/
if (wal != NULL)
	{
	if (wal_replaying)
		wal_truncate_pending = 1;
	else
		wal->truncate();
	}

/*
	Eager token-index policy: build the V6 token graph for the segment just
	registered above (append_segment() already loaded its multivectors, so
	it is visible to build_token_index()'s loop).  Idempotent -- skips any
	segment that already has a built index -- and best-effort like the
	other flush() sidecars: a build failure just leaves that segment on the
	brute-force MaxSim fallback until the next flush/backfill.
*/
if (token_index_eager)
	build_token_index();

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::APPEND_SEGMENT()
	----------------------------------------
	Open a disk segment (generation) for searching and append it to the
	segments[] array (growing it if necessary).

	The segment is opened INDEX_IN_MEMORY (loads the whole segment into RAM,
	matching the NRT writer engine's cost model) and unquantized (quantize =
	0; quantization_bits = -1 is the indexer's own default and is irrelevant
	here since ATIRE_API::open() ignores it once the index reports itself
	quantized -- it isn't, since flush() never asks the writer to quantize).

	This build always defines FILENAME_INDEX, so ANT_memory_index::serialise()
	(called by writer->finish() in flush()) unconditionally writes the
	~documentfilenames* variables and ANT_version is compiled as ANT_V5
	(source/version.cpp).  ATIRE_API::open() / ANT_search_engine::open()
	auto-detect this from the header (ant_version starts as ANT_VX) and skip
	reading a doclist file entirely for ANT_V5 indexes -- so doclist_filename
	below is passed only because ATIRE_API::open() requires the parameter; the
	seg_G.doclist file is never created (ATIRE_indexer::finish() only closes
	id_list, and id_list itself is never opened, under #ifndef FILENAME_INDEX)
	and is never read back.  Filenames must instead be fetched with
	get_document_filename() (see search_one_segment()).
*/
long ATIRE_segment_index::append_segment(long long generation)
{
char index_filename[1024], doclist_filename[1024], del_filename[1024], vec_filename[1024], vsig_filename[1024];
ATIRE_API *engine;
long long which;

segment_filename(index_filename, sizeof(index_filename), generation, "aspt");
segment_filename(doclist_filename, sizeof(doclist_filename), generation, "doclist");
segment_filename(del_filename, sizeof(del_filename), generation, "del");
segment_filename(vec_filename, sizeof(vec_filename), generation, "vec");
segment_filename(vsig_filename, sizeof(vsig_filename), generation, "vsig");

engine = new ATIRE_API();

/*
	ATIRE_API::ant_version is not initialised by its constructor -- callers
	are expected to set it (see ATIRE_API_server::init(), atire.cpp) before
	open().  ANT_VX (-1) means "auto-detect from the index header", which is
	what ANT_search_engine::open() does when it sees ANT_VX: it reads the
	version byte written by serialise() (always ANT_V5 in this build) instead
	of demanding a match.  Without this call ant_version is garbage and
	open() spuriously fails a version-mismatch check.
*/
engine->set_ant_version(ANT_VX);
if (engine->open(ATIRE_API::INDEX_IN_MEMORY, index_filename, doclist_filename, /*quantize=*/0, /*quantization_bits=*/-1) != 0)
	{
	delete engine;
	return 1;
	}

if (segment_count >= segments_allocated)
	{
	long long new_cap = segments_allocated == 0 ? 4 : segments_allocated * 2;
	segment *new_segments = new segment[new_cap];
	for (which = 0; which < segment_count; which++)
		new_segments[which] = segments[which];
	delete [] segments;
	segments = new_segments;
	segments_allocated = new_cap;
	}

segments[segment_count].generation = generation;
segments[segment_count].engine = engine;
segments[segment_count].tombstones = ANT_index_tombstones::load(del_filename, engine->get_document_count());
if (vector_dimension_current != 0)
	{
	char qvec_filename[1024];
	segment_filename(qvec_filename, sizeof(qvec_filename), generation, "qvec");
	ANT_vector_store *v = ANT_vector_store::load(qvec_filename, vector_dimension_current, engine->get_document_count());
	if (v->document_count() == 0)		/* no/degraded .qvec -> float .vec */
		{ delete v; v = ANT_vector_store::load(vec_filename, vector_dimension_current, engine->get_document_count()); }
	segments[segment_count].vectors = v;

	segments[segment_count].exact_vectors = NULL;
	if (quantization_current == QUANTIZE_EXACT)
		{
		ANT_vector_store *ev = ANT_vector_store::load(vec_filename, vector_dimension_current, engine->get_document_count());	// float .vec
		if (ev->document_count() == engine->get_document_count() && ev->document_count() > 0 && !ev->is_quantized())
			segments[segment_count].exact_vectors = ev;
		else
			delete ev;		// no/degraded/quantized .vec -> fall back to scanning .vectors
		}
	}
else
	{
	segments[segment_count].vectors = NULL;
	segments[segment_count].exact_vectors = NULL;
	}
segments[segment_count].signatures = (vector_dimension_current != 0 && signature_bits_current != 0) ? ANT_signature_store::load(vsig_filename, signature_bits_current, engine->get_document_count()) : NULL;

if (vector_dimension_current != 0 && hnsw_M_current != 0)
	{
	char hnsw_name[4096];
	segment_filename(hnsw_name, sizeof(hnsw_name), segments[segment_count].generation, "hnsw");
	segments[segment_count].hnsw_graph = ANT_hnsw::load(hnsw_name, hnsw_M_current, hnsw_ef_construction_current, segments[segment_count].engine->get_document_count());
	}
else
	segments[segment_count].hnsw_graph = NULL;

if (rerank_configured())
	{
	char mvec_filename[1024];
	segment_filename(mvec_filename, sizeof(mvec_filename), generation, "mvec");
	segments[segment_count].multivectors = ANT_multivector_store::load(mvec_filename, rerank_dimension_current, engine->get_document_count());

	char tann_filename[1024];
	segment_filename(tann_filename, sizeof(tann_filename), generation, "tann");
	segments[segment_count].token_index = ANT_token_index::load(tann_filename, segments[segment_count].multivectors, token_index_M, token_index_ef_construction, ANT_vector_store::METRIC_DOT);
	}
else
	{
	segments[segment_count].multivectors = NULL;
	segments[segment_count].token_index = NULL;
	}

if (attributes_configured())
	{
	char attr_filename[1024], pay_filename[1024];
	segment_filename(attr_filename, sizeof(attr_filename), generation, "attr");
	segment_filename(pay_filename, sizeof(pay_filename), generation, "pay");
	segments[segment_count].attributes = ANT_attribute_store::load(attr_filename, &attribute_schema_current, engine->get_document_count());
	segments[segment_count].payload = ANT_payload_store::load(pay_filename, engine->get_document_count());
	}
else
	{
	segments[segment_count].attributes = NULL;
	segments[segment_count].payload = NULL;
	}

segment_count++;

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::REBUILD_WRITER_ENGINE()
	-----------------------------------------------
	Rebuild the NRT search view over the writer's (still-growing) in-memory
	index.  take_ownership = 0: this wrapper must NOT free the memory index
	when it is destroyed -- the writer keeps indexing into it afterwards.
*/
void ATIRE_segment_index::rebuild_writer_engine(void)
{
char **doc_list;
long long doc_count;

if (!writer_engine_stale && writer_engine != NULL)
	return;

delete writer_engine;
writer_engine = NULL;

/*
	No live writer (a previous flush() failed after tearing it down): nothing
	to wrap; search() serves only the open disk segments.
*/
if (writer == NULL)
	{
	writer_engine_stale = 0;
	return;
	}

if (writer_documents == 0)
	{
	writer_engine_stale = 0;
	return;
	}

doc_list = writer->get_doc_list(&doc_count);

writer_engine = new ATIRE_API();
writer_engine->open_from_memory_index(writer->get_index(), doc_list, doc_count, /*take_ownership=*/0);

/*
	The freshly-built NRT wrapper snapshotted its OWN (memory-segment-only)
	statistics into its ranking function; push the collection-wide global
	statistics into it so its scores line up with the disk segments'.  A no-op
	when global stats are disabled (refresh pushes the restore sentinel).
*/
refresh_global_statistics();

writer_engine_stale = 0;
}

/*
	ATIRE_SEGMENT_INDEX::GET_DOCUMENT_COUNT()
	--------------------------------------------
*/
long long ATIRE_segment_index::get_document_count(void)
{
long long total, which;

total = writer_documents - (writer_tombstones ? writer_tombstones->count() : 0);

for (which = 0; which < segment_count; which++)
	total += segments[which].engine->get_document_count() - segments[which].tombstones->count();

return total;
}

/*
	ATIRE_SEGMENT_INDEX::DISK_SEGMENT_ENGINE()
	-------------------------------------------
	Test-only accessor: the ANT_search_engine underlying a given disk segment,
	for postings-level comparisons (df/cf) that go beneath the search API.
*/
ANT_search_engine *ATIRE_segment_index::disk_segment_engine(long long which)
{
return segments[which].engine->get_search_engine();
}

/*
	ATIRE_SEGMENT_INDEX::DISK_SEGMENT_HAS_SIGNATURES()
	---------------------------------------------------
	Test-only accessor: whether the given disk segment has a cached, non-empty
	signature store loaded (i.e. its .vsig sidecar was present and valid at
	open/append time). Defined here (rather than inline in the header) so the
	header can stay include-free (forward-declares ANT_signature_store only).
*/
long ATIRE_segment_index::disk_segment_has_signatures(long long which)
{
return segments[which].signatures != NULL && segments[which].signatures->document_count() > 0;
}

/*
	ATIRE_SEGMENT_INDEX::DISK_SEGMENT_HAS_HNSW()
	-----------------------------------------------
	Test-only accessor: whether the given disk segment has a cached,
	non-empty HNSW graph loaded (i.e. its .hnsw sidecar was present and valid
	at open/append time). Defined here (rather than inline in the header) so
	the header can stay include-free (forward-declares ANT_hnsw only).
*/
long ATIRE_segment_index::disk_segment_has_hnsw(long long which)
{
return segments[which].hnsw_graph != NULL && !segments[which].hnsw_graph->empty();
}

/*
	ATIRE_SEGMENT_INDEX::SET_TOKEN_INDEX_POLICY()
	------------------------------------------------
	1 = eager (flush() builds the V6 token graph for the just-flushed segment
	immediately); 0 = ondemand (default -- callers backfill via
	build_token_index() whenever they choose).  Always succeeds.
*/
long ATIRE_segment_index::set_token_index_policy(int eager)
{
token_index_eager = eager ? 1 : 0;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::DISK_SEGMENT_HAS_TOKEN_INDEX()
	-------------------------------------------------------
	Test-only accessor: whether the given disk segment has a cached,
	non-empty V6 token index loaded (i.e. its .tann sidecar was present and
	valid at open/append time, or build_token_index()/eager flush built one).
*/
long ATIRE_segment_index::disk_segment_has_token_index(long long which)
{
return (which >= 0 && which < segment_count && segments[which].token_index != NULL && !segments[which].token_index->empty()) ? 1 : 0;
}

ANT_memory_index *ATIRE_segment_index::writer_memory_index_for_test(void)
{
return writer == NULL ? NULL : writer->get_index();
}

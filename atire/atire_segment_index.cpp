/*
	ATIRE_SEGMENT_INDEX.CPP
	-----------------------
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
#include "../source/wal.h"

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

writer_vector_data = NULL;
writer_vector_presence = NULL;
writer_vector_capacity = 0;
writer_vectors_present = 0;

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
delete wal;

for (which = 0; which < segment_count; which++)
	{
	delete segments[which].engine;
	delete segments[which].tombstones;
	delete segments[which].vectors;
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
	ATIRE_SEGMENT_INDEX::RESET_WRITER_VECTORS()
	--------------------------------------------
	Frees the memory-segment vector buffer and zeroes its bookkeeping.  Called
	from start_new_writer() (a fresh segment starts with a fresh, empty
	buffer -- docids are local to the segment) and from the destructor.  The
	vector.config state (dimension/metric) is index-wide, not per-segment, and
	is untouched here.
*/
void ATIRE_segment_index::reset_writer_vectors(void)
{
delete [] writer_vector_data;
delete [] writer_vector_presence;
writer_vector_data = NULL;
writer_vector_presence = NULL;
writer_vector_capacity = 0;
writer_vectors_present = 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_VECTOR_CONFIG()
	----------------------------------------
	Must be called before open().  The configuration is fixed for the life of
	the index; open() writes it to <dir>/vector.config on first use and
	verifies it against an existing file on every later use.
*/
long ATIRE_segment_index::set_vector_config(long long dimension, long metric)
{
if (directory != NULL)
	return 1;			// already open
if (dimension < 1 || dimension > 65536)
	return 1;
if (metric != VECTOR_METRIC_DOT && metric != VECTOR_METRIC_COSINE && metric != VECTOR_METRIC_L2)
	return 1;
pending_vector_dimension = dimension;
pending_vector_metric = metric;
vector_config_pending = 1;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_DURABLE()
	-----------------------------------
	Opts into WAL-backed durability: every mutator append()s a record after
	its engine-level success, and open() replays whatever the log holds
	before serving the index.  Must be called before open() (like
	set_vector_config()) -- there is no mechanism to retrofit replay-safe
	logging onto an index that has already been mutating without one.
*/
long ATIRE_segment_index::set_durable(long on)
{
if (directory != NULL)
	return 1;			// already open
durable = on;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_WAL_FSYNC()
	--------------------------------------
	May be called before or after open(): before open() it is remembered
	(wal_fsync_pending) and applied once the log is created; after open()
	it is applied immediately if the log already exists.
*/
void ATIRE_segment_index::set_wal_fsync(long on)
{
wal_fsync_pending = on;
if (wal != NULL)
	wal->set_fsync(on);
}

/*
	ATIRE_SEGMENT_INDEX::WAL_HEALTHY()
	-------------------------------------
	1 when durability is disabled (nothing to be unhealthy about) or when
	the WAL's own health flag is set; 0 once an append has failed.
	truncate() (on flush()'s success path) restores health.
*/
long ATIRE_segment_index::wal_healthy(void)
{
return wal == NULL ? 1 : wal->healthy();
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_VECTOR_CONFIG()
	-----------------------------------------
	Reads <dir>/vector.config (two lines: dimension, metric).  Absent file
	leaves vectors disabled.  Garbage is treated as absent (defensive parsing
	per house convention).
*/
long ATIRE_segment_index::load_vector_config(void)
{
char filename[4096], line[64];
FILE *fp;
long long dimension = 0;
long metric = -1;

snprintf(filename, sizeof(filename), "%s/vector.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fgets(line, sizeof(line), fp) != NULL)
	dimension = atoll(line);
if (fgets(line, sizeof(line), fp) != NULL)
	metric = atol(line);
fclose(fp);
if (dimension < 1 || dimension > 65536 || metric < VECTOR_METRIC_DOT || metric > VECTOR_METRIC_L2)
	return 0;			// corrupt: treat as absent
vector_dimension_current = dimension;
vector_metric = metric;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_VECTOR_CONFIG()
	-----------------------------------------
*/
long ATIRE_segment_index::save_vector_config(void)
{
char filename[4096], temp_name[4200];
FILE *fp;

snprintf(filename, sizeof(filename), "%s/vector.config", directory);
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
fprintf(fp, "%lld\n%ld\n", vector_dimension_current, vector_metric);
fclose(fp);
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::WRITER_VECTOR_APPEND()
	-------------------------------------------
	Keeps the vector buffer parallel to the writer's docids: called exactly
	once per successfully indexed document (NULL for lexical-only docs).  The
	docid is passed explicitly by the caller (add_document_core(), before
	writer_documents is incremented) rather than derived from writer_documents
	here, so the ordering of the buffer append relative to that increment is
	the caller's concern, not this function's.  Cosine-mode normalization
	happens in the caller (add_document_core()) before this is reached.
*/
long ATIRE_segment_index::writer_vector_append(long long docid, const float *vector_or_null)
{
if (writer_vector_capacity == 0)
	{
	writer_vector_capacity = 1024;
	writer_vector_data = new float[writer_vector_capacity * vector_dimension_current];
	writer_vector_presence = new unsigned char[(writer_vector_capacity + 7) / 8];
	memset(writer_vector_presence, 0, (size_t)((writer_vector_capacity + 7) / 8));
	}
if (docid >= writer_vector_capacity)
	{
	long long new_capacity = writer_vector_capacity * 2;
	while (docid >= new_capacity)
		new_capacity *= 2;
	float *new_data = new float[new_capacity * vector_dimension_current];
	unsigned char *new_presence = new unsigned char[(new_capacity + 7) / 8];
	memset(new_presence, 0, (size_t)((new_capacity + 7) / 8));
	memcpy(new_data, writer_vector_data, (size_t)(writer_vector_capacity * vector_dimension_current * sizeof(float)));
	memcpy(new_presence, writer_vector_presence, (size_t)((writer_vector_capacity + 7) / 8));
	delete [] writer_vector_data;
	delete [] writer_vector_presence;
	writer_vector_data = new_data;
	writer_vector_presence = new_presence;
	writer_vector_capacity = new_capacity;
	}
if (vector_or_null == NULL)
	memset(writer_vector_data + docid * vector_dimension_current, 0, (size_t)(vector_dimension_current * sizeof(float)));
else
	{
	memcpy(writer_vector_data + docid * vector_dimension_current, vector_or_null, (size_t)(vector_dimension_current * sizeof(float)));
	writer_vector_presence[docid / 8] |= (unsigned char)(1 << (docid % 8));
	writer_vectors_present++;
	}
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
long long ATIRE_segment_index::add_document_core(const char *key, const char *document, const float *vector)
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
	ATIRE_SEGMENT_INDEX::UPDATE_DOCUMENT()
	-----------------------------------------
	Lexical-only path: one code path with the vector overload above.
*/
long long ATIRE_segment_index::update_document(const char *key, const char *document)
{
return update_document(key, document, NULL);
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

	segment_filename(vec_filename, sizeof(vec_filename), flushed_vector_generation, "vec");
	vec_failed = vec_writer.create(vec_filename, vector_dimension_current) != 0;
	for (docid = 0; !vec_failed && docid < flushed_document_count; docid++)
		{
		const float *row = (writer_vector_presence[docid / 8] & (1 << (docid % 8))) ? writer_vector_data + docid * vector_dimension_current : NULL;
		vec_failed = vec_writer.append(row) != 0;
		}
	if (!vec_failed)
		vec_failed = vec_writer.finish() != 0;
	if (vec_failed)
		return 1;		// pre-manifest failure: degraded per flush()'s existing contract
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

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::COMPACT()
	------------------------------
	Merge the given disk segments into one new segment and swap it in.
	Ordering is crash-safe at every boundary (see the Phase 2 design spec,
	docs/superpowers/specs/2026-07-06-compacting-merge-design.md section 3):
	the output is written fully before anything references it; the
	"compacting" marker makes a mid-swap crash trigger a full keymap
	rebuild at the next open(); input files are deleted last (leftovers are
	swept as orphans).  Only disk segments may be named here -- the live
	writer is never compacted (callers pass manifest generations, all of
	which are, by construction, flushed/disk segments; see maintain()).

	Step-5 (atomic manifest swap) failure semantics: if the merge, marker
	and keymap remap (steps 1-4) all succeeded but manifest->save() then
	fails, the in-memory keymap ALREADY points every remapped key at the
	new output segment -- that cannot be undone cheaply (and undoing it
	would mean re-deriving which keys used to point where, information the
	remap loop does not retain). Rather than leave a torn state where the
	keymap and segments[] disagree, this method proceeds to complete the
	in-memory swap anyway: the inputs are dropped from segments[] (so
	search()/get_document_count() agree with the keymap) but their files
	are NOT deleted and the marker is NOT removed. The result: the
	in-process view is fully consistent (output live, inputs gone, keymap
	correct); the on-disk manifest still lists the old inputs (and not the
	output); and the leftover "compacting" marker forces open()'s
	marker-triggered keymap rebuild in open(), which
	reconstructs everything from whichever segments the reloaded manifest
	actually names. This is returned as failure (1) so the caller knows
	the on-disk state was not durably swapped, even though the running
	process now behaves as if it were.
*/
long ATIRE_segment_index::compact(long long *input_generations, long long input_count)
{
char output_name[4096], marker_name[4096], filename_buffer[4096];
long long which, input, docid;

if (input_count < 1)
	return 1;

/*
	Resolve the inputs to open segments (all must exist and be distinct).  A
	duplicate generation in input_generations[] would feed the same engine
	into merge() twice -- merge()'s N-way walk has no notion of "the same
	document seen through two inputs", so it would emit that segment's live
	documents a second time (duplicated postings, doubled document count),
	silently corrupting the output.  Reject before doing any work.
*/
segment **inputs = new segment *[input_count];
for (input = 0; input < input_count; input++)
	{
	for (which = 0; which < input; which++)
		if (input_generations[which] == input_generations[input])
			{
			delete [] inputs;
			return 1;
			}
	inputs[input] = NULL;
	for (which = 0; which < segment_count; which++)
		if (segments[which].generation == input_generations[input])
			inputs[input] = &segments[which];
	if (inputs[input] == NULL)
		{
		delete [] inputs;
		return 1;
		}
	}

/*
	Step 1: burn the output generation (manifest saved by contract --
	crash here leaves nothing, the generation number is simply skipped)
*/
long long output_generation = manifest->take_generation();
if (manifest->save() != 0)
	{
	delete [] inputs;
	return 1;
	}
segment_filename(output_name, sizeof(output_name), output_generation, "aspt");

/*
	Step 2: merge.  A crash or failure here leaves an unmanifested (or
	nonexistent) output file and the inputs untouched -- orphan-swept at
	the next open(), no data lost.
*/
ANT_search_engine **engines = new ANT_search_engine *[input_count];
ANT_index_tombstones **stones = new ANT_index_tombstones *[input_count];
for (input = 0; input < input_count; input++)
	{
	engines[input] = inputs[input]->engine->get_search_engine();
	stones[input] = inputs[input]->tombstones;
	}
ANT_index_merger *merger = new ANT_index_merger();
long merge_result = merger->merge(engines, stones, input_count, output_name);
delete merger;
delete [] engines;
delete [] stones;
if (merge_result != 0)
	{
	delete [] inputs;
	return 1;
	}

/*
	Step 2b: rewrite the vector sidecar for the merged output.  The
	renumbering below is byte-identical to the merger's: both are built from
	the same tombstones in the same input order (ANT_docid_renumberer is
	deterministic).  Inputs without vectors contribute absent rows.  A .vec
	failure aborts the compaction pre-marker, leaving the index untouched
	(the output .aspt is removed like any pre-step-3 failure).
*/
if (vector_dimension_current != 0)
	{
	long any_vectors = false;
	for (input = 0; input < input_count; input++)
		if (inputs[input]->vectors != NULL && inputs[input]->vectors->document_count() > 0)
			any_vectors = true;
	if (any_vectors)
		{
		ANT_index_tombstones **stone_list = new ANT_index_tombstones *[input_count];
		long long *doc_counts = new long long[input_count];
		for (input = 0; input < input_count; input++)
			{
			stone_list[input] = inputs[input]->tombstones;
			doc_counts[input] = inputs[input]->engine->get_document_count();
			}
		ANT_docid_renumberer *vec_renumberer = new ANT_docid_renumberer(stone_list, doc_counts, input_count);
		char vec_name[4096];
		segment_filename(vec_name, sizeof(vec_name), output_generation, "vec");
		ANT_vector_store_writer vec_writer;
		long vec_failed = vec_writer.create(vec_name, vector_dimension_current) != 0;
		for (input = 0; !vec_failed && input < input_count; input++)
			for (docid = 0; !vec_failed && docid < doc_counts[input]; docid++)
				{
				if (vec_renumberer->renumber(input, docid) < 0)
					continue;		/* tombstoned: dropped, exactly like its postings */
				const float *row = (inputs[input]->vectors != NULL && inputs[input]->vectors->has(docid)) ? inputs[input]->vectors->get(docid) : NULL;
				vec_failed = vec_writer.append(row) != 0;
				}
		if (!vec_failed)
			vec_failed = vec_writer.finish() != 0;
		delete vec_renumberer;
		delete [] stone_list;
		delete [] doc_counts;
		if (vec_failed)
			{
			remove(output_name);
			delete [] inputs;
			return 1;
			}
		}
	}

/*
	Step 3: marker -- from here until removal, a crash makes the next
	open() rebuild the keymap from the segments rather than trust the log
*/
snprintf(marker_name, sizeof(marker_name), "%s/compacting", directory);
FILE *marker = fopen(marker_name, "wb");
if (marker == NULL)
	{
	remove(output_name);
	delete [] inputs;
	return 1;
	}
fclose(marker);

/*
	Step 4: open the output and repoint the keymap at it.  Every document
	in the output is, by construction (the merger only emits live docs),
	the live copy of its key -- scan docid-ascending and unconditionally
	overwrite each key's keymap entry.
*/
if (append_segment(output_generation) != 0)
	{
	remove(output_name);
	remove(marker_name);
	delete [] inputs;
	return 1;
	}
segment *output_segment = &segments[segment_count - 1];
for (docid = 0; docid < output_segment->engine->get_document_count(); docid++)
	{
	char *filename = output_segment->engine->get_document_filename(filename_buffer, docid);
	if (filename != NULL && filename[0] != '\0')
		keymap->add(filename, output_generation, docid);
	}

/*
	Step 5: atomic manifest swap.  See the banner above for what happens
	if save() fails here -- the keymap is already remapped, so we proceed
	to step 6's in-memory removal regardless, but skip the file deletions
	and marker removal (the marker forces a keymap rebuild -- reconciling
	everything -- at the next open()).
*/
for (input = 0; input < input_count; input++)
	manifest->remove_segment(input_generations[input]);
manifest->add_segment(output_generation);
long manifest_swapped = manifest->save() == 0;

/*
	Step 6: drop the inputs from segments[] (in-memory swap complete
	either way).  inputs[] holds pointers into segments[], which THIS
	shuffle invalidates as soon as the first removal happens -- so each
	iteration re-finds its victim by generation rather than trusting the
	now-stale inputs[input] pointer.
*/
for (input = 0; input < input_count; input++)
	for (which = 0; which < segment_count; which++)
		if (segments[which].generation == input_generations[input])
			{
			delete segments[which].engine;
			delete segments[which].tombstones;
			delete segments[which].vectors;
			for (long long shuffle = which; shuffle < segment_count - 1; shuffle++)
				segments[shuffle] = segments[shuffle + 1];
			segment_count--;
			break;
			}

delete [] inputs;

if (!manifest_swapped)
	return 1;			// degraded but consistent -- see the banner above

/*
	Only once the manifest swap is durable is it safe to delete the input
	files and consume the marker.
*/
for (input = 0; input < input_count; input++)
	delete_segment_files(input_generations[input]);
remove(marker_name);

/*
	The compacted output replaced its inputs (a new engine with its own local
	statistics): repush the collection-wide global statistics so scores stay
	single-segment-equivalent.  The total N is unchanged by a merge, but the
	output engine snapshotted its own locals at append_segment() time.
*/
refresh_global_statistics();

return 0;
}

/*
	TIER_OF()
	---------
	Size tier = number of decimal digits in the live-document count
	(1-9 -> tier 1, 10-99 -> tier 2, ...).  File-local: only maintain() uses it.
*/
static long tier_of(long long live_documents)
{
long tier = 1;

while (live_documents >= 10)
	{
	live_documents /= 10;
	tier++;
	}
return tier;
}

/*
	ATIRE_SEGMENT_INDEX::MAINTAIN()
	-------------------------------
	Run the tiered merge policy (design spec section 4) to quiescence, over
	disk segments only -- the live writer is never a compact() input.

	Triggers, evaluated fresh on every iteration (segment_count/generations
	change after each compact()):
	1. Tier trigger: bucket disk segments by tier_of(live document count);
	   the first tier holding >= merge_factor members has ALL of its members
	   merged in one compact() call.
	2. Tombstone trigger: any segment whose tombstones->count()/document_count
	   exceeds tombstone_compact_ratio joins the candidate set too (added
	   after the tier trigger, deduplicated against it) -- if the tier
	   trigger did not fire, an over-deleted segment is compacted alone
	   (a 1-way "merge", which just rewrites it without its dead documents;
	   see ANT_index_merger::merge()'s N-way walk, which degrades to N=1
	   without any special-casing).

	Stops when neither trigger fires (0 candidates -> quiescent) or after a
	safety cap of 10 iterations, so a pathological inventory that keeps
	re-triggering cannot loop maintain() forever; a later maintain() call
	picks up where this one left off.
*/
long ATIRE_segment_index::maintain(void)
{
long long candidates[1024];
long long candidate_count, which, other;
long iteration;
long any_compacted = 0;

for (iteration = 0; iteration < 10; iteration++)
	{
	candidate_count = 0;

	/*
		Tier trigger: find the first tier with >= merge_factor members
	*/
	for (which = 0; which < segment_count && candidate_count == 0; which++)
		{
		long long in_tier = 0;
		long tier = tier_of(segments[which].engine->get_document_count() - segments[which].tombstones->count());
		for (other = 0; other < segment_count; other++)
			if (tier_of(segments[other].engine->get_document_count() - segments[other].tombstones->count()) == tier)
				in_tier++;
		if (in_tier >= merge_factor)
			for (other = 0; other < segment_count && candidate_count < 1024; other++)
				if (tier_of(segments[other].engine->get_document_count() - segments[other].tombstones->count()) == tier)
					candidates[candidate_count++] = segments[other].generation;
		}

	/*
		Tombstone trigger: over-deleted segments join (or run alone)
	*/
	for (which = 0; which < segment_count && candidate_count < 1024; which++)
		{
		long long docs = segments[which].engine->get_document_count();
		if (docs > 0 && (double)segments[which].tombstones->count() / (double)docs > tombstone_compact_ratio)
			{
			long already_in = false;
			for (other = 0; other < candidate_count; other++)
				if (candidates[other] == segments[which].generation)
					already_in = true;
			if (!already_in)
				candidates[candidate_count++] = segments[which].generation;
			}
		}

	if (candidate_count == 0)
		{
		if (any_compacted)
			keymap->compact_log();		// best-effort; compaction floods log with remap records
		return 0;					// quiescent
		}
	if (compact(candidates, candidate_count) != 0)
		return 1;
	any_compacted = 1;
	}
if (any_compacted)
	keymap->compact_log();				// best-effort; compaction floods log with remap records
return 0;						// safety cap: good enough, next maintain() continues
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
char index_filename[1024], doclist_filename[1024], del_filename[1024], vec_filename[1024];
ATIRE_API *engine;
long long which;

segment_filename(index_filename, sizeof(index_filename), generation, "aspt");
segment_filename(doclist_filename, sizeof(doclist_filename), generation, "doclist");
segment_filename(del_filename, sizeof(del_filename), generation, "del");
segment_filename(vec_filename, sizeof(vec_filename), generation, "vec");

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
segments[segment_count].vectors = vector_dimension_current != 0 ? ANT_vector_store::load(vec_filename, vector_dimension_current, engine->get_document_count()) : NULL;
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
	ATIRE_SEGMENT_INDEX::REFRESH_GLOBAL_STATISTICS()
	------------------------------------------------
	Pushes global N and mean document length into every open engine (disk
	segments + the NRT wrapper) and rebuilds their ranking functions.  When
	disabled, pushes the restore sentinel instead.  Called at every boundary
	that changes the segment set; O(segments).
*/
void ATIRE_segment_index::refresh_global_statistics(void)
{
long long which, total_documents = 0;
double total_length = 0.0;

if (!global_stats_enabled)
	{
	for (which = 0; which < segment_count; which++)
		segments[which].engine->apply_global_statistics(0, 0.0);
	if (writer_engine != NULL)
		writer_engine->apply_global_statistics(0, 0.0);
	return;
	}

for (which = 0; which < segment_count; which++)
	{
	long long docs = segments[which].engine->get_document_count();
	double mean = 0.0;

	if (docs <= 0)
		continue;		// nothing to contribute (and no trustworthy mean to read)
	segments[which].engine->get_search_engine()->get_document_lengths(&mean);
	total_documents += docs;
	total_length += (double)docs * mean;
	}
if (writer_engine != NULL)
	{
	double mean = 0.0;

	writer_engine->get_search_engine()->get_document_lengths(&mean);
	total_documents += writer_documents;
	total_length += (double)writer_documents * mean;
	}
else
	total_documents += writer_documents;	/* lengths unknown until a wrapper exists; next refresh corrects */

if (total_documents == 0)
	return;
double global_mean = total_length / (double)total_documents;

/*
	Defence in depth: never push a non-finite or non-positive mean -- a
	poisoned mean would NaN every length-normalised score in every segment
	(worse than the per-segment drift this feature fixes).  !(x > 0.0)
	catches NaN as well as zero/negative without needing isfinite().
*/
if (!(global_mean > 0.0))
	return;
for (which = 0; which < segment_count; which++)
	segments[which].engine->apply_global_statistics(total_documents, global_mean);
if (writer_engine != NULL)
	writer_engine->apply_global_statistics(total_documents, global_mean);
}

/*
	ATIRE_SEGMENT_INDEX::SET_GLOBAL_STATS()
	---------------------------------------
	Toggle cross-segment global ranking statistics (default on).  Refreshes
	immediately when the index is already open so the change takes effect on
	the next query without waiting for a flush/compact boundary.
*/
void ATIRE_segment_index::set_global_stats(long on)
{
global_stats_enabled = on;
if (directory != NULL)
	refresh_global_statistics();
}

/*
	ATIRE_SEGMENT_INDEX::RESET_RESULTS()
	-------------------------------------
	Free the filenames owned by results[0, results_count) and zero
	results_count, ready for a fresh round of append_result() calls.  Shared
	by search(), search_vector() and search_hybrid(), which all overwrite the
	same results[] array on each call.
*/
void ATIRE_segment_index::reset_results(void)
{
long long which;

for (which = 0; which < results_count; which++)
	delete [] results[which].filename;
results_count = 0;
}

/*
	ATIRE_SEGMENT_INDEX::APPEND_RESULT()
	---------------------------------------
	Ensure results[] has room for one more hit (growing by doubling, from an
	initial capacity of 256, copying the existing entries across), then
	reserve the next slot and return a pointer to it for the caller to fill.
*/
ATIRE_segment_index::hit *ATIRE_segment_index::append_result(void)
{
if (results_count >= results_allocated)
	{
	long long new_cap = results_allocated == 0 ? 256 : results_allocated * 2;
	hit *new_results = new hit[new_cap];
	long long i;

	for (i = 0; i < results_count; i++)
		new_results[i] = results[i];
	delete [] results;
	results = new_results;
	results_allocated = new_cap;
	}

results_count++;
return &results[results_count - 1];
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_ONE_SEGMENT()
	--------------------------------------------
	Run query against one already-open segment engine and merge its (score,
	generation, docid, filename) results into the shared results[] array,
	skipping any docid that segment's tombstones mark deleted.
*/
void ATIRE_segment_index::search_one_segment(ATIRE_API *engine, ANT_index_tombstones *tombstones, long long generation, char *query, long long top_k, long use_filename_index)
{
char query_copy[MAX_TERM_LENGTH];
long long fetch, hits, which, docid, list_len;
ANT_search_engine *se;
ANT_search_engine_result *list;
ANT_search_engine_accumulator *accumulator;
char *filename;
char filename_index_buffer[4096];		// used only when use_filename_index (disk segments)

fetch = top_k + (tombstones ? tombstones->count() : 0);

strncpy(query_copy, query, sizeof(query_copy) - 1);
query_copy[sizeof(query_copy) - 1] = '\0';

hits = engine->search(query_copy, fetch);

se = engine->get_search_engine();
list = se->get_results_list();
list_len = list->results_list_length;

for (which = 0; which < hits && which < fetch && which < list_len; which++)
	{
	accumulator = list->accumulator_pointers[which];
	docid = accumulator - list->accumulator;

	if (tombstones != NULL && tombstones->is_deleted(docid))
		continue;

	/*
		Disk segments are reopened via ATIRE_API::open() as ANT_V5 (this build's
		serialise() always writes the FILENAME_INDEX filename table); the memory
		writer's NRT view is wired up via open_from_memory_index(), which forces
		ant_version = ANT_V3 and never populates the filename-index tables.  Use
		the accessor that matches which one this engine actually is.
	*/
	if (use_filename_index)
		filename = engine->get_document_filename(filename_index_buffer, docid);
	else
		filename = engine->get_document_filename_from_doclist(docid);

	hit *slot = append_result();

	slot->generation = generation;
	slot->docid = docid;
	slot->score = accumulator->get_rsv();
	if (filename != NULL)
		{
		slot->filename = new char[strlen(filename) + 1];
		strcpy(slot->filename, filename);
		}
	else
		slot->filename = NULL;
	}
}

/*
	Sort comparator for the merged results[] array: score descending, then
	(generation, docid) ascending for deterministic ties.
*/
static int ATIRE_segment_index_hit_cmp(const void *a, const void *b)
{
const ATIRE_segment_index::hit *ha = (const ATIRE_segment_index::hit *)a;
const ATIRE_segment_index::hit *hb = (const ATIRE_segment_index::hit *)b;

if (ha->score > hb->score)
	return -1;
if (ha->score < hb->score)
	return 1;
if (ha->generation != hb->generation)
	return ha->generation < hb->generation ? -1 : 1;
if (ha->docid != hb->docid)
	return ha->docid < hb->docid ? -1 : 1;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH()
	--------------------------------
*/
long long ATIRE_segment_index::search(char *query, long long top_k)
{
long long which;

reset_results();

for (which = 0; which < segment_count; which++)
	search_one_segment(segments[which].engine, segments[which].tombstones, segments[which].generation, query, top_k, /*use_filename_index=*/1);

rebuild_writer_engine();
if (writer_engine != NULL)
	search_one_segment(writer_engine, writer_tombstones, writer_generation, query, top_k, /*use_filename_index=*/0);

qsort(results, (size_t)results_count, sizeof(*results), ATIRE_segment_index_hit_cmp);

if (results_count > top_k)
	{
	/*
		Free the filenames of the entries the truncation cuts: both the
		free loop at the top of this method and the destructor only walk
		[0, results_count), so anything past top_k would otherwise leak.
	*/
	for (which = top_k; which < results_count; which++)
		delete [] results[which].filename;
	results_count = top_k;
	}

return results_count;
}

/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES()
	----------------------------------------
	Exact top-k across every disk segment's vector store and the live memory
	buffer.  In cosine mode the query is normalized here (stored vectors were
	normalized at insertion -- see add_document_core()).  Returns the
	candidate count; caller supplies best[top_k].
*/
long long ATIRE_segment_index::vector_candidates(const float *query, long long top_k, ANT_vector_candidate *best)
{
long long which, docid, best_count = 0;
float *normalized = NULL;

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;

if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{
		delete [] normalized;
		return 0;
		}
	query = normalized;
	}

for (which = 0; which < segment_count; which++)
	if (segments[which].vectors != NULL)
		segments[which].vectors->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k);

for (docid = 0; docid < writer_documents; docid++)
	{
	if (writer_vector_presence == NULL || !(writer_vector_presence[docid / 8] & (1 << (docid % 8))))
		continue;
	if (writer_tombstones->is_deleted(docid))
		continue;
	ANT_vector_candidate_insert(best, &best_count, top_k, ANT_vector_store::kernel(query, writer_vector_data + docid * vector_dimension_current, vector_dimension_current, vector_metric), writer_generation, docid);
	}

delete [] normalized;
return best_count;
}

/*
	ATIRE_SEGMENT_INDEX::RESOLVE_HIT_FILENAME()
	--------------------------------------------
	Memory-segment docs resolve through the writer's doc list; disk segments
	through the engine's filename-index accessor (see search_one_segment()'s
	banner for why the two engines need different accessors).
*/
char *ATIRE_segment_index::resolve_hit_filename(long long generation, long long docid, char *buffer, long long buffer_size)
{
long long which, count;
char **doc_list;

if (writer != NULL && generation == writer_generation)
	{
	doc_list = writer->get_doc_list(&count);
	if (docid < count && doc_list[docid] != NULL)
		{
		snprintf(buffer, (size_t)buffer_size, "%s", doc_list[docid]);
		return buffer;
		}
	return NULL;
	}
for (which = 0; which < segment_count; which++)
	if (segments[which].generation == generation)
		return segments[which].engine->get_document_filename(buffer, docid);
return NULL;
}

/*
	VECTOR_CANDIDATE_COMPARE()
	---------------------------
	qsort comparator for vector-search results: score descending, ties broken
	by (generation, docid) ascending for deterministic output.
*/
static int vector_candidate_compare(const void *a, const void *b)
{
const ANT_vector_candidate *one = (const ANT_vector_candidate *)a;
const ANT_vector_candidate *two = (const ANT_vector_candidate *)b;

if (one->score > two->score)
	return -1;
if (one->score < two->score)
	return 1;
if (one->generation != two->generation)
	return one->generation < two->generation ? -1 : 1;
return one->docid < two->docid ? -1 : (one->docid == two->docid ? 0 : 1);
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_VECTOR()
	--------------------------------------
	Exact top-k dense-vector search across the live memory buffer and every
	open disk segment's vector store.  Mirrors search()'s results[] contract
	(prior hits' filenames freed at entry) -- results[]/results_count are
	shared with search(); only one of the two result sets exists at a time.
*/
long long ATIRE_segment_index::search_vector(const float *query, long long top_k)
{
char filename_buffer[4096];
long long which, count;
ANT_vector_candidate *best;

reset_results();

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;

best = new ANT_vector_candidate[top_k];
count = vector_candidates(query, top_k, best);
qsort(best, (size_t)count, sizeof(*best), vector_candidate_compare);

for (which = 0; which < count; which++)
	{
	char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));

	hit *slot = append_result();

	slot->generation = best[which].generation;
	slot->docid = best[which].docid;
	slot->score = best[which].score;
	if (filename != NULL)
		{
		slot->filename = new char[strlen(filename) + 1];
		strcpy(slot->filename, filename);
		}
	else
		{
		slot->filename = new char[1];
		slot->filename[0] = '\0';
		}
	}

delete [] best;
return results_count;
}

/*
	struct ANT_FUSED_CANDIDATE
	---------------------------
	Bundles the RRF-scored candidate with its filename so the two travel
	together through qsort() (a parallel filename array desynchronizes from
	the candidate array once qsort reorders one but not the other).
*/
struct ANT_fused_candidate
{
ANT_vector_candidate candidate;
char *filename;		// owned; NULL only if not yet resolved (never published that way)
} ;

/*
	ANT_FUSED_CANDIDATE_COMPARE()
	-----------------------------
	qsort comparator for fused candidates: score descending, ties broken by
	(generation, docid) ascending, mirroring vector_candidate_compare().
*/
static int ANT_fused_candidate_compare(const void *a, const void *b)
{
const ANT_fused_candidate *one = (const ANT_fused_candidate *)a;
const ANT_fused_candidate *two = (const ANT_fused_candidate *)b;

if (one->candidate.score > two->candidate.score)
	return -1;
if (one->candidate.score < two->candidate.score)
	return 1;
if (one->candidate.generation != two->candidate.generation)
	return one->candidate.generation < two->candidate.generation ? -1 : 1;
if (one->candidate.docid != two->candidate.docid)
	return one->candidate.docid < two->candidate.docid ? -1 : 1;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_HYBRID()
	------------------------------------
	Reciprocal Rank Fusion of the lexical top-k and the vector top-k:
	fused(d) = sum over lists containing d of 1 / (60 + rank_d), ranks
	1-based.  60 is the standard RRF constant.  Either side may be absent;
	the result degrades to the other side (still RRF-scored, order preserved).

	The candidate and its filename are carried together in a single
	ANT_fused_candidate[] array (see struct above, just up) so that qsort()
	cannot desynchronize them; a parallel filename array would move the
	ANT_vector_candidate rows without moving the corresponding filename rows.
*/
long long ATIRE_segment_index::search_hybrid(char *query_text, const float *query_vector, long long top_k)
{
long long lexical_count = 0, vector_count = 0, fused_count = 0, which, other;
char filename_buffer[4096];

if (top_k < 1)
	return 0;

/*
	Lexical side first: run the existing search and snapshot its hits (the
	results array is shared, so the snapshot must deep-copy the filenames)
	into the fused array before it gets overwritten.
*/
ANT_fused_candidate *fused = new ANT_fused_candidate[top_k * 2];

if (query_text != NULL && *query_text != '\0')
	lexical_count = search(query_text, top_k);
for (which = 0; which < lexical_count; which++)
	{
	fused[fused_count].candidate.generation = results[which].generation;
	fused[fused_count].candidate.docid = results[which].docid;
	fused[fused_count].candidate.score = 1.0 / (60.0 + (double)(which + 1));
	fused[fused_count].filename = new char[strlen(results[which].filename) + 1];
	strcpy(fused[fused_count].filename, results[which].filename);
	fused_count++;
	}

/*
	Vector side: candidates + rank contribution, merged into the fused set
	by (generation, docid) identity.
*/
if (query_vector != NULL && vector_dimension_current != 0)
	{
	ANT_vector_candidate *best = new ANT_vector_candidate[top_k];
	vector_count = vector_candidates(query_vector, top_k, best);
	qsort(best, (size_t)vector_count, sizeof(*best), vector_candidate_compare);
	for (which = 0; which < vector_count; which++)
		{
		double contribution = 1.0 / (60.0 + (double)(which + 1));
		long found = false;
		for (other = 0; other < fused_count; other++)
			if (fused[other].candidate.generation == best[which].generation && fused[other].candidate.docid == best[which].docid)
				{
				fused[other].candidate.score += contribution;
				found = true;
				break;
				}
		if (!found)
			{
			fused[fused_count].candidate.generation = best[which].generation;
			fused[fused_count].candidate.docid = best[which].docid;
			fused[fused_count].candidate.score = contribution;
			char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));
			fused[fused_count].filename = new char[(filename != NULL ? strlen(filename) : 0) + 1];
			strcpy(fused[fused_count].filename, filename != NULL ? filename : "");
			fused_count++;
			}
		}
	delete [] best;
	}

/*
	Sort fused by score desc (ties: generation, docid asc) -- candidate and
	filename move together, so this cannot desynchronize them -- then
	truncate and publish into the shared results array.  The lexical
	search() call above already freed the PREVIOUS results at its entry (or,
	if query_text was NULL/empty, the results array is whatever it held
	before this call); either way those filenames are snapshotted into
	fused[] by now, so free them here before repopulating.
*/
qsort(fused, (size_t)fused_count, sizeof(*fused), ANT_fused_candidate_compare);

reset_results();

long long publish = fused_count < top_k ? fused_count : top_k;
for (which = 0; which < publish; which++)
	{
	hit *slot = append_result();

	slot->generation = fused[which].candidate.generation;
	slot->docid = fused[which].candidate.docid;
	slot->score = fused[which].candidate.score;
	slot->filename = fused[which].filename;		/* ownership transfer */
	fused[which].filename = NULL;
	}
for (which = publish; which < fused_count; which++)
	delete [] fused[which].filename;
delete [] fused;
return results_count;
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

ANT_memory_index *ATIRE_segment_index::writer_memory_index_for_test(void)
{
return writer == NULL ? NULL : writer->get_index();
}

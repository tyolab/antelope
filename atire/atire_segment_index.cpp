/*
	ATIRE_SEGMENT_INDEX.CPP
	-----------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

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

results = NULL;
results_count = 0;
results_allocated = 0;
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

for (which = 0; which < segment_count; which++)
	{
	delete segments[which].engine;
	delete segments[which].tombstones;
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

delete writer_engine;
writer_engine = NULL;
writer_engine_stale = 1;

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::REBUILD_KEYMAP()
	-----------------------------------------
	TASK 11: the keymap is, by design, a CACHE over information the segments
	already carry themselves -- each segment stores its documents' filenames
	internally (this build always serialises FILENAME_INDEX-style, ANT_V5;
	see append_segment()'s banner) -- so if keymap.log is lost, the map can
	be reconstructed by scanning every open disk segment's stored filenames
	rather than losing the ability to look documents up by key.

	segments[] is in manifest order, which is the order generations were
	added (= append/flush order), i.e. OLDEST first.  Walking it in that
	order and simply calling keymap->add() for every live document means a
	key that appears in two segments (an update whose tombstone was itself
	recorded only in the lost keymap.log) ends up pointing at the segment
	processed LAST -- the newest one -- exactly the "newer copy wins"
	semantics an update provides.  But that leaves the OLDER copy silently
	still reachable through the segment's own doclist / accumulator scan
	(search_one_segment() does not consult the keymap, only tombstones), so
	it must also be explicitly tombstoned: it was already dead before the
	log was lost (its update is the very reason it is being superseded now),
	and a live-looking stale duplicate would otherwise resurface in search
	results and could later be resurrected by update_document()/
	delete_document() acting on it via a keymap collision.
*/
void ATIRE_segment_index::rebuild_keymap(void)
{
long long which, docid, doc_count;
long long old_generation, old_docid;
char filename_buffer[4096];
char *filename;

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
			earlier segment's copy is the OLDER one -- it was superseded by
			an update whose tombstone lived only in the log we just lost.
			Tombstone it now (persists a .del immediately) so it stops
			showing up in search() and cannot be resurrected via the keymap.
		*/
		if (keymap->find(filename, &old_generation, &old_docid))
			tombstone(old_generation, old_docid);

		keymap->add(filename, segments[which].generation, docid);
		}
	}
}

/*
	ATIRE_SEGMENT_INDEX::OPEN()
	------------------------------
*/
long ATIRE_segment_index::open(const char *directory)
{
long long which;

this->directory = new char[strlen(directory) + 1];
strcpy(this->directory, directory);

manifest = ANT_index_manifest::load(this->directory);

/*
	Must be captured BEFORE ANT_index_keymap::load(), which -- when the log
	file does not exist -- fopen("ab")s it into existence as a side effect
	of preparing to append (see index_keymap.cpp's load()).  After that call
	the file always exists, so this is the only point at which "was there
	really a keymap.log going into this open()" can still be asked.
*/
long had_keymap_log = ANT_index_keymap::log_exists(this->directory);
keymap = ANT_index_keymap::load(this->directory);

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
	Reconcile the keymap against the manifest BEFORE the new writer's
	generation exists: keymap entries can reference generations that were
	added to but never flushed (a session that exited without flush()ing
	its memory segment).  On reopen those entries are lies -- the segment
	they point at does not exist and never will -- so any live keymap
	entry whose generation is not one of the manifest's segments must be
	dropped now, or delete_document() would fail confusingly and
	update_document() would tombstone into nothing.  Only manifest
	generations are valid at this point; the current session's writer
	generation is handed out (and starts collecting keymap entries) below,
	after this check.
*/
long long *manifest_generations = new long long[manifest->segment_count()];
for (which = 0; which < manifest->segment_count(); which++)
	manifest_generations[which] = manifest->get_segment(which);
keymap->retain_generations(manifest_generations, manifest->segment_count());
delete [] manifest_generations;

/*
	ORPHAN SWEEP
	------------
	Remove seg_* files whose generation the manifest does not reference.
	These arise from: (a) a session that died with an unflushed writer (its
	.aspt/.doclist were created by start_new_writer()'s init() but the
	generation was never added to the manifest, since add_segment() only
	happens after a successful flush()); or (b) a crash/I-O failure between
	flush() writing the segment's files and flush() saving the manifest.

	In case (b) the segment is COMPLETE on disk -- but flush()'s durability
	contract is that a flush is durable only once the manifest save
	succeeds, so a segment the manifest never came to reference is, by
	definition, not durable: this sweep deletes it, and any documents that
	were only in it are lost BY DESIGN.  This must run before
	start_new_writer() below claims the next generation, so it can never
	touch the new writer's own (not-yet-manifested) files.
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
	Keymap recovery (Task 11): if there was no keymap.log going into this
	open(), the keymap loaded above is empty (retain_generations() just ran
	against it and had nothing to do) and every already-open segment is a
	source of ground truth for it -- rebuild by scanning their stored
	filenames.  Runs after the orphan sweep (segments[] must reflect only
	the segments that are actually durable) and before start_new_writer()
	claims the next generation (rebuild only concerns already-flushed
	segments; the new writer starts with nothing to rebuild anyway).  The
	rebuilt entries are appended through keymap->add()/tombstone(), whose
	log handle load() already opened for append, so the rebuild itself
	persists for the next open().
*/
if (!had_keymap_log && segment_count > 0)
	rebuild_keymap();

if (start_new_writer() != 0)
	return 1;

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::ADD_DOCUMENT()
	--------------------------------------
	The indexer's parser normalises tokens in place, so both the key and the
	document must be writable heap buffers (string literals fault); we copy
	both, index, then free the copies (the indexer keeps its own copies of
	whatever it needs internally, e.g. the doclist).
*/
long long ATIRE_segment_index::add_document(const char *key, const char *document)
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

writer_documents++;
writer_engine_stale = 1;

keymap->add(key, writer_generation, docid);

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
	TASK 9: upsert.  Add the new content FIRST, then tombstone the old
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
long long ATIRE_segment_index::update_document(const char *key, const char *document)
{
long long old_generation, old_docid;
long had_old = keymap->find(key, &old_generation, &old_docid);

long long handle = add_document(key, document);		// also repoints the keymap at the new copy
if (handle < 0)
	return -1;
if (had_old)
	tombstone(old_generation, old_docid);
return handle;
}

/*
	ATIRE_SEGMENT_INDEX::DELETE_DOCUMENT()
	-----------------------------------------
	TASK 9: look the key up in the keymap, mark it deleted in the owning
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
	ANT_memory_index::serialise(), which -- via the Task 5 quantization_bits
	save/restore -- expects to be the sole owner at that point).
*/
delete writer_engine;
writer_engine = NULL;
writer_engine_stale = 1;

if (writer->finish() == 0)		// docno <= 0 (shouldn't happen: writer_documents > 0 above) or serialise() failed
	return 1;

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

return start_new_writer();
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
char index_filename[1024], doclist_filename[1024], del_filename[1024];
ATIRE_API *engine;
long long which;

segment_filename(index_filename, sizeof(index_filename), generation, "aspt");
segment_filename(doclist_filename, sizeof(doclist_filename), generation, "doclist");
segment_filename(del_filename, sizeof(del_filename), generation, "del");

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

writer_engine_stale = 0;
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

	if (results_count >= results_allocated)
		{
		long long new_cap = results_allocated == 0 ? 16 : results_allocated * 2;
		hit *new_results = new hit[new_cap];
		long long i;
		for (i = 0; i < results_count; i++)
			new_results[i] = results[i];
		delete [] results;
		results = new_results;
		results_allocated = new_cap;
		}

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

	results[results_count].generation = generation;
	results[results_count].docid = docid;
	results[results_count].score = accumulator->get_rsv();
	if (filename != NULL)
		{
		results[results_count].filename = new char[strlen(filename) + 1];
		strcpy(results[results_count].filename, filename);
		}
	else
		results[results_count].filename = NULL;

	results_count++;
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

for (which = 0; which < results_count; which++)
	delete [] results[which].filename;
results_count = 0;

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

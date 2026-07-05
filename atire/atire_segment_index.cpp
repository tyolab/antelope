/*
	ATIRE_SEGMENT_INDEX.CPP
	-----------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atire_segment_index.h"
#include "atire_api.h"
#include "indexer.h"
#include "../source/index_manifest.h"
#include "../source/index_keymap.h"
#include "../source/index_tombstones.h"
#include "../source/search_engine.h"
#include "../source/search_engine_result.h"
#include "../source/search_engine_accumulator.h"

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

flush_after_documents = 0;

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

snprintf(options, sizeof(options), "atire_segment_writer -nologo -findex %s -fdoclist %s", index_filename, doclist_filename);

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
	ATIRE_SEGMENT_INDEX::OPEN()
	------------------------------
*/
long ATIRE_segment_index::open(const char *directory)
{
this->directory = new char[strlen(directory) + 1];
strcpy(this->directory, directory);

manifest = ANT_index_manifest::load(this->directory);
keymap = ANT_index_keymap::load(this->directory);

/*
	TASK 7: reopen existing disk segments listed in the manifest here (build
	an ATIRE_API + ANT_index_tombstones per segment, populate segments[]).
*/

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
long long docid;

if (key == NULL || document == NULL)
	return -1;

key_copy = new char[strlen(key) + 1];
strcpy(key_copy, key);

doc_copy = new char[strlen(document) + 1];
strcpy(doc_copy, document);

writer->index_document(key_copy, doc_copy);

/*
	ATIRE_indexer::get_docno() is 1-based (docno is pre-incremented before
	the first document is indexed), but the search engine's accumulator
	array -- and so the docid we must hand back for get_document_filename()
	and get_hit()->docid to line up with -- is 0-based (docid = pointer
	arithmetic into a zero-based array; doc_list[] is likewise appended
	0-based).  Subtract 1 here so make_handle()'s docid matches what
	search_one_segment() will report for the same document.
*/
docid = writer->get_docno() - 1;

delete [] key_copy;
delete [] doc_copy;

writer_documents++;
writer_engine_stale = 1;

keymap->add(key, writer_generation, docid);

return make_handle(writer_generation, docid);
}

/*
	ATIRE_SEGMENT_INDEX::UPDATE_DOCUMENT()
	-----------------------------------------
	TASK 9: upsert (tombstone the old (generation, docid) via the keymap,
	then add_document() the new content).
	note: empty documents roll back docno in the indexer -- add_document
	handle aliasing guard needed.
*/
long long ATIRE_segment_index::update_document(const char *key, const char *document)
{
return -1;
}

/*
	ATIRE_SEGMENT_INDEX::DELETE_DOCUMENT()
	-----------------------------------------
	TASK 9: look the key up in the keymap, mark it deleted in the owning
	segment's tombstones (writer_tombstones or segments[which].tombstones),
	and remove it from the keymap.
	note: empty documents roll back docno in the indexer -- add_document
	handle aliasing guard needed.
*/
long ATIRE_segment_index::delete_document(const char *key)
{
return 1;
}

/*
	ATIRE_SEGMENT_INDEX::FLUSH()
	-------------------------------
	TASK 7: serialise the writer's memory index + tombstones to disk as an
	immutable segment, register it in the manifest, append it to segments[],
	then start_new_writer() for the next generation.
*/
long ATIRE_segment_index::flush(void)
{
return 1;
}

/*
	ATIRE_SEGMENT_INDEX::APPEND_SEGMENT()
	----------------------------------------
	TASK 7: open a disk segment (generation) for searching and append it to
	the segments[] array (growing it if necessary).
*/
long ATIRE_segment_index::append_segment(long long generation)
{
return 1;
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
void ATIRE_segment_index::search_one_segment(ATIRE_API *engine, ANT_index_tombstones *tombstones, long long generation, char *query, long long top_k)
{
char query_copy[MAX_TERM_LENGTH];
long long fetch, hits, which, docid, list_len;
ANT_search_engine *se;
ANT_search_engine_result *list;
ANT_search_engine_accumulator *accumulator;
char *filename;

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

/*
	TASK 7: search each open disk segment (segments[0..segment_count)) here,
	same as the writer engine below.
*/

rebuild_writer_engine();
if (writer_engine != NULL)
	search_one_segment(writer_engine, writer_tombstones, writer_generation, query, top_k);

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

/*
	TASK 7: add each disk segment's live document count (writer_documents
	equivalent - tombstones->count()) once segments[] is populated.
*/
for (which = 0; which < segment_count; which++)
	;

return total;
}

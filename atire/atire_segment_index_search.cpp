/*
	ATIRE_SEGMENT_INDEX_SEARCH.CPP
	------------------------------
	Lexical search, the shared results buffer, and hit-filename resolution.
	Part of ATIRE_segment_index, whose implementation is split across
	atire_segment_index*.cpp by feature (see
	docs/superpowers/specs/2026-07-06-segment-index-file-split-design.md).
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
results[results_count - 1].payload = NULL;
results[results_count - 1].payload_length = 0;
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
	populate_hit_payload(slot);
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
	ATIRE_SEGMENT_INDEX::POPULATE_HIT_PAYLOAD()
	--------------------------------------------
	Fills slot->payload/payload_length from the owning segment (live writer
	buffer or disk segment payload sidecar), looked up by slot->generation +
	slot->docid, mirroring resolve_hit_filename()'s live-vs-disk generation
	dispatch.  A no-op (NULL, 0) when attributes/payloads are not configured
	for this index.
*/
void ATIRE_segment_index::populate_hit_payload(hit *slot)
{
slot->payload = NULL;
slot->payload_length = 0;
if (!attributes_configured())
	return;
if (writer != NULL && slot->generation == writer_generation)
	{
	if (writer_attribute_sets != NULL && slot->docid >= 0 && slot->docid < writer_attribute_sets_capacity && writer_attribute_sets[slot->docid] != NULL)
		{
		long long len = 0;
		const unsigned char *p = writer_attribute_sets[slot->docid]->payload_bytes(&len);
		slot->payload = p;
		slot->payload_length = len;
		}
	return;
	}
long long which;
for (which = 0; which < segment_count; which++)
	if (segments[which].generation == slot->generation)
		{
		if (segments[which].payload != NULL)
			segments[which].payload->get(slot->docid, &slot->payload, &slot->payload_length);
		return;
		}
}

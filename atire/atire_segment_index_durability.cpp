/*
	ATIRE_SEGMENT_INDEX_DURABILITY.CPP
	----------------------------------
	WAL durable-mode setters and the global ranking-statistics push.  Part of
	ATIRE_segment_index, whose implementation is split across
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

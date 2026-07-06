/*
	INDEX_KEYMAP.H
	--------------
	Maps a caller-supplied stable document key (we use the filename passed at
	indexing time) to the (segment generation, local docid) that currently
	holds the live version of that document.

	In memory: open-addressing (linear probing) hash table, growable.
	On disk:   append-only log "<dir>/keymap.log", one record per line:
	               A<TAB>generation<TAB>docid<TAB>key
	               D<TAB>key
	           load() replays the log; later records win.  The log is
	           rebuildable from segment doclists, so it is a cache, not the
	           source of truth.

	Keys must be non-empty and must not contain tab or newline characters
	(they could not round-trip through the log format); such keys are
	rejected: add()/remove() become no-ops and find() reports not-found.

	Only one live ANT_index_keymap per directory: two concurrently-open
	instances do not see each other's writes.
*/
#ifndef INDEX_KEYMAP_H_
#define INDEX_KEYMAP_H_

#include <stdio.h>

class ANT_index_keymap
{
private:
	struct slot
	{
	char *key;					// NULL = never used
	long long generation;
	long long docid;			// docid < 0 = removed (tombstone slot, key retained for probing)
	} ;

private:
	slot *table;
	long long slots_allocated, slots_used;
	FILE *log;					// append handle, NULL while replaying
	char *directory;

private:
	ANT_index_keymap(const char *directory);
	static unsigned long long hash(const char *key);
	static long key_is_valid(const char *key);
	slot *find_slot(const char *key);
	void grow(void);
	void insert_no_log(const char *key, long long generation, long long docid);

public:
	~ANT_index_keymap();

	static ANT_index_keymap *load(const char *directory);

	/*
		ANT_INDEX_KEYMAP::LOG_EXISTS()
		------------------------------
		Check (before calling load(), which creates the log file for
		append if it is missing) whether "<directory>/keymap.log" already
		exists.  Callers use this to decide whether the keymap needs to be
		rebuilt from the segments' own stored filenames -- see
		ATIRE_segment_index::rebuild_keymap().
	*/
	static long log_exists(const char *directory);

	void add(const char *key, long long generation, long long docid);
	void remove(const char *key);
	long find(const char *key, long long *generation, long long *docid);

	/*
		ANT_INDEX_KEYMAP::RETAIN_GENERATIONS()
		---------------------------------------
		Reconcile the keymap against the set of generations that actually
		exist on disk (the manifest).  A memory segment that was added to but
		never flushed before the process exited leaves keymap entries
		pointing at a generation that is not, and never will be, on disk --
		those entries are lies.  Every LIVE entry whose generation is not in
		the given list is marked removed (docid = -1) and a "D\t<key>\n"
		record is appended to the log so the reconciliation survives the
		next reload too.
	*/
	void retain_generations(const long long *generations, long long generation_count);

	/*
		ANT_INDEX_KEYMAP::LOG_DEAD_RATIO()
		----------------------------------
		Proportion of the replayed log that no longer contributes a live entry.
		Dead count is (total replayed - live); live count is the number of
		entries with docid >= 0.  Returns 0 when the log was empty.
	*/
	double log_dead_ratio(void);

	/*
		ANT_INDEX_KEYMAP::COMPACT_LOG()
		-------------------------------
		Write a fresh log holding one A record per live entry (temp + rename),
		then reopen the append handle.  On any failure the old log remains
		fully usable -- merely uncompacted, never lost.  Returns 0 on success.
	*/
	long compact_log(void);

private:
	long long replayed_records;		// count of A/D records successfully replayed in load()
} ;

#endif /* INDEX_KEYMAP_H_ */

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
	void add(const char *key, long long generation, long long docid);
	void remove(const char *key);
	long find(const char *key, long long *generation, long long *docid);
} ;

#endif /* INDEX_KEYMAP_H_ */

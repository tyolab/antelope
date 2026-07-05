# Incremental Index (Phase 1: Segments + Tombstones, Read/Write Path) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add document add/update/delete and incremental index growth to antelope via a segmented index (live in-memory segment + immutable disk segments + tombstones + keymap), per spec `docs/superpowers/specs/2026-07-05-incremental-index-design.md` §Phase 1.

**Architecture:** Low-level, engine-independent components (`ANT_index_tombstones`, `ANT_index_manifest`, `ANT_index_keymap`) live in `source/`. The coordinator `ATIRE_segment_index` lives in `atire/` and composes existing machinery: `ATIRE_indexer` as the live memory segment writer, `ATIRE_API::open_from_memory_index` for NRT search (extended with a non-owning mode), one `ATIRE_API` per disk segment, and post-sort tombstone filtering with over-fetch (`top_k + segment tombstone count`). Compaction (extending `atire_merge`) is Phase 2 — segments only accumulate in this phase.

**Tech Stack:** C++ (codebase is pre-C++11 style: no STL in engine code, `long`/`long long` types, function bodies unindented). Build: GNU make via `GNUmakefile` — any new `source/*.cpp` or `atire/*.cpp` is picked up automatically (`SOURCES` glob), and any `tests/*.cpp` with a `main()` becomes `bin/<basename>` via `make <basename>`.

**Conventions used throughout:**
- Tests are plain `main()` programs: `if (!cond) { printf("FAIL: ...\n"); exit(1); }` … ending `printf("PASSED\n"); return 0;`.
- Tests build/run with: `make <test_name> && ./bin/<test_name>` (from repo root; `make` reads `GNUmakefile` by default).
- Tests create scratch index dirs under `/tmp` with `mkdtemp`.
- Segment file naming: `seg_%06lld.aspt`, `seg_%06lld.doclist`, `seg_%06lld.del`.
- Tests assert *consistency* (e.g. the handle returned by `add_document` equals what `search` reports), not absolute docid values, so 0- vs 1-based docno details in `ATIRE_indexer` can't silently break assertions.

**Out of scope for this plan (later plans):** compacting merge (Phase 2), tiered merge policy (Phase 2), WAL durability mode, cross-segment global ranking stats, thread-safety locks, SWIG/Node.js binding regeneration (needs the Node ≤ 14 build environment).

---

### Task 1: Amend the spec with two design refinements found during code exploration

Code exploration invalidated two spec details; fix the spec before building against it.

**Files:**
- Modify: `docs/superpowers/specs/2026-07-05-incremental-index-design.md` (§2.2 and §2.3)

- [ ] **Step 1: Rewrite §2.3's filtering paragraph**

Replace the paragraph beginning "Query-time filtering happens **during top-k selection**" with:

```markdown
Query-time filtering happens **post-sort with over-fetch**. ATIRE maintains its top-k
inside the ranking functions' heap (`relevance_rank_top_k`), so injecting an exclusion
bitset there would require touching every ranking function. Instead, each segment is
searched for `top_k + that segment's tombstone count` results; tombstoned docids are then
dropped from the sorted list. Since at most `tombstone count` results can be removed, the
surviving list always holds at least `top_k` live documents (or every live match).
Compaction (Phase 2) keeps tombstone counts — and therefore the over-fetch — small.
Deleted documents still contribute to df/IDF until compaction — the standard, accepted
inaccuracy of every segment-based engine.
```

- [ ] **Step 2: Amend §2.2's NRT/ownership paragraph**

Replace the sentence "Single writer; searches and the writer are serialized with a readers-writer lock (writes are short — one document)." with:

```markdown
Single writer. `ANT_search_engine_memory_index` snapshots per-document state (lengths,
accumulator sizing) at `open()`, so the NRT view is a wrapper engine that is rebuilt
lazily: adds mark the segment dirty, and the next search discards and reconstructs the
wrapper (cheap — proportional to the small memory segment). This requires a non-owning
mode on the wrapper so destroying it does not free the shared `ANT_memory_index`.
Phase 1 is single-threaded (matching ATIRE's existing engines and the Node.js binding);
a readers-writer lock is deferred until a multi-threaded consumer exists.
```

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-07-05-incremental-index-design.md
git commit -m "spec: over-fetch tombstone filtering; rebuild-on-dirty NRT view"
```

---

### Task 2: Tombstone bitmap (`ANT_index_tombstones`)

**Files:**
- Create: `source/index_tombstones.h`
- Create: `source/index_tombstones.cpp`
- Test: `tests/test_index_tombstones.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
/*
	TEST_INDEX_TOMBSTONES.CPP
	-------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "index_tombstones.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_tombstones_XXXXXX";
char *dir = mkdtemp(dir_template);
char filename[1024];
CHECK(dir != NULL);
sprintf(filename, "%s/seg_000001.del", dir);

/*
	Fresh bitmap: nothing deleted
*/
ANT_index_tombstones *t = new ANT_index_tombstones(100);
CHECK(t->count() == 0);
CHECK(!t->is_deleted(0));
CHECK(!t->is_deleted(99));

/*
	set / get / count; setting twice counts once
*/
t->set_deleted(7);
t->set_deleted(99);
t->set_deleted(7);
CHECK(t->is_deleted(7));
CHECK(t->is_deleted(99));
CHECK(!t->is_deleted(8));
CHECK(t->count() == 2);

/*
	Grow: setting past the initial size must work (used by the live memory segment)
*/
t->set_deleted(250);
CHECK(t->is_deleted(250));
CHECK(!t->is_deleted(200));
CHECK(t->count() == 3);

/*
	Save / load round trip
*/
CHECK(t->save(filename) == 0);
ANT_index_tombstones *loaded = ANT_index_tombstones::load(filename, 300);
CHECK(loaded != NULL);
CHECK(loaded->is_deleted(7));
CHECK(loaded->is_deleted(99));
CHECK(loaded->is_deleted(250));
CHECK(!loaded->is_deleted(8));
CHECK(loaded->count() == 3);

/*
	Loading a missing file yields an empty bitmap (segment with no deletions)
*/
char missing[1024];
sprintf(missing, "%s/absent.del", dir);
ANT_index_tombstones *empty = ANT_index_tombstones::load(missing, 300);
CHECK(empty != NULL);
CHECK(empty->count() == 0);

/*
	Save must not leave a temp file behind (atomic write-temp + rename)
*/
char tmpname[1200];
sprintf(tmpname, "%s.tmp", filename);
CHECK(access(tmpname, F_OK) != 0);

delete t;
delete loaded;
delete empty;
printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_index_tombstones`
Expected: compile FAILURE — `index_tombstones.h: No such file or directory`

- [ ] **Step 3: Write the implementation**

`source/index_tombstones.h`:

```cpp
/*
	INDEX_TOMBSTONES.H
	------------------
	Per-segment deletion bitmap.  Documents are marked deleted here rather than
	removed from the (immutable) postings; deleted docids are filtered from
	results at query time and physically dropped at compaction (Phase 2).
*/
#ifndef INDEX_TOMBSTONES_H_
#define INDEX_TOMBSTONES_H_

class ANT_index_tombstones
{
private:
	unsigned char *bitmap;
	long long bitmap_bytes;			// allocated size in bytes
	long long deleted_documents;

private:
	void grow_to(long long docid);

public:
	ANT_index_tombstones(long long documents);
	~ANT_index_tombstones();

	void set_deleted(long long docid);
	long is_deleted(long long docid);
	long long count(void) { return deleted_documents; }

	long save(const char *filename);							// 0 on success; write-temp + rename
	static ANT_index_tombstones *load(const char *filename, long long documents);	// empty bitmap if file absent
} ;

#endif /* INDEX_TOMBSTONES_H_ */
```

`source/index_tombstones.cpp`:

```cpp
/*
	INDEX_TOMBSTONES.CPP
	--------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "index_tombstones.h"

/*
	ANT_INDEX_TOMBSTONES::ANT_INDEX_TOMBSTONES()
	--------------------------------------------
*/
ANT_index_tombstones::ANT_index_tombstones(long long documents)
{
bitmap_bytes = (documents / 8) + 1;
bitmap = new unsigned char[bitmap_bytes];
memset(bitmap, 0, (size_t)bitmap_bytes);
deleted_documents = 0;
}

/*
	ANT_INDEX_TOMBSTONES::~ANT_INDEX_TOMBSTONES()
	---------------------------------------------
*/
ANT_index_tombstones::~ANT_index_tombstones()
{
delete [] bitmap;
}

/*
	ANT_INDEX_TOMBSTONES::GROW_TO()
	-------------------------------
*/
void ANT_index_tombstones::grow_to(long long docid)
{
long long needed = (docid / 8) + 1;
if (needed <= bitmap_bytes)
	return;

long long new_bytes = bitmap_bytes * 2 > needed ? bitmap_bytes * 2 : needed;
unsigned char *new_bitmap = new unsigned char[new_bytes];
memcpy(new_bitmap, bitmap, (size_t)bitmap_bytes);
memset(new_bitmap + bitmap_bytes, 0, (size_t)(new_bytes - bitmap_bytes));
delete [] bitmap;
bitmap = new_bitmap;
bitmap_bytes = new_bytes;
}

/*
	ANT_INDEX_TOMBSTONES::SET_DELETED()
	-----------------------------------
*/
void ANT_index_tombstones::set_deleted(long long docid)
{
grow_to(docid);
if (!(bitmap[docid / 8] & (1 << (docid % 8))))
	{
	bitmap[docid / 8] |= (unsigned char)(1 << (docid % 8));
	deleted_documents++;
	}
}

/*
	ANT_INDEX_TOMBSTONES::IS_DELETED()
	----------------------------------
*/
long ANT_index_tombstones::is_deleted(long long docid)
{
if (docid / 8 >= bitmap_bytes)
	return 0;
return (bitmap[docid / 8] & (1 << (docid % 8))) != 0;
}

/*
	ANT_INDEX_TOMBSTONES::SAVE()
	----------------------------
	Write-temp then rename so a crash can never leave a torn bitmap.
*/
long ANT_index_tombstones::save(const char *filename)
{
char temp_name[4096];
FILE *fp;

if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
if (fwrite(&deleted_documents, sizeof(deleted_documents), 1, fp) != 1
	|| fwrite(&bitmap_bytes, sizeof(bitmap_bytes), 1, fp) != 1
	|| fwrite(bitmap, 1, (size_t)bitmap_bytes, fp) != (size_t)bitmap_bytes)
	{
	fclose(fp);
	remove(temp_name);
	return 1;
	}
fclose(fp);
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}

/*
	ANT_INDEX_TOMBSTONES::LOAD()
	----------------------------
*/
ANT_index_tombstones *ANT_index_tombstones::load(const char *filename, long long documents)
{
FILE *fp;
ANT_index_tombstones *result = new ANT_index_tombstones(documents);

if ((fp = fopen(filename, "rb")) == NULL)
	return result;			// no .del file means no deletions

long long stored_count, stored_bytes;
if (fread(&stored_count, sizeof(stored_count), 1, fp) == 1
	&& fread(&stored_bytes, sizeof(stored_bytes), 1, fp) == 1)
	{
	if (stored_count < 0 || stored_bytes <= 0 || stored_bytes > (1LL << 40))
		{
		fclose(fp);
		return result;		// corrupt header: treat as a segment with no deletions
		}
	result->grow_to(stored_bytes * 8 - 1);
	if (fread(result->bitmap, 1, (size_t)stored_bytes, fp) == (size_t)stored_bytes)
		result->deleted_documents = stored_count;
	}
fclose(fp);
return result;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_index_tombstones && ./bin/test_index_tombstones`
Expected: `PASSED`
(Note: the first `make` after adding a file to `source/` recompiles nothing else — only the new objects — but the very first build of the repo compiles everything and can take minutes. That is normal.)

- [ ] **Step 5: Commit**

```bash
git add source/index_tombstones.h source/index_tombstones.cpp tests/test_index_tombstones.cpp
git commit -m "feat: per-segment tombstone bitmap with atomic save/load"
```

---

### Task 3: Segment manifest (`ANT_index_manifest`)

**Files:**
- Create: `source/index_manifest.h`
- Create: `source/index_manifest.cpp`
- Test: `tests/test_index_manifest.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
/*
	TEST_INDEX_MANIFEST.CPP
	-----------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "index_manifest.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_manifest_XXXXXX";
char *dir = mkdtemp(dir_template);
CHECK(dir != NULL);

/*
	Loading from an empty directory yields a new, empty manifest at generation 1
	(generation is the *next* segment number to hand out)
*/
ANT_index_manifest *m = ANT_index_manifest::load(dir);
CHECK(m != NULL);
CHECK(m->segment_count() == 0);
CHECK(m->get_generation() == 1);

/*
	Take generations, add segments, save
*/
long long g1 = m->take_generation();
long long g2 = m->take_generation();
CHECK(g1 == 1);
CHECK(g2 == 2);
m->add_segment(g1);
m->add_segment(g2);
CHECK(m->segment_count() == 2);
CHECK(m->get_segment(0) == g1);
CHECK(m->get_segment(1) == g2);
CHECK(m->save() == 0);

/*
	Reload: same contents, generation continues from where we left off
*/
ANT_index_manifest *m2 = ANT_index_manifest::load(dir);
CHECK(m2->segment_count() == 2);
CHECK(m2->get_segment(0) == g1);
CHECK(m2->get_segment(1) == g2);
CHECK(m2->take_generation() == 3);

/*
	contains(): used for orphan cleanup
*/
CHECK(m2->contains(g1));
CHECK(!m2->contains(99));

/*
	Atomic save: no temp file left behind
*/
char tmpname[1200];
sprintf(tmpname, "%s/manifest.tmp", dir);
CHECK(access(tmpname, F_OK) != 0);

delete m;
delete m2;
printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_index_manifest`
Expected: compile FAILURE — `index_manifest.h: No such file or directory`

- [ ] **Step 3: Write the implementation**

`source/index_manifest.h`:

```cpp
/*
	INDEX_MANIFEST.H
	----------------
	The manifest is the authoritative list of live disk segments in an index
	directory, plus the next segment generation number.  It is always updated
	by write-temp + rename so readers see either the old or the new state,
	never a torn file.

	On-disk format (text, "<dir>/manifest"):
		line 1:  next generation number
		line 2+: one live segment generation number per line
*/
#ifndef INDEX_MANIFEST_H_
#define INDEX_MANIFEST_H_

class ANT_index_manifest
{
private:
	char *directory;
	long long *segments;
	long long segments_used, segments_allocated;
	long long generation;			// next generation to hand out

private:
	ANT_index_manifest(const char *directory);

public:
	~ANT_index_manifest();

	static ANT_index_manifest *load(const char *directory);	// missing file -> fresh manifest, generation 1
	long save(void);											// 0 on success

	long long take_generation(void) { return generation++; }
	long long get_generation(void) { return generation; }

	void add_segment(long long segment_generation);
	long long segment_count(void) { return segments_used; }
	long long get_segment(long long which) { return segments[which]; }
	long contains(long long segment_generation);
} ;

#endif /* INDEX_MANIFEST_H_ */
```

`source/index_manifest.cpp`:

```cpp
/*
	INDEX_MANIFEST.CPP
	------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "index_manifest.h"

/*
	ANT_INDEX_MANIFEST::ANT_INDEX_MANIFEST()
	----------------------------------------
*/
ANT_index_manifest::ANT_index_manifest(const char *directory)
{
this->directory = new char[strlen(directory) + 1];
strcpy(this->directory, directory);
segments_allocated = 8;
segments = new long long[segments_allocated];
segments_used = 0;
generation = 1;
}

/*
	ANT_INDEX_MANIFEST::~ANT_INDEX_MANIFEST()
	-----------------------------------------
*/
ANT_index_manifest::~ANT_index_manifest()
{
delete [] directory;
delete [] segments;
}

/*
	ANT_INDEX_MANIFEST::LOAD()
	--------------------------
*/
ANT_index_manifest *ANT_index_manifest::load(const char *directory)
{
char filename[4096];
FILE *fp;
ANT_index_manifest *result = new ANT_index_manifest(directory);

sprintf(filename, "%s/manifest", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return result;			// fresh index directory

char line[64];
if (fgets(line, sizeof(line), fp) != NULL)
	result->generation = atoll(line);
while (fgets(line, sizeof(line), fp) != NULL)
	if (atoll(line) > 0)
		result->add_segment(atoll(line));
fclose(fp);
return result;
}

/*
	ANT_INDEX_MANIFEST::SAVE()
	--------------------------
*/
long ANT_index_manifest::save(void)
{
char filename[4096], temp_name[4200];
FILE *fp;
long long which;

sprintf(filename, "%s/manifest", directory);
sprintf(temp_name, "%s.tmp", filename);
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
fprintf(fp, "%lld\n", generation);
for (which = 0; which < segments_used; which++)
	fprintf(fp, "%lld\n", segments[which]);
fclose(fp);
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}

/*
	ANT_INDEX_MANIFEST::ADD_SEGMENT()
	---------------------------------
*/
void ANT_index_manifest::add_segment(long long segment_generation)
{
if (segments_used >= segments_allocated)
	{
	long long *bigger = new long long[segments_allocated * 2];
	memcpy(bigger, segments, (size_t)(segments_used * sizeof(*segments)));
	delete [] segments;
	segments = bigger;
	segments_allocated *= 2;
	}
segments[segments_used++] = segment_generation;
}

/*
	ANT_INDEX_MANIFEST::CONTAINS()
	------------------------------
*/
long ANT_index_manifest::contains(long long segment_generation)
{
long long which;

for (which = 0; which < segments_used; which++)
	if (segments[which] == segment_generation)
		return 1;
return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_index_manifest && ./bin/test_index_manifest`
Expected: `PASSED`

- [ ] **Step 5: Commit**

```bash
git add source/index_manifest.h source/index_manifest.cpp tests/test_index_manifest.cpp
git commit -m "feat: segment manifest with atomic save and generation counter"
```

---

### Task 4: External-key map (`ANT_index_keymap`)

**Files:**
- Create: `source/index_keymap.h`
- Create: `source/index_keymap.cpp`
- Test: `tests/test_index_keymap.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
/*
	TEST_INDEX_KEYMAP.CPP
	---------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "index_keymap.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *dir = mkdtemp(dir_template);
CHECK(dir != NULL);
long long generation, docid;

/*
	Empty map
*/
ANT_index_keymap *map = ANT_index_keymap::load(dir);
CHECK(map != NULL);
CHECK(!map->find("doc-1", &generation, &docid));

/*
	add / find / overwrite (newest wins) / remove
*/
map->add("doc-1", 1, 0);
map->add("doc-2", 1, 1);
CHECK(map->find("doc-1", &generation, &docid) && generation == 1 && docid == 0);
CHECK(map->find("doc-2", &generation, &docid) && generation == 1 && docid == 1);

map->add("doc-1", 2, 5);		// updated version lives in segment 2
CHECK(map->find("doc-1", &generation, &docid) && generation == 2 && docid == 5);

map->remove("doc-2");
CHECK(!map->find("doc-2", &generation, &docid));

/*
	Growth: many keys must survive table resize
*/
char key[64];
long long i;
for (i = 0; i < 5000; i++)
	{
	sprintf(key, "bulk-%lld", i);
	map->add(key, 3, i);
	}
for (i = 0; i < 5000; i++)
	{
	sprintf(key, "bulk-%lld", i);
	CHECK(map->find(key, &generation, &docid) && generation == 3 && docid == i);
	}
CHECK(map->find("doc-1", &generation, &docid) && generation == 2 && docid == 5);

/*
	Persistence: the append-only log replays to the same state
*/
delete map;
ANT_index_keymap *reloaded = ANT_index_keymap::load(dir);
CHECK(reloaded->find("doc-1", &generation, &docid) && generation == 2 && docid == 5);
CHECK(!reloaded->find("doc-2", &generation, &docid));
sprintf(key, "bulk-%lld", (long long)4999);
CHECK(reloaded->find(key, &generation, &docid) && generation == 3 && docid == 4999);

delete reloaded;
printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_index_keymap`
Expected: compile FAILURE — `index_keymap.h: No such file or directory`

- [ ] **Step 3: Write the implementation**

`source/index_keymap.h`:

```cpp
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
	           rebuildable from segment doclists (Task 12), so it is a cache,
	           not the source of truth.
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
```

`source/index_keymap.cpp`:

```cpp
/*
	INDEX_KEYMAP.CPP
	----------------
*/
#include <stdlib.h>
#include <string.h>
#include "index_keymap.h"

/*
	ANT_INDEX_KEYMAP::ANT_INDEX_KEYMAP()
	------------------------------------
*/
ANT_index_keymap::ANT_index_keymap(const char *directory)
{
this->directory = new char[strlen(directory) + 1];
strcpy(this->directory, directory);
slots_allocated = 1024;
slots_used = 0;
table = new slot[slots_allocated];
memset(table, 0, (size_t)(slots_allocated * sizeof(*table)));
log = NULL;
}

/*
	ANT_INDEX_KEYMAP::~ANT_INDEX_KEYMAP()
	-------------------------------------
*/
ANT_index_keymap::~ANT_index_keymap()
{
long long which;

if (log != NULL)
	fclose(log);
for (which = 0; which < slots_allocated; which++)
	delete [] table[which].key;
delete [] table;
delete [] directory;
}

/*
	ANT_INDEX_KEYMAP::HASH()
	------------------------
	FNV-1a
*/
unsigned long long ANT_index_keymap::hash(const char *key)
{
unsigned long long h = 14695981039346656037ULL;

while (*key != '\0')
	{
	h ^= (unsigned char)*key++;
	h *= 1099511628211ULL;
	}
return h;
}

/*
	ANT_INDEX_KEYMAP::FIND_SLOT()
	-----------------------------
	Returns the slot holding key, or the first never-used slot on its probe path.
*/
ANT_index_keymap::slot *ANT_index_keymap::find_slot(const char *key)
{
unsigned long long where = hash(key) % slots_allocated;

while (table[where].key != NULL)
	{
	if (strcmp(table[where].key, key) == 0)
		return &table[where];
	where = (where + 1) % slots_allocated;
	}
return &table[where];
}

/*
	ANT_INDEX_KEYMAP::GROW()
	------------------------
*/
void ANT_index_keymap::grow(void)
{
slot *old_table = table;
long long old_allocated = slots_allocated;
long long which;

slots_allocated *= 2;
table = new slot[slots_allocated];
memset(table, 0, (size_t)(slots_allocated * sizeof(*table)));
slots_used = 0;
for (which = 0; which < old_allocated; which++)
	if (old_table[which].key != NULL)
		{
		if (old_table[which].docid >= 0)
			insert_no_log(old_table[which].key, old_table[which].generation, old_table[which].docid);
		delete [] old_table[which].key;
		}
delete [] old_table;
}

/*
	ANT_INDEX_KEYMAP::INSERT_NO_LOG()
	---------------------------------
*/
void ANT_index_keymap::insert_no_log(const char *key, long long generation, long long docid)
{
if (slots_used * 4 >= slots_allocated * 3)		// resize at 75% load
	grow();

slot *where = find_slot(key);
if (where->key == NULL)
	{
	where->key = new char[strlen(key) + 1];
	strcpy(where->key, key);
	slots_used++;
	}
where->generation = generation;
where->docid = docid;
}

/*
	ANT_INDEX_KEYMAP::LOAD()
	------------------------
*/
ANT_index_keymap *ANT_index_keymap::load(const char *directory)
{
char filename[4096];
char line[8192];
FILE *fp;
ANT_index_keymap *result = new ANT_index_keymap(directory);

sprintf(filename, "%s/keymap.log", directory);
if ((fp = fopen(filename, "rb")) != NULL)
	{
	while (fgets(line, sizeof(line), fp) != NULL)
		{
		char *newline = strchr(line, '\n');
		if (newline != NULL)
			*newline = '\0';
		if (line[0] == 'A')
			{
			long long generation = atoll(line + 2);
			char *tab = strchr(line + 2, '\t');
			if (tab == NULL)
				continue;
			long long docid = atoll(tab + 1);
			char *key = strchr(tab + 1, '\t');
			if (key == NULL)
				continue;
			result->insert_no_log(key + 1, generation, docid);
			}
		else if (line[0] == 'D')
			{
			slot *where = result->find_slot(line + 2);
			if (where->key != NULL)
				where->docid = -1;
			}
		}
	fclose(fp);
	}
result->log = fopen(filename, "ab");
return result;
}

/*
	ANT_INDEX_KEYMAP::ADD()
	-----------------------
*/
void ANT_index_keymap::add(const char *key, long long generation, long long docid)
{
insert_no_log(key, generation, docid);
if (log != NULL)
	{
	fprintf(log, "A\t%lld\t%lld\t%s\n", generation, docid, key);
	fflush(log);
	}
}

/*
	ANT_INDEX_KEYMAP::REMOVE()
	--------------------------
*/
void ANT_index_keymap::remove(const char *key)
{
slot *where = find_slot(key);
if (where->key != NULL)
	where->docid = -1;
if (log != NULL)
	{
	fprintf(log, "D\t%s\n", key);
	fflush(log);
	}
}

/*
	ANT_INDEX_KEYMAP::FIND()
	------------------------
*/
long ANT_index_keymap::find(const char *key, long long *generation, long long *docid)
{
slot *where = find_slot(key);
if (where->key == NULL || where->docid < 0)
	return 0;
*generation = where->generation;
*docid = where->docid;
return 1;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_index_keymap && ./bin/test_index_keymap`
Expected: `PASSED`

- [ ] **Step 5: Commit**

```bash
git add source/index_keymap.h source/index_keymap.cpp tests/test_index_keymap.cpp
git commit -m "feat: external key to (segment, docid) map with append-only log"
```

---

### Task 5: Core engine hooks — results-list accessor and non-owning memory-index engine

Two tiny, surgical changes to existing engine classes that the coordinator needs.

**Files:**
- Modify: `source/search_engine.h` (add one public accessor near `get_variable`, around line 145–160)
- Modify: `source/search_engine_memory_index.h` (ownership flag)
- Modify: `source/search_engine_memory_index.cpp` (destructor honors the flag)
- Modify: `atire/atire_api.h` and `atire/atire_api.cpp` (`open_from_memory_index` gains a `take_ownership` parameter, default unchanged)
- Test: `tests/test_memory_engine_ownership.cpp`

- [ ] **Step 1: Write the failing test**

The test proves: (a) a non-owning engine can be destroyed and the underlying `ANT_memory_index` reused by a second engine — this is the rebuild-on-dirty pattern; (b) documents added *after* the first wrapper was built are visible through the second wrapper.

```cpp
/*
	TEST_MEMORY_ENGINE_OWNERSHIP.CPP
	--------------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../atire/indexer.h"
#include "../atire/atire_api.h"
#include "memory_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
long long count;
char query[64];

ATIRE_indexer *indexer = new ATIRE_indexer();
indexer->init((char *)"atire_test -nologo");
indexer->index_document((char *)"doc-1", (char *)"<DOC>aardvark zebra</DOC>");
indexer->index_document((char *)"doc-2", (char *)"<DOC>zebra quokka</DOC>");

/*
	First wrapper: non-owning.  get_index(), not release_index() -- the
	indexer keeps its pointer and can continue indexing afterwards.
*/
ATIRE_API *engine_one = new ATIRE_API();
CHECK(engine_one->open_from_memory_index(indexer->get_index(), indexer->get_doc_list(&count), count, /*take_ownership =*/ 0) == 0);
strcpy(query, "aardvark");
CHECK(engine_one->search(query, 10) == 1);
delete engine_one;			// must NOT free the memory index

/*
	Keep indexing into the same index, then wrap again: both old and new
	documents must be searchable.
*/
indexer->index_document((char *)"doc-3", (char *)"<DOC>aardvark wombat</DOC>");
ATIRE_API *engine_two = new ATIRE_API();
CHECK(engine_two->open_from_memory_index(indexer->get_index(), indexer->get_doc_list(&count), count, 0) == 0);
strcpy(query, "aardvark");
CHECK(engine_two->search(query, 10) == 2);
strcpy(query, "wombat");
CHECK(engine_two->search(query, 10) == 1);
delete engine_two;

delete indexer;
printf("PASSED\n");
return 0;
}
```

Notes for the implementer:
- `ATIRE_indexer::init(char *options)` tokenizes an argv-style string; the first token is the program name. `-nologo` suppresses the banner. If `init` requires an index filename to be set even when never flushing, add `-findex /tmp/unused.aspt -fdoclist /tmp/unused_doclist.aspt` to the option string in this test.
- `ATIRE_API::search` returns the hit count and may mutate the query buffer — hence the writable `query[]` array.
- The document markup (`<DOC>…</DOC>`) matches what the TREC parser expects; `indexer.cpp`'s existing `index_document(char *file_name, char *file)` path (used by the Node.js binding) defines the exact expectations — mirror whatever that path needs.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_memory_engine_ownership`
Expected: compile FAILURE — `open_from_memory_index` takes 3 arguments, not 4.

- [ ] **Step 3: Implement the hooks**

In `source/search_engine.h`, in the `public:` section of `ANT_search_engine` (near `get_variable`), add:

```cpp
	ANT_search_engine_result *get_results_list(void) { return results_list; }
```

(`results_list` is a protected/private member; this read accessor is needed by Task 8 to pull docids and RSV scores out of a finished search without re-running the sort.)

In `source/search_engine_memory_index.h`, add to the class:

```cpp
protected:
	long owns_index;			// if false, the destructor leaves the shared ANT_memory_index alone

public:
	void set_index_ownership(long owns) { owns_index = owns; }
```

In `source/search_engine_memory_index.cpp`:
- Constructor: add `owns_index = 1;` after `postings_buffer_length = 0;`
- Destructor: replace the body with

```cpp
delete memory;
if (owns_index)
	delete index;
```

(The `ANT_memory` arena is created by `ATIRE_API::open_from_memory_index` per wrapper and holds only wrapper-lifetime allocations — `results_list`, decompress buffers — so the wrapper must free it *unconditionally*, otherwise every NRT rebuild in Task 6 leaks an arena. The `ANT_memory_index` has its own internal arenas and shares nothing with this one, so sparing it is safe. Only the index pointer is ownership-guarded.)

In `atire/atire_api.h`, change the declaration:

```cpp
	long open_from_memory_index(ANT_memory_index *index, char **doc_list, long long doc_count, long take_ownership = 1);
```

In `atire/atire_api.cpp`, `open_from_memory_index`: add the parameter, and after the `ANT_search_engine_memory_index` is constructed, add:

```cpp
if (!take_ownership)
	((ANT_search_engine_memory_index *)search_engine)->set_index_ownership(0);
```

Also inspect the existing body: if it frees or NULLs anything else it shouldn't when `take_ownership` is 0 (e.g. taking ownership of `doc_list`), guard that the same way. Existing callers pass no fourth argument and keep today's owning behavior.

**Also check** `~ATIRE_API` (in `atire_api.cpp`): with `take_ownership = 0` the wrapper engine must still be deleted by `~ATIRE_API` (it is — the engine is owned by the API object); only the *index inside it* is spared, which `owns_index = 0` handles.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_memory_engine_ownership && ./bin/test_memory_engine_ownership`
Expected: `PASSED`

Then confirm no regression in the owning path:
Run: `make atire index atire_merge`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add source/search_engine.h source/search_engine_memory_index.h source/search_engine_memory_index.cpp atire/atire_api.h atire/atire_api.cpp tests/test_memory_engine_ownership.cpp
git commit -m "feat: non-owning memory-index engine mode + results_list accessor"
```

---

### Task 6: `ATIRE_segment_index` skeleton — open, add, NRT search on the memory segment

**Files:**
- Create: `atire/atire_segment_index.h`
- Create: `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp` (grows over Tasks 6–12)

- [ ] **Step 1: Write the failing test**

```cpp
/*
	TEST_SEGMENT_INDEX.CPP
	----------------------
	End-to-end tests for the segmented incremental index.  Each task in the
	implementation plan appends a test function here; main() calls them in order.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(void)
{
static char dir_template[] = "/tmp/ant_segidx_XXXXXX";
char buffer[64];
strcpy(buffer, "/tmp/ant_segidx_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
(void)dir_template;
return result;
}

/*
	TEST_NRT_ADD_AND_SEARCH()
	-------------------------
	Documents are searchable immediately after add_document, before any flush.
*/
static void test_nrt_add_and_search(void)
{
char *dir = make_index_dir();
char query[64];
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

long long h1 = index->add_document("doc-1", "<DOC>aardvark zebra</DOC>");
long long h2 = index->add_document("doc-2", "<DOC>zebra quokka</DOC>");
CHECK(h1 >= 0);
CHECK(h2 >= 0);
CHECK(h1 != h2);

strcpy(query, "zebra");
long long hits = index->search(query, 10);
CHECK(hits == 2);

strcpy(query, "aardvark");
hits = index->search(query, 10);
CHECK(hits == 1);
ATIRE_segment_index::hit *hit = index->get_hit(0);
CHECK(strcmp(hit->filename, "doc-1") == 0);
CHECK(ATIRE_segment_index::make_handle(hit->generation, hit->docid) == h1);

/*
	A doc added after a search is visible to the next search (dirty rebuild)
*/
index->add_document("doc-3", "<DOC>aardvark wombat</DOC>");
strcpy(query, "aardvark");
CHECK(index->search(query, 10) == 2);

delete index;
delete [] dir;
printf("test_nrt_add_and_search OK\n");
}

int main(void)
{
test_nrt_add_and_search();
printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_segment_index`
Expected: compile FAILURE — `atire_segment_index.h: No such file or directory`

- [ ] **Step 3: Write the header**

`atire/atire_segment_index.h`:

```cpp
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

	long long flush_after_documents;	// 0 = only flush manually

	hit *results;
	long long results_count, results_allocated;

private:
	void start_new_writer(void);
	void rebuild_writer_engine(void);
	void search_one_segment(ATIRE_API *engine, ANT_index_tombstones *tombstones, long long generation, char *query, long long top_k);
	long append_segment(long long generation);
	void segment_filename(char *buffer, long long generation, const char *extension);

public:
	ATIRE_segment_index();
	~ATIRE_segment_index();

	long open(const char *directory);						// 0 on success

	long long add_document(const char *key, const char *document);		// returns handle, -1 on error
	long long update_document(const char *key, const char *document);	// upsert; returns new handle
	long delete_document(const char *key);								// 0 on success, 1 if key unknown

	long flush(void);										// memory segment -> disk segment; 0 on success
	void set_flush_threshold(long long documents) { flush_after_documents = documents; }

	long long search(char *query, long long top_k);			// returns number of hits stored
	hit *get_hit(long long which) { return &results[which]; }

	long long get_document_count(void);						// live (non-tombstoned) documents

	static long long make_handle(long long generation, long long docid) { return (generation << 40) | docid; }
} ;

#endif /* ATIRE_SEGMENT_INDEX_H_ */
```

- [ ] **Step 4: Write the implementation (this task: open on empty dir, add, NRT search on memory segment only)**

`atire/atire_segment_index.cpp`:

```cpp
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
#include "index_manifest.h"
#include "index_keymap.h"
#include "index_tombstones.h"
#include "search_engine.h"
#include "search_engine_result.h"
#include "search_engine_accumulator.h"

/*
	ATIRE_SEGMENT_INDEX::ATIRE_SEGMENT_INDEX()
	------------------------------------------
*/
ATIRE_segment_index::ATIRE_segment_index()
{
directory = NULL;
manifest = NULL;
keymap = NULL;
segments = NULL;
segment_count = segments_allocated = 0;
writer = NULL;
writer_generation = 0;
writer_documents = 0;
writer_tombstones = NULL;
writer_engine = NULL;
writer_engine_stale = 1;
flush_after_documents = 0;
results = NULL;
results_count = results_allocated = 0;
}

/*
	ATIRE_SEGMENT_INDEX::~ATIRE_SEGMENT_INDEX()
	-------------------------------------------
*/
ATIRE_segment_index::~ATIRE_segment_index()
{
long long which;

delete writer_engine;		// non-owning: does not free the writer's memory index
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
delete [] results;
}

/*
	ATIRE_SEGMENT_INDEX::SEGMENT_FILENAME()
	---------------------------------------
*/
void ATIRE_segment_index::segment_filename(char *buffer, long long generation, const char *extension)
{
sprintf(buffer, "%s/seg_%06lld.%s", directory, generation, extension);
}

/*
	ATIRE_SEGMENT_INDEX::START_NEW_WRITER()
	---------------------------------------
	Create a fresh in-memory writing segment.  Its generation (and therefore
	its eventual on-disk filenames) is taken now so keymap entries written
	while it is in memory remain valid after flush.
*/
void ATIRE_segment_index::start_new_writer(void)
{
char index_name[4096], doclist_name[4096], options[16384];

writer_generation = manifest->take_generation();
segment_filename(index_name, writer_generation, "aspt");
segment_filename(doclist_name, writer_generation, "doclist");
sprintf(options, "atire_segment_writer -nologo -findex %s -fdoclist %s", index_name, doclist_name);

writer = new ATIRE_indexer();
writer->init(options);
writer_documents = 0;
writer_tombstones = new ANT_index_tombstones(1024);
writer_engine = NULL;
writer_engine_stale = 1;
manifest->save();				// persist the bumped generation counter
}

/*
	ATIRE_SEGMENT_INDEX::OPEN()
	---------------------------
*/
long ATIRE_segment_index::open(const char *directory)
{
this->directory = new char[strlen(directory) + 1];
strcpy(this->directory, directory);

manifest = ANT_index_manifest::load(directory);
keymap = ANT_index_keymap::load(directory);

/*
	Disk segments are opened in Task 7 (none exist until flush works).
*/
start_new_writer();
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::ADD_DOCUMENT()
	-----------------------------------
*/
long long ATIRE_segment_index::add_document(const char *key, const char *document)
{
char *key_copy = new char[strlen(key) + 1];
char *doc_copy = new char[strlen(document) + 1];

strcpy(key_copy, key);
strcpy(doc_copy, document);
writer->index_document(key_copy, doc_copy);
delete [] doc_copy;

long long docid = writer->get_docno();
delete [] key_copy;
writer_documents++;
writer_engine_stale = 1;
keymap->add(key, writer_generation, docid);
return make_handle(writer_generation, docid);
}

/*
	ATIRE_SEGMENT_INDEX::REBUILD_WRITER_ENGINE()
	--------------------------------------------
	The NRT view: wrap the writer's live ANT_memory_index in a search engine.
	ANT_search_engine_memory_index snapshots document counts at open(), so the
	wrapper is discarded and rebuilt after any add since the last search.
*/
void ATIRE_segment_index::rebuild_writer_engine(void)
{
long long count;

if (!writer_engine_stale && writer_engine != NULL)
	return;

delete writer_engine;
writer_engine = NULL;
if (writer_documents == 0)
	{
	writer_engine_stale = 0;
	return;
	}

writer_engine = new ATIRE_API();
writer_engine->open_from_memory_index(writer->get_index(), writer->get_doc_list(&count), count, /*take_ownership =*/ 0);
writer_engine_stale = 0;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_ONE_SEGMENT()
	-----------------------------------------
	Search a single segment for top_k + tombstone-count results (over-fetch),
	drop tombstoned docids, and append survivors to this->results.
*/
void ATIRE_segment_index::search_one_segment(ATIRE_API *engine, ANT_index_tombstones *tombstones, long long generation, char *query, long long top_k)
{
long long hits, which, fetch;
char query_copy[16384];

fetch = top_k + (tombstones == NULL ? 0 : tombstones->count());
strncpy(query_copy, query, sizeof(query_copy) - 1);
query_copy[sizeof(query_copy) - 1] = '\0';

hits = engine->search(query_copy, fetch);

ANT_search_engine *se = engine->get_search_engine();
ANT_search_engine_result *list = se->get_results_list();

for (which = 0; which < hits && which < fetch; which++)
	{
	ANT_search_engine_accumulator *accumulator = list->accumulator_pointers[which];
	long long docid = accumulator - list->accumulator;
	if (tombstones != NULL && tombstones->is_deleted(docid))
		continue;
	if (results_count >= results_allocated)
		{
		long long bigger_size = results_allocated == 0 ? 256 : results_allocated * 2;
		hit *bigger = new hit[bigger_size];
		memcpy(bigger, results, (size_t)(results_count * sizeof(*results)));
		delete [] results;
		results = bigger;
		results_allocated = bigger_size;
		}
	results[results_count].generation = generation;
	results[results_count].docid = docid;
	results[results_count].score = (double)accumulator->get_rsv();
	results[results_count].filename = engine->get_document_filename_from_doclist(docid);
	results_count++;
	}
}

/*
	HIT_SCORE_COMPARE()
	-------------------
	qsort comparator: descending score, ties broken by (generation, docid) for determinism.
*/
static int hit_score_compare(const void *a, const void *b)
{
const ATIRE_segment_index::hit *one = (const ATIRE_segment_index::hit *)a;
const ATIRE_segment_index::hit *two = (const ATIRE_segment_index::hit *)b;

if (one->score > two->score)
	return -1;
if (one->score < two->score)
	return 1;
if (one->generation != two->generation)
	return one->generation < two->generation ? -1 : 1;
return one->docid < two->docid ? -1 : (one->docid == two->docid ? 0 : 1);
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH()
	-----------------------------
*/
long long ATIRE_segment_index::search(char *query, long long top_k)
{
long long which;

results_count = 0;

for (which = 0; which < segment_count; which++)
	search_one_segment(segments[which].engine, segments[which].tombstones, segments[which].generation, query, top_k);

rebuild_writer_engine();
if (writer_engine != NULL)
	search_one_segment(writer_engine, writer_tombstones, writer_generation, query, top_k);

qsort(results, (size_t)results_count, sizeof(*results), hit_score_compare);
if (results_count > top_k)
	results_count = top_k;
return results_count;
}

/*
	ATIRE_SEGMENT_INDEX::GET_DOCUMENT_COUNT()
	-----------------------------------------
*/
long long ATIRE_segment_index::get_document_count(void)
{
long long which, total = writer_documents - writer_tombstones->count();

for (which = 0; which < segment_count; which++)
	total += segments[which].engine->get_document_count() - segments[which].tombstones->count();
return total;
}

/*
	Stubs completed in later tasks
*/
long ATIRE_segment_index::append_segment(long long generation) { (void)generation; return 1; }		// Task 7
long ATIRE_segment_index::flush(void) { return 1; }													// Task 7
long long ATIRE_segment_index::update_document(const char *key, const char *document) { (void)key; (void)document; return -1; }	// Task 9
long ATIRE_segment_index::delete_document(const char *key) { (void)key; return 1; }					// Task 9
```

Implementer notes for this task:
- `ATIRE_indexer::index_document(char *file_name, char *file)` may modify the buffers it is given (the parser writes into the document buffer) — hence the copies.
- If `get_docno()` turns out to be the *count* rather than the last-assigned id (check `indexer.cpp`: `docno` starts at -1), adjust `add_document` to record the id the doclist actually associates with the document. The test's consistency assertion (`make_handle(hit->generation, hit->docid) == h1`) is what must pass — it compares the handle from `add_document` against the docid that comes back out of a search of the same index, so any off-by-one is caught immediately.
- `ANT_search_engine_result::accumulator` and `accumulator_pointers` are public (marked "remove this line later" in `search_engine_result.h:33` — they have been public for a decade; rely on them).
- `get_rsv()` is on `ANT_search_engine_accumulator` (`source/search_engine_accumulator.h`).
- `get_document_filename_from_doclist(docid)` works for the memory segment because `open_from_memory_index` uses the in-memory doclist (`ANT_V3` mode, established in commit d27e624). Task 7 makes it work for disk segments by passing the doclist filename at open.

- [ ] **Step 5: Run test to verify it passes**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: `test_nrt_add_and_search OK` then `PASSED`

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat: ATIRE_segment_index skeleton with NRT add + search on memory segment"
```

---

### Task 7: Flush the memory segment to disk; reopen an existing index

**Files:**
- Modify: `atire/atire_segment_index.cpp` (implement `flush`, `append_segment`; extend `open`)
- Test: `tests/test_segment_index.cpp` (append `test_flush_and_reopen`)

- [ ] **Step 1: Write the failing test — append to `tests/test_segment_index.cpp` and call from `main()`**

```cpp
/*
	TEST_FLUSH_AND_REOPEN()
	-----------------------
*/
static void test_flush_and_reopen(void)
{
char *dir = make_index_dir();
char query[64];

/*
	Build, flush, keep searching in the same session
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
long long h1 = index->add_document("doc-1", "<DOC>aardvark zebra</DOC>");
index->add_document("doc-2", "<DOC>zebra quokka</DOC>");
CHECK(index->flush() == 0);

/*
	After flush: same results, now served from the disk segment
*/
strcpy(query, "zebra");
CHECK(index->search(query, 10) == 2);

/*
	Keep writing into the fresh memory segment; search spans disk + memory
*/
index->add_document("doc-3", "<DOC>zebra wombat</DOC>");
strcpy(query, "zebra");
CHECK(index->search(query, 10) == 3);
strcpy(query, "aardvark");
CHECK(index->search(query, 10) == 1);
CHECK(ATIRE_segment_index::make_handle(index->get_hit(0)->generation, index->get_hit(0)->docid) == h1);
delete index;			// doc-3 not flushed: relaxed durability, lost on close without flush

/*
	Reopen from disk: the flushed segment is there, the unflushed doc is gone
*/
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
strcpy(query, "zebra");
CHECK(reopened->search(query, 10) == 2);
strcpy(query, "wombat");
CHECK(reopened->search(query, 10) == 0);
CHECK(reopened->get_document_count() == 2);
delete reopened;
delete [] dir;
printf("test_flush_and_reopen OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: FAIL at `CHECK(index->flush() == 0)` (stub returns 1).

- [ ] **Step 3: Implement flush / append_segment / reopen**

Replace the Task-7 stubs in `atire/atire_segment_index.cpp`:

```cpp
/*
	ATIRE_SEGMENT_INDEX::APPEND_SEGMENT()
	-------------------------------------
	Open an on-disk segment for searching and add it to the segment list.
*/
long ATIRE_segment_index::append_segment(long long generation)
{
char index_name[4096], doclist_name[4096], del_name[4096];

segment_filename(index_name, generation, "aspt");
segment_filename(doclist_name, generation, "doclist");
segment_filename(del_name, generation, "del");

ATIRE_API *engine = new ATIRE_API();
if (engine->open(ATIRE_API::INDEX_IN_MEMORY, index_name, doclist_name, /*quantize =*/ 0, /*quantization_bits =*/ 0) != 0)
	{
	delete engine;
	return 1;
	}

if (segment_count >= segments_allocated)
	{
	long long bigger_size = segments_allocated == 0 ? 8 : segments_allocated * 2;
	segment *bigger = new segment[bigger_size];
	memcpy(bigger, segments, (size_t)(segment_count * sizeof(*segments)));
	delete [] segments;
	segments = bigger;
	segments_allocated = bigger_size;
	}
segments[segment_count].generation = generation;
segments[segment_count].engine = engine;
segments[segment_count].tombstones = ANT_index_tombstones::load(del_name, engine->get_document_count());
segment_count++;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::FLUSH()
	----------------------------
	Serialize the live memory segment to disk, register it in the manifest,
	reopen it as a disk segment, and start a fresh memory segment.
	Order matters for crash safety: the manifest is written only after the
	segment files are complete, so a crash mid-flush leaves an orphan file
	that startup cleanup removes (Task 10) -- never a manifest pointing at
	a torn segment.
*/
long ATIRE_segment_index::flush(void)
{
char del_name[4096];

if (writer_documents == 0)
	return 0;					// nothing to do

delete writer_engine;			// non-owning wrapper; drop before finish()
writer_engine = NULL;

writer->finish();				// serialises the memory index to seg_G.aspt / seg_G.doclist

/*
	Deletions that happened while this segment was in memory become its .del file
*/
if (writer_tombstones->count() > 0)
	{
	segment_filename(del_name, writer_generation, "del");
	if (writer_tombstones->save(del_name) != 0)
		return 1;
	}

long long flushed_generation = writer_generation;
delete writer;
writer = NULL;
delete writer_tombstones;
writer_tombstones = NULL;

if (append_segment(flushed_generation) != 0)
	return 1;

manifest->add_segment(flushed_generation);
if (manifest->save() != 0)
	return 1;

start_new_writer();
return 0;
}
```

And extend `open()` — replace the comment "Disk segments are opened in Task 7" with:

```cpp
long long which;
for (which = 0; which < manifest->segment_count(); which++)
	if (append_segment(manifest->get_segment(which)) != 0)
		return 1;
```

Implementer notes:
- `ATIRE_indexer::finish()` writes the index to the `-findex` filename and closes the doclist file (`id_list` was opened `"wbx"` at init — exclusive create, which is why every writer generation gets fresh filenames).
- If `ATIRE_API::open` with `INDEX_IN_MEMORY` (loads whole index into RAM, enum value 1 in `atire_api.h:57`) fails on the doclist, check what `atire.cpp`/`atire_api_server.cpp` pass for `doclist_filename` and mirror the working invocation. `quantize = 0` because Phase 2's merge cannot consume quantized segments (spec §6).
- After `writer->finish()`, `writer->get_index()` may be invalid — that is why the NRT wrapper is deleted first and the writer object discarded entirely.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: `test_nrt_add_and_search OK`, `test_flush_and_reopen OK`, `PASSED`

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat: flush memory segment to disk segment; reopen index from manifest"
```

---

### Task 8: Multi-segment growth — several disk segments, merged scoring

**Files:**
- Test only: `tests/test_segment_index.cpp` (append `test_multi_segment_growth`) — Task 6/7 code should already handle this; this task proves it and fixes whatever falls out.

- [ ] **Step 1: Write the test — append and call from `main()`**

```cpp
/*
	TEST_MULTI_SEGMENT_GROWTH()
	---------------------------
	Grow the index across three flushes; results must merge across all
	segments plus the live memory segment, ranked consistently.
*/
static void test_multi_segment_growth(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

/*
	Three batches of 10 docs, each flushed -> three disk segments.
	Every doc contains "common"; doc i contains "unique<i>".
*/
long long batch, expected_total = 0;
for (batch = 0; batch < 3; batch++)
	{
	for (i = 0; i < 10; i++)
		{
		long long n = batch * 10 + i;
		sprintf(key, "doc-%lld", n);
		sprintf(doc, "<DOC>common unique%lld filler words here</DOC>", n);
		CHECK(index->add_document(key, doc) >= 0);
		expected_total++;
		}
	CHECK(index->flush() == 0);
	}

/*
	Two more docs stay in memory (4th, unflushed segment)
*/
index->add_document("doc-30", "<DOC>common unique30</DOC>");
index->add_document("doc-31", "<DOC>common unique31</DOC>");
expected_total += 2;

CHECK(index->get_document_count() == expected_total);

/*
	A term in every doc: all 32 found across 4 segments
*/
strcpy(query, "common");
CHECK(index->search(query, 100) == expected_total);

/*
	top_k truncation works across segments
*/
strcpy(query, "common");
CHECK(index->search(query, 5) == 5);

/*
	Unique terms resolve to the right doc regardless of segment
*/
strcpy(query, "unique0");
CHECK(index->search(query, 10) == 1);
CHECK(strcmp(index->get_hit(0)->filename, "doc-0") == 0);
strcpy(query, "unique25");
CHECK(index->search(query, 10) == 1);
CHECK(strcmp(index->get_hit(0)->filename, "doc-25") == 0);
strcpy(query, "unique31");
CHECK(index->search(query, 10) == 1);
CHECK(strcmp(index->get_hit(0)->filename, "doc-31") == 0);

delete index;
delete [] dir;
printf("test_multi_segment_growth OK\n");
}
```

- [ ] **Step 2: Run the test**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: all three test functions pass. If this passes first try, Tasks 6–7 were built correctly — proceed to commit. If it fails, debug here: likely suspects are per-segment `hits` exceeding what `accumulator_pointers` actually holds (clamp `which < list->results_list_length` in `search_one_segment` — add that clamp regardless), and filename pointers into a segment's buffer being reused between hits (if `get_document_filename_from_doclist` returns a shared static buffer, copy the string into a per-search arena inside `search_one_segment`: allocate with `new char[]`, free all at the start of the next `search()`).

- [ ] **Step 3: Commit**

```bash
git add tests/test_segment_index.cpp atire/atire_segment_index.cpp
git commit -m "test: multi-segment growth with merged cross-segment ranking"
```

---

### Task 9: Update and delete via tombstones + keymap

**Files:**
- Modify: `atire/atire_segment_index.cpp` (implement `update_document`, `delete_document`)
- Test: `tests/test_segment_index.cpp` (append `test_update_and_delete`)

- [ ] **Step 1: Write the failing test — append and call from `main()`**

```cpp
/*
	TEST_UPDATE_AND_DELETE()
	------------------------
*/
static void test_update_and_delete(void)
{
char *dir = make_index_dir();
char query[64];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

index->add_document("doc-1", "<DOC>aardvark original</DOC>");
index->add_document("doc-2", "<DOC>quokka stable</DOC>");
CHECK(index->flush() == 0);

/*
	Update a flushed document: old version tombstoned in disk segment,
	new version searchable from memory segment
*/
long long new_handle = index->update_document("doc-1", "<DOC>aardvark revised wombat</DOC>");
CHECK(new_handle >= 0);

strcpy(query, "aardvark");
CHECK(index->search(query, 10) == 1);			// not 2: old version filtered
CHECK(strcmp(index->get_hit(0)->filename, "doc-1") == 0);
CHECK(ATIRE_segment_index::make_handle(index->get_hit(0)->generation, index->get_hit(0)->docid) == new_handle);
strcpy(query, "original");
CHECK(index->search(query, 10) == 0);			// old body no longer reachable
strcpy(query, "wombat");
CHECK(index->search(query, 10) == 1);			// new body is
CHECK(index->get_document_count() == 2);

/*
	Update an unflushed (memory segment) document
*/
index->add_document("doc-3", "<DOC>ephemeral first</DOC>");
index->update_document("doc-3", "<DOC>ephemeral second</DOC>");
strcpy(query, "ephemeral");
CHECK(index->search(query, 10) == 1);
strcpy(query, "first");
CHECK(index->search(query, 10) == 0);

/*
	Delete
*/
CHECK(index->delete_document("doc-2") == 0);
strcpy(query, "quokka");
CHECK(index->search(query, 10) == 0);
CHECK(index->delete_document("no-such-key") == 1);

/*
	upsert: update of an unknown key behaves as add
*/
CHECK(index->update_document("doc-new", "<DOC>upserted marsupial</DOC>") >= 0);
strcpy(query, "marsupial");
CHECK(index->search(query, 10) == 1);

/*
	Tombstones survive flush + reopen
*/
CHECK(index->flush() == 0);
delete index;
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
strcpy(query, "original");
CHECK(reopened->search(query, 10) == 0);
strcpy(query, "quokka");
CHECK(reopened->search(query, 10) == 0);
strcpy(query, "wombat");
CHECK(reopened->search(query, 10) == 1);
strcpy(query, "first");
CHECK(reopened->search(query, 10) == 0);
strcpy(query, "second");
CHECK(reopened->search(query, 10) == 1);
delete reopened;
delete [] dir;
printf("test_update_and_delete OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: FAIL at `CHECK(new_handle >= 0)` (stub returns -1).

- [ ] **Step 3: Implement update/delete**

Replace the Task-9 stubs in `atire/atire_segment_index.cpp`:

```cpp
/*
	ATIRE_SEGMENT_INDEX::TOMBSTONE()  (private helper -- add to the header's private section)
	--------------------------------
	Mark (generation, docid) deleted.  For a disk segment the .del file is
	persisted immediately (write-temp + rename, so it is crash-atomic).
*/
long ATIRE_segment_index::tombstone(long long generation, long long docid)
{
char del_name[4096];
long long which;

if (generation == writer_generation)
	{
	writer_tombstones->set_deleted(docid);
	writer_engine_stale = 1;		// results filtered per-search anyway, but keep the view coherent
	return 0;
	}
for (which = 0; which < segment_count; which++)
	if (segments[which].generation == generation)
		{
		segments[which].tombstones->set_deleted(docid);
		segment_filename(del_name, generation, "del");
		return segments[which].tombstones->save(del_name);
		}
return 1;			// unknown segment: keymap and manifest disagree (corruption)
}

/*
	ATIRE_SEGMENT_INDEX::DELETE_DOCUMENT()
	--------------------------------------
*/
long ATIRE_segment_index::delete_document(const char *key)
{
long long generation, docid;

if (!keymap->find(key, &generation, &docid))
	return 1;
if (tombstone(generation, docid) != 0)
	return 1;
keymap->remove(key);
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::UPDATE_DOCUMENT()
	--------------------------------------
	Upsert.  Add the new version FIRST, then tombstone the old (spec §2.7):
	a crash in between leaves a transient duplicate rather than a lost
	document; duplicates resolve because the keymap points at the newer copy.
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
```

Add to the header's private section:

```cpp
	long tombstone(long long generation, long long docid);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: all four test functions pass, `PASSED`.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat: update/delete documents via tombstones and keymap"
```

---

### Task 10: Auto-flush threshold and startup orphan cleanup

**Files:**
- Modify: `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp` (append `test_autoflush_and_orphan_cleanup`)

- [ ] **Step 1: Write the failing test — append and call from `main()`**

```cpp
/*
	TEST_AUTOFLUSH_AND_ORPHAN_CLEANUP()
	-----------------------------------
*/
static void test_autoflush_and_orphan_cleanup(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[128], orphan[4096];
long long i;

/*
	Auto-flush: threshold 5, add 12 docs -> at least 2 disk segments,
	everything still searchable
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->set_flush_threshold(5);
for (i = 0; i < 12; i++)
	{
	sprintf(key, "doc-%lld", i);
	sprintf(doc, "<DOC>common unique%lld</DOC>", i);
	CHECK(index->add_document(key, doc) >= 0);
	}
strcpy(query, "common");
CHECK(index->search(query, 100) == 12);
delete index;

/*
	Orphan cleanup: a seg file not referenced by the manifest (as left by a
	crash mid-flush) is removed at open
*/
sprintf(orphan, "%s/seg_999999.aspt", dir);
FILE *fp = fopen(orphan, "wb");
fputs("torn segment from a crash", fp);
fclose(fp);

ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(access(orphan, F_OK) != 0);		// cleaned up
strcpy(query, "common");
long long hits = reopened->search(query, 100);
CHECK(hits >= 10);						// all flushed docs survive (unflushed remainder of the 12 may be lost: relaxed durability)
delete reopened;
delete [] dir;
printf("test_autoflush_and_orphan_cleanup OK\n");
}
```

Add `#include <unistd.h>` at the top of the test file if not already present (for `access`).

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: FAIL — either fewer than 12 results with threshold set (auto-flush not firing is *not* a failure of search, so the actual first failure is the orphan-file `access` check).

- [ ] **Step 3: Implement**

In `add_document`, after `keymap->add(...)` and before `return`, add:

```cpp
if (flush_after_documents > 0 && writer_documents >= flush_after_documents)
	flush();
```

**Careful:** `flush()` calls `start_new_writer()` which changes `writer_generation` — compute the return handle *before* the flush check:

```cpp
long long handle = make_handle(writer_generation, docid);
if (flush_after_documents > 0 && writer_documents >= flush_after_documents)
	flush();
return handle;
```

In `open()`, after the manifest loop and before `start_new_writer()`, add orphan cleanup:

```cpp
/*
	Remove segment files not referenced by the manifest: a crash between
	writing segment files and saving the manifest leaves such orphans, and
	they are garbage by design (spec §3).
*/
#include <dirent.h>   // (goes at the top of the file with the other includes)

DIR *directory_handle = opendir(directory);
if (directory_handle != NULL)
	{
	struct dirent *entry;
	while ((entry = readdir(directory_handle)) != NULL)
		if (strncmp(entry->d_name, "seg_", 4) == 0)
			{
			long long file_generation = atoll(entry->d_name + 4);
			if (!manifest->contains(file_generation))
				{
				char victim[4096];
				sprintf(victim, "%s/%s", directory, entry->d_name);
				remove(victim);
				}
			}
	closedir(directory_handle);
	}
```

**Ordering note:** this must run *before* `start_new_writer()` bumps the generation and *after* the manifest is loaded; the current writer's files don't exist yet at that point, so they can't be swept. But note that `start_new_writer` saves the manifest with the bumped generation precisely so that a crashed session's writer generation is never reused — the sweep then collects that crashed writer's partial files on the *next* open. Verify the sweep does not delete the doclist of a segment whose generation IS in the manifest (`atoll("000003.doclist" + …)` — the parse reads the number regardless of extension, which is correct: all three extensions share the segment's fate).

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: all five test functions pass, `PASSED`.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat: auto-flush threshold and startup orphan segment cleanup"
```

---

### Task 11: Keymap recovery — rebuild from segment doclists

**Files:**
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp` (add `rebuild_keymap()`; call it from `open()` when `keymap.log` is missing but segments exist)
- Modify: `source/index_keymap.h` / `source/index_keymap.cpp` (add `static long log_exists(const char *directory)`)
- Test: `tests/test_segment_index.cpp` (append `test_keymap_recovery`)

- [ ] **Step 1: Write the failing test — append and call from `main()`**

```cpp
/*
	TEST_KEYMAP_RECOVERY()
	----------------------
	Delete keymap.log; on reopen it is rebuilt from segment doclists
	(newest segment wins for duplicate keys) and updates still work.
*/
static void test_keymap_recovery(void)
{
char *dir = make_index_dir();
char query[64], victim[4096];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->add_document("doc-1", "<DOC>aardvark original</DOC>");
CHECK(index->flush() == 0);
index->update_document("doc-1", "<DOC>aardvark revised</DOC>");	// newer copy in second segment
index->add_document("doc-2", "<DOC>quokka</DOC>");
CHECK(index->flush() == 0);
delete index;

sprintf(victim, "%s/keymap.log", dir);
CHECK(remove(victim) == 0);

ATIRE_segment_index *recovered = new ATIRE_segment_index();
CHECK(recovered->open(dir) == 0);

/*
	The rebuilt keymap must point doc-1 at the NEWER copy: an update through
	it tombstones the revised version, not the (already dead) original.
*/
CHECK(recovered->update_document("doc-1", "<DOC>aardvark final</DOC>") >= 0);
strcpy(query, "aardvark");
CHECK(recovered->search(query, 10) == 1);
strcpy(query, "revised");
CHECK(recovered->search(query, 10) == 0);
strcpy(query, "final");
CHECK(recovered->search(query, 10) == 1);
CHECK(recovered->delete_document("doc-2") == 0);
strcpy(query, "quokka");
CHECK(recovered->search(query, 10) == 0);

delete recovered;
delete [] dir;
printf("test_keymap_recovery OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: FAIL — after removing `keymap.log`, `update_document("doc-1", …)` acts as a blind add (no old version tombstoned), so `search("revised")` returns 1, or `search("aardvark")` returns 2.

- [ ] **Step 3: Implement**

`source/index_keymap.h` / `.cpp` — add:

```cpp
/*
	ANT_INDEX_KEYMAP::LOG_EXISTS()
	------------------------------
*/
long ANT_index_keymap::log_exists(const char *directory)
{
char filename[4096];
FILE *fp;

sprintf(filename, "%s/keymap.log", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
fclose(fp);
return 1;
}
```

`atire/atire_segment_index.cpp` — add private method (declare in header):

```cpp
/*
	ATIRE_SEGMENT_INDEX::REBUILD_KEYMAP()
	-------------------------------------
	The keymap log is a cache over the segment doclists.  Rebuild it by
	scanning segments oldest to newest: a key seen again in a newer segment
	supersedes the older copy, whose docid gets tombstoned (it was dead
	before the log was lost -- updates always tombstone the older copy).
	Tombstoned docids must not (re)enter the map.
*/
void ATIRE_segment_index::rebuild_keymap(void)
{
long long which, docid, old_generation, old_docid;
char *filename;

for (which = 0; which < segment_count; which++)
	for (docid = 0; docid < segments[which].engine->get_document_count(); docid++)
		{
		if (segments[which].tombstones->is_deleted(docid))
			continue;
		filename = segments[which].engine->get_document_filename_from_doclist(docid);
		if (filename == NULL)
			continue;
		if (keymap->find(filename, &old_generation, &old_docid))
			tombstone(old_generation, old_docid);		// duplicate: older copy is dead
		keymap->add(filename, segments[which].generation, docid);
		}
}
```

In `open()`, capture `long had_keymap_log = ANT_index_keymap::log_exists(directory);` *before* `ANT_index_keymap::load(directory)` (load creates the log file for appending). After the segment-opening loop and orphan cleanup, add:

```cpp
if (!had_keymap_log && segment_count > 0)
	rebuild_keymap();
```

Implementer note: if `get_document_filename_from_doclist` returns a static/shared buffer, `keymap->add` already copies the key (it `strcpy`s into its own slot), so no extra copy is needed here — but the `find` call uses the same buffer before the copy, which is fine.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: all six test functions pass, `PASSED`.

- [ ] **Step 5: Commit**

```bash
git add source/index_keymap.h source/index_keymap.cpp atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat: rebuild keymap from segment doclists when keymap.log is lost"
```

---

### Task 12: Equivalence verification and full regression

**Files:**
- Test: `tests/test_segment_index.cpp` (append `test_equivalence_with_oneshot`)

- [ ] **Step 1: Write the test — append and call from `main()`**

The final check from spec §5: after adds + updates + deletes across several segments, the *result sets* (matched documents, by key) for exact-term queries must equal those of a conceptual one-shot index of the surviving collection. (Scores may differ — per-segment stats — so compare membership, not scores; unique terms and all-doc terms make membership deterministic.)

```cpp
/*
	TEST_EQUIVALENCE_WITH_ONESHOT()
	-------------------------------
	Final state after a messy history must match the logical collection:
	docs 0..19 added; evens 0..8 updated; docs 15..19 deleted.
	Surviving logical collection: 15 docs, with evens 0..8 revised.
*/
static void test_equivalence_with_oneshot(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->set_flush_threshold(7);				// force segment boundaries in awkward places

for (i = 0; i < 20; i++)
	{
	sprintf(key, "doc-%lld", i);
	sprintf(doc, "<DOC>common body%lld</DOC>", i);
	CHECK(index->add_document(key, doc) >= 0);
	}
for (i = 0; i < 10; i += 2)
	{
	sprintf(key, "doc-%lld", i);
	sprintf(doc, "<DOC>common revised%lld</DOC>", i);
	CHECK(index->update_document(key, doc) >= 0);
	}
for (i = 15; i < 20; i++)
	{
	sprintf(key, "doc-%lld", i);
	CHECK(index->delete_document(key) == 0);
	}
CHECK(index->flush() == 0);

CHECK(index->get_document_count() == 15);

strcpy(query, "common");
CHECK(index->search(query, 100) == 15);

for (i = 0; i < 15; i++)
	{
	sprintf(key, "doc-%lld", i);
	if (i < 10 && i % 2 == 0)
		sprintf(query, "revised%lld", i);
	else
		sprintf(query, "body%lld", i);
	CHECK(index->search(query, 10) == 1);
	CHECK(strcmp(index->get_hit(0)->filename, key) == 0);
	}
for (i = 0; i < 10; i += 2)
	{
	sprintf(query, "body%lld", i);		// pre-update bodies unreachable
	CHECK(index->search(query, 10) == 0);
	}
for (i = 15; i < 20; i++)
	{
	sprintf(query, "body%lld", i);		// deleted docs unreachable
	CHECK(index->search(query, 10) == 0);
	}

/*
	And all of it survives a reopen
*/
delete index;
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(reopened->get_document_count() == 15);
strcpy(query, "common");
CHECK(reopened->search(query, 100) == 15);
delete reopened;
delete [] dir;
printf("test_equivalence_with_oneshot OK\n");
}
```

- [ ] **Step 2: Run the full test suite and the pre-existing binaries**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: all seven test functions pass, `PASSED`.

Run: `make test_index_tombstones test_index_manifest test_index_keymap test_memory_engine_ownership && ./bin/test_index_tombstones && ./bin/test_index_manifest && ./bin/test_index_keymap && ./bin/test_memory_engine_ownership`
Expected: `PASSED` × 4.

Run: `make internal` (builds `index`, `atire`, `atire_client`, `atire_broker`, `atire_dictionary`, `atire_merge`, `atire_doclist`)
Expected: clean build — proves the core-engine edits (Task 5) broke nothing.

- [ ] **Step 3: Commit**

```bash
git add tests/test_segment_index.cpp
git commit -m "test: end-state equivalence after mixed add/update/delete history"
```

---

## Self-review record

- **Spec coverage (Phase 1):** manifest+atomic rename → Task 3/7; live memory segment + NRT rebuild-on-dirty + non-owning engine → Tasks 5/6; tombstones + persistence + query-time filtering with over-fetch → Tasks 2/8/9; keymap + append log + returns (segment, docid) handle → Tasks 4/6; keymap rebuild from doclists → Task 11; write API add/update/delete/flush + upsert + add-before-tombstone ordering → Tasks 6/9; flush thresholds (freshness profiles) → Task 10; orphan cleanup → Task 10; non-quantized flush → Task 7 (`quantize = 0`); testing strategy → Tasks 8/12. Deliberately deferred (stated in header): compaction/merge policy (Phase 2), WAL, global stats, locks, Node/SWIG exposure.
- **Known engine-contact risks, flagged in-task:** exact `ATIRE_indexer::init` option handling (Task 5 note), `get_docno` off-by-one (Task 6 note, caught by consistency assertion), `ATIRE_API::open` doclist argument shape (Task 7 note), static filename buffer reuse (Task 8 note, Task 11 note).

# Compacting Merge (Incremental Index Phase 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Physically drop tombstoned documents and merge segments: `ANT_index_merger` (tombstone-aware N-way postings merge), `ATIRE_segment_index::compact()` (crash-safe swap + keymap remap), and `maintain()` (tiered policy), per spec `docs/superpowers/specs/2026-07-06-compacting-merge-design.md`.

**Architecture:** `ANT_index_merger` (new, `source/`) is a faithful in-process adaptation of `atire_merge.cpp`'s `merge_index()` — same N-way B-tree walk, same impact-quantum alignment, same output file layout — specialized to Phase 1 segments (non-quantized, unpruned, no stored documents, FILENAME_INDEX + IMPACT_HEADER build) with tombstone filtering + docid renumbering injected at the quantum-decode step, and all `exit()` calls replaced by error returns. `compact()` orchestrates merge → marker → keymap remap → atomic manifest swap → cleanup; `open()` gains marker-triggered keymap rebuild; `maintain()` runs the tiered policy.

**Tech Stack:** C++ (pre-C++11 house style: no STL, column-0 function bodies, banner comments). Build: `GNUmakefile` auto-globs `source/*.cpp`; `tests/<name>.cpp` → `make <name>` → `bin/<name>`. This build defines `IMPACT_HEADER`, `FILENAME_INDEX`, `SPECIAL_COMPRESSION`, `PARALLEL_INDEXING_DOCUMENTS` — the merger implements ONLY these paths (guard others with `#error` if convenient, or simply omit).

**Ground truth to read before any task:** `atire/atire_merge.cpp` (the adaptation source; key regions cited per task), `atire/atire_segment_index.{h,cpp}` (the consumer), `source/index_tombstones.h`, `source/index_manifest.h`, `source/index_keymap.h`, `tests/test_segment_index.cpp` (harness: `CHECK`, `make_index_dir()`, `unique_term(char*, long long)`).

**Verified engine facts (do not re-derive):**
- `merge_index()` walks one `ANT_btree_iterator` per input in lexicographic lock-step; per term it decompresses each input's impact header (`atire_merge.cpp:835-848`), then for `current_tf` 255→1 concatenates matching quanta across inputs, re-delta-encoding docids with a per-engine `offset` base shift (`atire_merge.cpp:859-915`). **This offset shift is exactly where tombstone filtering + renumbering replaces simple offsetting.**
- Special variables the output must contain (from `atire_merge.cpp:617-772`): `~documentfilenamesstart/finish` (concatenated filename blob), `~documentfilenamesindexstart/finish` (offset array + total, `FILENAME_INDEX` path, lines 693-703), `~documentlongest` (max over inputs, line 729), `~length` (per-doc length vector — MUST be filtered + renumbered, line 764), `~stemmer` (must match across inputs, else error, lines 617-621), `~trimpoint` (write `LONG_MAX`… check what an unpruned Phase 1 segment carries — read what `ANT_memory_index::serialise` writes for `~trimpoint` and mirror it). `~documentoffsets` is doc-store only — Phase 1 segments never store documents, so it is OMITTED (and `get_document_count()`-style consumers don't need it; verify a flushed segment lacks it with `bin/atire_dictionary` or `get_postings_details`).
- Output file assembly (header/dictionary/B-tree/footer): `atire_merge.cpp:604-612` (file header) and `1063-1135` (term qsort, `write_node` second level, root level, footer fields incl. `ANT_file_signature_index`, `ANT_version`, `ANT_file_signature`).
- Helper functions to adapt into the class (currently free functions with globals in `atire_merge.cpp`): `write_postings` (~line 180-275), `write_impact_header_postings` (~280-430), `write_variable`, `write_node` (~104-160), `find_end_of_node` (~80-97), the `postings_list` growth buffer (globals at lines 30-34). `should_prune` is NOT needed (no pruning).
- `SPECIAL_COMPRESSION` quirk: df ≤ 2 non-`~` terms are stored in a compressed special form (`atire_merge.cpp:305-330` handles the conversion) — the adapted `write_impact_header_postings` must keep this behavior or searches for rare terms break.
- Segments are opened in `ATIRE_segment_index` via `ATIRE_API`; the underlying `ANT_search_engine` is reachable via `get_search_engine()`. `ANT_search_engine::get_variable(term)`, `get_postings_details(term, leaf)`, `get_postings(leaf, dest)`, `get_document_filenames(buf, &len)`, `document_count()` are the read APIs `merge_index()` itself uses — the merger uses the same ones.
- `ANT_index_tombstones` exposes `is_deleted(docid)` / `count()`. Manifest: `take_generation()` (save-before-files contract), `add_segment`, `contains`, `save`; **it has no remove_segment — Task 5 adds one.**
- Keymap: `add(key, gen, docid)` overwrites; `rebuild_keymap()` exists on `ATIRE_segment_index` (returns `long`, 0 = success); `ANT_index_keymap::log_exists(dir)`.

**Worktree:** create via superpowers:using-git-worktrees (`.worktrees/compacting-merge`, branch `feature/compacting-merge`). First build in a fresh worktree: `mkdir -p obj bin && make all` (external libs), then `make internal` / `make tests` are incremental.

---

### Task 1: Docid renumbering table (`ANT_docid_renumberer`)

The pure core every other piece depends on: maps (input segment ordinal, old docid) → new dense docid, skipping tombstoned docs.

**Files:**
- Create: `source/index_merge.h` (starts with just this class; Task 2 extends the file)
- Create: `source/index_merge.cpp`
- Test: `tests/test_index_merge.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
/*
	TEST_INDEX_MERGE.CPP
	--------------------
	Unit tests for the compacting merger's components.  End-to-end merge
	tests live in test_segment_index.cpp; this file tests the pure parts.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "index_merge.h"
#include "index_tombstones.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
/*
	Two segments: seg0 has 5 docs with docids 1,3 deleted; seg1 has 3 docs, docid 0 deleted.
	Live docs in order: seg0:{0,2,4} -> new 0,1,2 ; seg1:{1,2} -> new 3,4.
*/
ANT_index_tombstones *t0 = new ANT_index_tombstones(5);
ANT_index_tombstones *t1 = new ANT_index_tombstones(3);
t0->set_deleted(1);
t0->set_deleted(3);
t1->set_deleted(0);

long long counts[2];
ANT_index_tombstones *stones[2];
counts[0] = 5;
counts[1] = 3;
stones[0] = t0;
stones[1] = t1;

ANT_docid_renumberer *map = new ANT_docid_renumberer(stones, counts, 2);

CHECK(map->total_live_documents() == 5);

CHECK(map->renumber(0, 0) == 0);
CHECK(map->renumber(0, 1) == -1);		// deleted
CHECK(map->renumber(0, 2) == 1);
CHECK(map->renumber(0, 3) == -1);		// deleted
CHECK(map->renumber(0, 4) == 2);
CHECK(map->renumber(1, 0) == -1);		// deleted
CHECK(map->renumber(1, 1) == 3);
CHECK(map->renumber(1, 2) == 4);

/*
	No tombstones at all: identity within segment + base offset
*/
ANT_index_tombstones *empty0 = new ANT_index_tombstones(2);
ANT_index_tombstones *empty1 = new ANT_index_tombstones(2);
ANT_index_tombstones *plain[2];
long long plain_counts[2];
plain[0] = empty0;
plain[1] = empty1;
plain_counts[0] = 2;
plain_counts[1] = 2;
ANT_docid_renumberer *identity = new ANT_docid_renumberer(plain, plain_counts, 2);
CHECK(identity->total_live_documents() == 4);
CHECK(identity->renumber(0, 1) == 1);
CHECK(identity->renumber(1, 0) == 2);
CHECK(identity->renumber(1, 1) == 3);

delete map;
delete identity;
delete t0;
delete t1;
delete empty0;
delete empty1;
printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_index_merge`
Expected: compile FAILURE — `index_merge.h: No such file or directory`

- [ ] **Step 3: Implement**

`source/index_merge.h`:

```cpp
/*
	INDEX_MERGE.H
	-------------
	Tombstone-aware N-way segment merger for the segmented incremental index
	(see docs/superpowers/specs/2026-07-06-compacting-merge-design.md).

	ANT_docid_renumberer maps (segment ordinal, old docid) to the dense new
	docid a compacted output segment assigns, or -1 for tombstoned documents.
*/
#ifndef INDEX_MERGE_H_
#define INDEX_MERGE_H_

class ANT_index_tombstones;

class ANT_docid_renumberer
{
private:
	long long **new_docid;			// per segment, per old docid: new docid or -1
	long long *documents;			// per segment: old document count
	long long segment_count;
	long long live_documents;

public:
	ANT_docid_renumberer(ANT_index_tombstones **tombstones, long long *document_counts, long long segment_count);
	~ANT_docid_renumberer();

	long long renumber(long long segment, long long old_docid) { return new_docid[segment][old_docid]; }
	long long total_live_documents(void) { return live_documents; }
	long long live_in_segment(long long segment);
} ;

#endif /* INDEX_MERGE_H_ */
```

`source/index_merge.cpp`:

```cpp
/*
	INDEX_MERGE.CPP
	---------------
*/
#include <stdio.h>
#include <stdlib.h>
#include "index_merge.h"
#include "index_tombstones.h"

/*
	ANT_DOCID_RENUMBERER::ANT_DOCID_RENUMBERER()
	--------------------------------------------
	Live documents are numbered densely: within a segment in ascending old
	docid order, segments in the order given (which is manifest order).
*/
ANT_docid_renumberer::ANT_docid_renumberer(ANT_index_tombstones **tombstones, long long *document_counts, long long segments)
{
long long segment, docid, next = 0;

segment_count = segments;
documents = new long long[segment_count];
new_docid = new long long *[segment_count];
for (segment = 0; segment < segment_count; segment++)
	{
	documents[segment] = document_counts[segment];
	new_docid[segment] = new long long[documents[segment]];
	for (docid = 0; docid < documents[segment]; docid++)
		if (tombstones[segment]->is_deleted(docid))
			new_docid[segment][docid] = -1;
		else
			new_docid[segment][docid] = next++;
	}
live_documents = next;
}

/*
	ANT_DOCID_RENUMBERER::~ANT_DOCID_RENUMBERER()
	---------------------------------------------
*/
ANT_docid_renumberer::~ANT_docid_renumberer()
{
long long segment;

for (segment = 0; segment < segment_count; segment++)
	delete [] new_docid[segment];
delete [] new_docid;
delete [] documents;
}

/*
	ANT_DOCID_RENUMBERER::LIVE_IN_SEGMENT()
	---------------------------------------
*/
long long ANT_docid_renumberer::live_in_segment(long long segment)
{
long long docid, live = 0;

for (docid = 0; docid < documents[segment]; docid++)
	if (new_docid[segment][docid] >= 0)
		live++;
return live;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_index_merge && ./bin/test_index_merge`
Expected: `PASSED`

- [ ] **Step 5: Commit**

```bash
git add source/index_merge.h source/index_merge.cpp tests/test_index_merge.cpp
git commit -m "feat: docid renumbering table for compacting merge"
```

---

### Task 2: `ANT_index_merger` — merge without tombstones

The big adaptation task: produce a valid, searchable `.aspt` from N input segments (no deletions yet — pure concatenation semantics identical to `atire_merge`). Getting the output to OPEN and MATCH is the hard part; tombstones (Task 3) are then a small delta.

**Files:**
- Modify: `source/index_merge.h` (add ANT_index_merger)
- Modify: `source/index_merge.cpp`
- Test: `tests/test_segment_index.cpp` (append `test_merger_no_tombstones`)

- [ ] **Step 1: Write the failing test — append to `tests/test_segment_index.cpp`:**

```cpp
#include "../source/index_merge.h"
#include "../source/index_tombstones.h"
#include "../atire/atire_api.h"
#include "../source/search_engine.h"
#include "../source/search_engine_btree_leaf.h"

/*
	TEST_MERGER_NO_TOMBSTONES()
	---------------------------
	Merge two flushed segments (no deletions) and verify the output opens
	as a normal index whose contents equal the union: same document count,
	same per-term df/cf, same search membership.
*/
static void test_merger_no_tombstones(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16], seg_a[4096], seg_b[4096], merged[4096];
long long i;

/*
	Build two segments through the normal write path
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 6; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common shared%s %s</DOC>", i % 2 == 0 ? "even" : "odd", letters);
	CHECK(index->add_document(key, doc) >= 0);
	if (i == 2)
		CHECK(index->flush() == 0);
	}
CHECK(index->flush() == 0);
delete index;

snprintf(seg_a, sizeof(seg_a), "%s/seg_000001.aspt", dir);
snprintf(seg_b, sizeof(seg_b), "%s/seg_000002.aspt", dir);
snprintf(merged, sizeof(merged), "%s/seg_000009.aspt", dir);

/*
	Open the inputs and merge
*/
ATIRE_API *engine_a = new ATIRE_API();
ATIRE_API *engine_b = new ATIRE_API();
CHECK(engine_a->open(ATIRE_API::INDEX_IN_MEMORY, seg_a, NULL, 0, 0) == 0);
CHECK(engine_b->open(ATIRE_API::INDEX_IN_MEMORY, seg_b, NULL, 0, 0) == 0);

ANT_search_engine *engines[2];
engines[0] = engine_a->get_search_engine();
engines[1] = engine_b->get_search_engine();
ANT_index_tombstones *no_deletes[2];
no_deletes[0] = new ANT_index_tombstones(engine_a->get_document_count());
no_deletes[1] = new ANT_index_tombstones(engine_b->get_document_count());

ANT_index_merger *merger = new ANT_index_merger();
CHECK(merger->merge(engines, no_deletes, 2, merged) == 0);
delete merger;

/*
	Open the output and compare against the union
*/
ATIRE_API *out = new ATIRE_API();
CHECK(out->open(ATIRE_API::INDEX_IN_MEMORY, merged, NULL, 0, 0) == 0);
CHECK(out->get_document_count() == 6);

/*
	Per-term stats must equal the sum of the inputs' (df: 6/3/3/1 for
	common/sharedeven/sharedodd/unique terms)
*/
ANT_search_engine_btree_leaf leaf;
CHECK(out->get_search_engine()->get_postings_details((char *)"common", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 6);
CHECK(out->get_search_engine()->get_postings_details((char *)"sharedeven", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 3);
CHECK(out->get_search_engine()->get_postings_details((char *)"sharedodd", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 3);

/*
	Search membership: every doc findable by its unique term, filenames intact
*/
strcpy(query, "common");
CHECK(out->search(query, 10) == 6);
for (i = 0; i < 6; i++)
	{
	unique_term(letters, i);
	strcpy(query, letters);
	CHECK(out->search(query, 10) == 1);
	}

/*
	Filename index survived: docid 0 is seg_a's first doc
*/
char filename_buffer[4096];
CHECK(strcmp(out->get_document_filename(filename_buffer, 0), "doc-0") == 0);
CHECK(strcmp(out->get_document_filename(filename_buffer, 3), "doc-3") == 0);

delete out;
delete engine_a;
delete engine_b;
delete no_deletes[0];
delete no_deletes[1];
delete [] dir;
printf("test_merger_no_tombstones OK\n");
}
```

Call it from `main()` after the existing functions. Adapt include paths/duplicate includes to what the file already has; if `unique_term` writes the full `"uniqueXY"` token, adjust the doc/query sprintf accordingly (match Task 8/12 usage already in the file). If `ATIRE_API::open`'s doclist argument rejects NULL, pass a dummy path — under V5 it is unread (verify against `append_segment`'s existing call and mirror it).

- [ ] **Step 2: Run to verify it fails**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: compile FAILURE — `ANT_index_merger` undeclared.

- [ ] **Step 3: Implement `ANT_index_merger`**

Add to `source/index_merge.h`:

```cpp
class ANT_search_engine;

/*
	class ANT_INDEX_MERGER
	----------------------
	In-process adaptation of atire_merge.cpp's merge_index(), specialized to
	Phase 1 segments: non-quantized, unpruned, no stored documents,
	FILENAME_INDEX + IMPACT_HEADER build.  All failures return nonzero and
	delete the partial output; no exit().
*/
class ANT_index_merger
{
private:
	unsigned char *postings_list;
	long long postings_list_size;
	long long longest_postings;
	int32_t longest_term;
	int64_t highest_df;
	long long terms_so_far;
	ANT_docid_renumberer *renumberer;
	/*
		further private state mirroring merge_index()'s locals: iterators,
		leaves, raw buffers, impact headers, term_list, memory_stats, factory,
		output ANT_file -- declared as members so helpers need no globals
	*/

private:
	/* adapted helpers -- same names as their atire_merge.cpp sources */
	ANT_memory_index_hash_node **find_end_of_node(ANT_memory_index_hash_node **start);
	ANT_memory_index_hash_node **write_node(ANT_file *file, ANT_memory_index_hash_node **start);
	ANT_memory_index_hash_node *write_postings(char *term, ANT_compressable_integer *raw_postings, ANT_file *index, ANT_search_engine_btree_leaf *leaf, long long output_documents);
	ANT_memory_index_hash_node *write_impact_header_postings(char *term, ANT_compressable_integer *header, ANT_compressable_integer quantum_count, ANT_compressable_integer *raw_postings, ANT_file *index, ANT_search_engine_btree_leaf *leaf, long long output_documents);
	ANT_memory_index_hash_node *write_variable(const char *name, long long value, ANT_file *index, long long output_documents);
	long grow_postings_buffer(long long needed);

public:
	ANT_index_merger();
	~ANT_index_merger();

	/*
		Merge the given open engines (with their tombstones) into a new index
		file at output_filename.  0 on success; on failure the partial output
		is removed.  Inputs are unmodified.
	*/
	long merge(ANT_search_engine **engines, ANT_index_tombstones **tombstones, long long engine_count, const char *output_filename);
} ;
```

(Exact private-member list is the implementer's to finalize — the contract is: no globals, no statics that survive across `merge()` calls, `merge()` re-entrant for sequential use.)

Implementation in `source/index_merge.cpp` — adapt `merge_index()` (`atire/atire_merge.cpp:433-1200`) with these EXACT deltas; everything not listed below is copied faithfully (same buffers, same growth factor 1.6, same qsort/btree/footer sequence):

1. **Setup (source lines 433-598):** inputs arrive as open `ANT_search_engine**` instead of filenames — drop the open/param-block code. Keep: per-engine `ANT_btree_iterator`, `ANT_search_engine_btree_leaf`, raw buffers sized `510 + combined_docs`, impact-header buffers, `term_list` sized by summed unique term counts, `combined_docs` = **`renumberer->total_live_documents()`** (not the raw sum — this is what the output's documents count and buffers key off). Build `renumberer = new ANT_docid_renumberer(...)` from the tombstones + `engines[i]->document_count()`. Reject quantized inputs (`engine->quantized()`) with `return 1` (not exit).
2. **Stemmer check (lines 617-621):** identical logic, `return 1` instead of exit.
3. **No document store:** skip the `do_documents` block entirely (lines 623-648 and 704-724, the `~documentoffsets` writes). Phase 1 segments never store documents.
4. **Filenames (lines 650-703):** iterate engines × docids; **skip docids where `renumberer->renumber(engine, docid) < 0`**; write surviving filenames (and their `filename_index_offsets` entries) in renumbered order — since renumbering preserves (segment, docid) order, the natural loop order IS renumbered order. Write `~documentfilenamesstart/finish` and `~documentfilenamesindexstart/finish` exactly as the source does, with `combined_docs` = live total.
5. **`~documentlongest` (line 729):** max over inputs (a live doc's length can only shrink the max if the longest was deleted — recomputing the true max needs the length vector, which we have: compute it from the filtered `~length` vector in step 6 instead of copying the input max. One-line difference; do it properly).
6. **`~length` (region around line 740-764):** read each input's `~length` postings (decoded to per-doc lengths the same way the source does), build the output vector containing only live docs in renumbered order, set `~documentlongest` from its max, write via the adapted `write_postings`.
7. **Main term walk (lines 780-1010):** identical N-way lexicographic iterator walk, EXCEPT inside the per-quantum document loop (`atire_merge.cpp:889-899`), replace:
```cpp
*current = (ANT_compressable_integer)(decompress_buffer[0] + offset - previous_docid);
previous_docid += *current++;
for (document = 1; document < number_documents; document++)
	{
	*current = decompress_buffer[document];
	previous_docid += *current++;
	}
```
with decode-filter-renumber-re-encode:
```cpp
/*
	decompress_buffer holds delta-encoded docids within this quantum for
	this input; rebuild absolute old docids, map each through the
	renumberer (dropping tombstoned ones), and delta-encode survivors
	against the output stream's running previous_docid.
*/
old_docid = 0;
survivors = 0;
for (document = 0; document < number_documents; document++)
	{
	old_docid += decompress_buffer[document];
	new_docid = renumberer->renumber(engine, old_docid);
	if (new_docid < 0)
		continue;
	*current++ = (ANT_compressable_integer)(new_docid - previous_docid);
	previous_docid = new_docid;
	survivors++;
	}
current_impact_header[ANT_impact_header::NUM_OF_QUANTUMS] += survivors - number_documents;	// correct the doc count added earlier
```
IMPORTANT consequences the implementer must handle (all local to this loop):
   - The quantum's doc count was pre-added at `atire_merge.cpp:888`; correct it as shown (or restructure to add `survivors` instead — cleaner).
   - A quantum whose survivors == 0 across ALL contributing engines must not be emitted: only advance `current_impact_header`/`number_quantums_used` when the merged quantum ends non-empty (restructure the `process_this_tf` block: gather survivors first, then commit the header entry only if > 0).
   - The absolute-docid rebase: in the source, `decompress_buffer[0]` is delta-from-0 within the engine's quantum and `offset` shifts across engines. In the replacement, `old_docid` accumulates deltas to absolute per-engine docids and the renumberer replaces `offset` entirely — the `offset += document_count()` bookkeeping (line 903) becomes dead; remove it.
   - df/cf recomputation (lines 926-934 sum the inputs' values): replace with counts computed from survivors — track per-term `surviving_df` (sum of survivors across quanta... careful: a doc appears in exactly one quantum per term, so df = total survivors) and `surviving_cf` (sum of `impact value × survivors`? NO — cf is the sum of tf over docs; with tf-ordered impacts, quantum impact value IS the tf, so cf = Σ per-quantum (impact × survivors_in_quantum)). Set `leaves[number_engines]->local_document_frequency/local_collection_frequency` from these. If df == 0, skip the term entirely (do not call write_impact_header_postings, do not add to term_list).
8. **`~` terms in the main walk:** the source's main loop processes only non-`~` terms in this region (verify how it partitions; `~`-prefixed vocabulary was handled explicitly before the walk). Mirror the source's partitioning exactly.
9. **Footer (lines 1063-1135):** identical, from member state (`longest_term`, `highest_df`, `longest_postings`, `terms_so_far`).
10. **Failure paths:** every I/O or invariant failure → free everything, `remove(output_filename)`, return 1. Wrap the body so cleanup is single-exit (goto-style cleanup or a small RAII-less `finished:` label is acceptable house style — `atire_merge` free-list at lines 1155-1200 shows everything that must be freed).
11. **SPECIAL_COMPRESSION df ≤ 2 path** (`write_impact_header_postings`, source lines 305-330): copy verbatim — but note df is now the SURVIVING df, so a term can newly fall into this path after filtering. That is correct and intended; just make sure the leaf passed in already has the post-filter df.

- [ ] **Step 4: Run to verify it passes**

Run: `make test_segment_index && ./bin/test_segment_index`
Expected: all prior functions + `test_merger_no_tombstones OK` + `PASSED`.
Also: `make internal` exit 0; `./bin/test_index_merge` still PASSED.

- [ ] **Step 5: Commit**

```bash
git add source/index_merge.h source/index_merge.cpp tests/test_segment_index.cpp
git commit -m "feat: in-process N-way segment merger (no-tombstone path)"
```

---

### Task 3: Tombstone filtering in the merger

**Files:**
- Modify: `source/index_merge.cpp` (only if Task 2 took shortcuts — the filtering code is already in; this task PROVES it)
- Test: `tests/test_segment_index.cpp` (append `test_merger_drops_tombstones`)

- [ ] **Step 1: Write the test — append and call from `main()`:**

```cpp
/*
	TEST_MERGER_DROPS_TOMBSTONES()
	------------------------------
	Merge two segments with deletions; the output must contain exactly the
	live documents, densely renumbered, with corrected df/cf.
*/
static void test_merger_drops_tombstones(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16], seg_a[4096], seg_b[4096], merged[4096], del_a[4096];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 6; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	if (i == 2)
		CHECK(index->flush() == 0);
	}
CHECK(index->flush() == 0);
/*
	Delete doc-1 (segment 1, docid 1) and doc-4 (segment 2, docid 1)
	through the API so the .del files are written for us
*/
CHECK(index->delete_document("doc-1") == 0);
CHECK(index->delete_document("doc-4") == 0);
delete index;

snprintf(seg_a, sizeof(seg_a), "%s/seg_000001.aspt", dir);
snprintf(seg_b, sizeof(seg_b), "%s/seg_000002.aspt", dir);
snprintf(del_a, sizeof(del_a), "%s/seg_000001.del", dir);
snprintf(merged, sizeof(merged), "%s/seg_000009.aspt", dir);

ATIRE_API *engine_a = new ATIRE_API();
ATIRE_API *engine_b = new ATIRE_API();
CHECK(engine_a->open(ATIRE_API::INDEX_IN_MEMORY, seg_a, NULL, 0, 0) == 0);
CHECK(engine_b->open(ATIRE_API::INDEX_IN_MEMORY, seg_b, NULL, 0, 0) == 0);
ANT_search_engine *engines[2];
engines[0] = engine_a->get_search_engine();
engines[1] = engine_b->get_search_engine();
ANT_index_tombstones *stones[2];
char del_b[4096];
snprintf(del_b, sizeof(del_b), "%s/seg_000002.del", dir);
stones[0] = ANT_index_tombstones::load(del_a, engine_a->get_document_count());
stones[1] = ANT_index_tombstones::load(del_b, engine_b->get_document_count());
CHECK(stones[0]->count() == 1);
CHECK(stones[1]->count() == 1);

ANT_index_merger *merger = new ANT_index_merger();
CHECK(merger->merge(engines, stones, 2, merged) == 0);
delete merger;

ATIRE_API *out = new ATIRE_API();
CHECK(out->open(ATIRE_API::INDEX_IN_MEMORY, merged, NULL, 0, 0) == 0);

/*
	4 live docs; df of "common" corrected from 6 to 4
*/
CHECK(out->get_document_count() == 4);
ANT_search_engine_btree_leaf leaf;
CHECK(out->get_search_engine()->get_postings_details((char *)"common", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 4);

/*
	Deleted docs' unique terms are GONE from the vocabulary (df would be 0)
*/
unique_term(letters, 1);
CHECK(out->get_search_engine()->get_postings_details(letters, &leaf) == NULL);
unique_term(letters, 4);
CHECK(out->get_search_engine()->get_postings_details(letters, &leaf) == NULL);

/*
	Survivors searchable; dense renumbering: doc-0,2,3,5 -> 0,1,2,3
*/
strcpy(query, "common");
CHECK(out->search(query, 10) == 4);
char filename_buffer[4096];
CHECK(strcmp(out->get_document_filename(filename_buffer, 0), "doc-0") == 0);
CHECK(strcmp(out->get_document_filename(filename_buffer, 1), "doc-2") == 0);
CHECK(strcmp(out->get_document_filename(filename_buffer, 2), "doc-3") == 0);
CHECK(strcmp(out->get_document_filename(filename_buffer, 3), "doc-5") == 0);

delete out;
delete engine_a;
delete engine_b;
delete stones[0];
delete stones[1];
delete [] dir;
printf("test_merger_drops_tombstones OK\n");
}
```

- [ ] **Step 2: Run.** If Task 2 was implemented per spec this may pass immediately (the filtering loop is the same code path with a non-identity renumberer). If it fails, the bug is real — likely spots: empty-quantum emission, df/cf accounting, or `~length` filtering. Fix in `source/index_merge.cpp` and report root cause.

- [ ] **Step 3: Commit**

```bash
git add source/index_merge.cpp tests/test_segment_index.cpp
git commit -m "test: merger drops tombstoned documents with corrected stats"
```

---

### Task 4: Segment-file deletion helper + manifest `remove_segment`

Two tiny primitives `compact()` needs.

**Files:**
- Modify: `source/index_manifest.h` / `source/index_manifest.cpp`
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp` (private helper)
- Test: `tests/test_index_manifest.cpp` (append a block)

- [ ] **Step 1: Failing test — append to `tests/test_index_manifest.cpp` before the final PASSED:**

```cpp
/*
	remove_segment(): used by compaction to drop merged inputs
*/
char rm_template[] = "/tmp/ant_manifest_XXXXXX";
char *rm_dir = mkdtemp(rm_template);
CHECK(rm_dir != NULL);
ANT_index_manifest *rm = ANT_index_manifest::load(rm_dir);
rm->add_segment(rm->take_generation());		// 1
rm->add_segment(rm->take_generation());		// 2
rm->add_segment(rm->take_generation());		// 3
CHECK(rm->remove_segment(2) == 0);
CHECK(rm->segment_count() == 2);
CHECK(rm->get_segment(0) == 1);
CHECK(rm->get_segment(1) == 3);
CHECK(!rm->contains(2));
CHECK(rm->remove_segment(99) == 1);			// absent -> 1, no change
CHECK(rm->segment_count() == 2);
CHECK(rm->save() == 0);
ANT_index_manifest *rm2 = ANT_index_manifest::load(rm_dir);
CHECK(rm2->segment_count() == 2);
CHECK(rm2->contains(1) && rm2->contains(3) && !rm2->contains(2));
delete rm;
delete rm2;
```

- [ ] **Step 2:** `make test_index_manifest` → FAIL (no remove_segment).

- [ ] **Step 3: Implement**

`source/index_manifest.h` (public section):
```cpp
	long remove_segment(long long segment_generation);	// 0 = removed, 1 = not present; call save() to persist
```

`source/index_manifest.cpp`:
```cpp
/*
	ANT_INDEX_MANIFEST::REMOVE_SEGMENT()
	------------------------------------
	Order-preserving removal.  In-memory only -- the caller decides when to
	save() (compaction batches removals + the addition into one atomic save).
*/
long ANT_index_manifest::remove_segment(long long segment_generation)
{
long long which, shuffle;

for (which = 0; which < segments_used; which++)
	if (segments[which] == segment_generation)
		{
		for (shuffle = which; shuffle < segments_used - 1; shuffle++)
			segments[shuffle] = segments[shuffle + 1];
		segments_used--;
		return 0;
		}
return 1;
}
```

In `atire/atire_segment_index.h` private section:
```cpp
	void delete_segment_files(long long generation);	// best-effort unlink of seg_G.aspt / seg_G.del
```
In `atire/atire_segment_index.cpp`:
```cpp
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
}
```
(Match `segment_filename`'s actual signature in the file.)

- [ ] **Step 4:** `make test_index_manifest && ./bin/test_index_manifest` → PASSED; `make test_segment_index` still builds.

- [ ] **Step 5: Commit**

```bash
git add source/index_manifest.h source/index_manifest.cpp atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_index_manifest.cpp
git commit -m "feat: manifest remove_segment and segment file deletion helper"
```

---

### Task 5: `compact()` — crash-safe merge-and-swap with keymap remap

**Files:**
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp` (append `test_compact_basic`)

- [ ] **Step 1: Failing test — append and call from `main()`:**

```cpp
/*
	TEST_COMPACT_BASIC()
	--------------------
	Compact two segments with deletions into one; searches, counts, keymap
	(update/delete by key) and reopen must all be correct afterwards.
*/
static void test_compact_basic(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 8; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	if (i == 3)
		CHECK(index->flush() == 0);
	}
CHECK(index->flush() == 0);
CHECK(index->delete_document("doc-2") == 0);
CHECK(index->delete_document("doc-5") == 0);

long long inputs[2];
inputs[0] = 1;
inputs[1] = 2;
CHECK(index->compact(inputs, 2) == 0);

/*
	One disk segment now; 6 live docs; tombstone over-fetch overhead gone
*/
CHECK(index->get_document_count() == 6);
strcpy(query, "common");
CHECK(index->search(query, 100) == 6);
unique_term(letters, 2);
strcpy(query, letters);
CHECK(index->search(query, 10) == 0);

/*
	Keymap remapped: update + delete by key still hit the right documents
*/
unique_term(letters, 3);
sprintf(doc, "<DOC>common replacement%s</DOC>", letters);
CHECK(index->update_document("doc-3", doc) >= 0);
strcpy(query, letters);
CHECK(index->search(query, 10) == 1);		// only the replacement's copy of the unique term
CHECK(index->delete_document("doc-7") == 0);
CHECK(index->get_document_count() == 5);

/*
	Durable: reopen sees the compacted state (plus the unflushed update is
	lost, so doc-3 reverts to its compacted body -- relaxed durability)
*/
delete index;
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(reopened->get_document_count() == 5);	// 6 - doc-7 deleted (persisted) ... doc-3's update was unflushed: old copy tombstoned (persisted), new copy lost -> 4? THINK (see step notes)
delete reopened;
delete [] dir;
printf("test_compact_basic OK\n");
}
```

**Step-1 note for the implementer (resolve before running):** the final count assertion depends on established Phase 1 semantics: `update_document("doc-3", …)` tombstones the compacted copy IMMEDIATELY (persisted .del) while the replacement lives in the unflushed writer — after `delete index` without flush, the replacement is lost, so the reopened count is **4** (6 − doc-7 − doc-3's both-copies-gone) and doc-3 is unfindable. That is correct relaxed-durability behavior (matches `test_stale_keymap_reconciliation`). Set the assertion to 4 and add `CHECK(reopened->search(strcpy(query, "common"), 100) == 4);`. If you disagree after tracing, escalate rather than bend.

- [ ] **Step 2:** run → FAIL (`compact` undeclared).

- [ ] **Step 3: Implement.** In `atire/atire_segment_index.h` public section:

```cpp
	long compact(long long *input_generations, long long input_count);	// merge those segments into one; 0 on success
```

In `atire/atire_segment_index.cpp` (includes: `index_merge.h`):

```cpp
/*
	ATIRE_SEGMENT_INDEX::COMPACT()
	------------------------------
	Merge the given disk segments into one new segment and swap it in.
	Ordering is crash-safe at every boundary (see the Phase 2 design spec,
	docs/superpowers/specs/2026-07-06-compacting-merge-design.md section 3):
	output written fully before anything references it; the "compacting"
	marker makes a mid-swap crash trigger a full keymap rebuild at the next
	open(); input files are deleted last (leftovers are swept as orphans).
*/
long ATIRE_segment_index::compact(long long *input_generations, long long input_count)
{
char output_name[4096], marker_name[4096], filename_buffer[4096];
long long which, input, docid;

if (input_count < 1)
	return 1;

/*
	Resolve the inputs to open segments (all must exist and be distinct)
*/
segment **inputs = new segment *[input_count];
for (input = 0; input < input_count; input++)
	{
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
	Step 1: burn the output generation (manifest saved by contract)
*/
long long output_generation = manifest->take_generation();
if (manifest->save() != 0)
	{
	delete [] inputs;
	return 1;
	}
segment_filename(output_name, sizeof(output_name), output_generation, "aspt");

/*
	Step 2: merge
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
	Step 3: marker -- from here until removal, a crash makes the next open()
	rebuild the keymap from the segments
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
	Step 4: open the output and repoint the keymap at it.  Every document in
	the output is, by construction, the live copy of its key.
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
	Step 5: atomic manifest swap
*/
for (input = 0; input < input_count; input++)
	manifest->remove_segment(input_generations[input]);
manifest->add_segment(output_generation);
if (manifest->save() != 0)
	return 1;		/* marker stays: next open() rebuilds the keymap; inputs remain manifested on disk but doubled in memory -- degraded, documented */

/*
	Step 6: drop the inputs -- close engines, remove from segments[], delete files
*/
for (input = 0; input < input_count; input++)
	{
	for (which = 0; which < segment_count; which++)
		if (segments[which].generation == input_generations[input])
			{
			delete segments[which].engine;
			delete segments[which].tombstones;
			for (long long shuffle = which; shuffle < segment_count - 1; shuffle++)
				segments[shuffle] = segments[shuffle + 1];
			segment_count--;
			break;
			}
	delete_segment_files(input_generations[input]);
	}
remove(marker_name);
delete [] inputs;
return 0;
}
```

Implementer notes:
- `inputs[]` holds pointers into `segments[]` which step 6 reshuffles — that's why step 6 re-finds by generation instead of using the stale pointers. Keep it that way.
- The step-5 failure path is deliberately conservative: report the code's actual behavior in your report (the in-memory state has both inputs and output active until process exit; searches over-count. If you can cheaply make it cleaner — e.g. proceed to step 6's in-memory removal anyway since the on-disk manifest still lists inputs and the marker forces rebuild — reason it through and pick ONE documented behavior).
- `merge()` must not be given the writer — `compact` only ever sees disk segments (callers pass manifest generations).

- [ ] **Step 4:** `make test_segment_index && ./bin/test_segment_index` → all prior + `test_compact_basic OK` + PASSED. `make internal` exit 0.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat: crash-safe compact() with keymap remap and atomic manifest swap"
```

---

### Task 6: Marker-triggered keymap rebuild at open + crash-window tests

**Files:**
- Modify: `atire/atire_segment_index.cpp` (open())
- Test: `tests/test_segment_index.cpp` (append `test_compaction_crash_windows`)

- [ ] **Step 1: Failing test — append and call from `main()`:**

```cpp
/*
	TEST_COMPACTION_CRASH_WINDOWS()
	-------------------------------
	Construct the on-disk states a crash can leave at each compact() boundary
	and assert open() recovers每 one: no lost live docs, no resurrected dead
	docs, update/delete by key still work.
*/
static void test_compaction_crash_windows(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16], path[4096];
long long i;

/*
	Common fixture: two flushed segments (gens 1,2), doc-1 deleted
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 4; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	if (i == 1)
		CHECK(index->flush() == 0);
	}
CHECK(index->flush() == 0);
CHECK(index->delete_document("doc-1") == 0);
delete index;

/*
	Window A: crash after merge output written, before marker/manifest --
	an unmanifested seg file.  Simulate with a garbage orphan.
*/
snprintf(path, sizeof(path), "%s/seg_000042.aspt", dir);
FILE *fp = fopen(path, "wb");
fputs("partial merge output", fp);
fclose(fp);
ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->open(dir) == 0);
CHECK(access(path, F_OK) != 0);				// orphan swept
CHECK(a->get_document_count() == 3);
strcpy(query, "common");
CHECK(a->search(query, 10) == 3);
delete a;

/*
	Window B: crash after marker created (keymap possibly half-remapped).
	Simulate: plant the marker; scramble the keymap by pointing doc-0 at a
	nonsense generation (writing a raw A-record to the log).
*/
snprintf(path, sizeof(path), "%s/compacting", dir);
fp = fopen(path, "wb");
fclose(fp);
char keymap_log[4096];
snprintf(keymap_log, sizeof(keymap_log), "%s/keymap.log", dir);
fp = fopen(keymap_log, "ab");
fprintf(fp, "A\t77\t0\tdoc-0\n");
fclose(fp);
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(dir) == 0);
snprintf(path, sizeof(path), "%s/compacting", dir);
CHECK(access(path, F_OK) != 0);				// marker consumed
/*
	Rebuilt keymap points doc-0 back at its real copy: update works and
	tombstones the real doc, not generation 77
*/
unique_term(letters, 0);
sprintf(doc, "<DOC>common recovered%s</DOC>", letters);
CHECK(b->update_document("doc-0", doc) >= 0);
strcpy(query, letters);
CHECK(b->search(query, 10) == 1);
CHECK(b->get_document_count() == 3);
delete b;
delete [] dir;
printf("test_compaction_crash_windows OK\n");
}
```

(Fix the stray non-ASCII character in the banner comment if your editor flags it — type the comment fresh.)

- [ ] **Step 2:** run → FAIL at Window B (marker not consumed / stale entry survives).

- [ ] **Step 3: Implement** in `open()`, replacing the current keymap-consistency block (the `had_keymap_log`/`retain_generations`/`rebuild_keymap` sequence). New logic:

```cpp
/*
	Keymap consistency.  Three cases:
	1. "compacting" marker present: a compaction died mid-swap -- the log may
	   contain remap records for a segment that never became live (or misses
	   records for one that did).  Discard the log entirely and rebuild from
	   the segments.
	2. No keymap.log: rebuild from the segments (Phase 1 recovery).
	3. Normal: drop entries for generations the manifest doesn't know
	   (unflushed-writer case) via retain_generations().
*/
snprintf(marker_name, sizeof(marker_name), "%s/compacting", directory);
long compaction_died = access(marker_name, F_OK) == 0;
if (compaction_died)
	{
	char keymap_log_name[4096];
	snprintf(keymap_log_name, sizeof(keymap_log_name), "%s/keymap.log", directory);
	delete keymap;
	remove(keymap_log_name);
	keymap = ANT_index_keymap::load(directory);		// fresh, empty, log recreated
	}
/* ... existing append_segment loop and orphan sweep stay where they are ... */
if (compaction_died || (!had_keymap_log && segment_count > 0))
	{
	if (segment_count > 0 && rebuild_keymap() != 0)
		return 1;
	remove(marker_name);
	}
else
	{
	/* existing retain_generations call */
	}
```
Adapt to the function's real structure (the orphan sweep and `append_segment` loop must still run before `rebuild_keymap`; `had_keymap_log` is already captured before load). Note the orphan sweep also removes the dead compaction OUTPUT if it never got manifested — which is why the rebuild runs after the sweep, over manifested segments only.

- [ ] **Step 4:** `make test_segment_index && ./bin/test_segment_index` → all + `test_compaction_crash_windows OK` + PASSED.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat: compaction marker triggers keymap rebuild at open"
```

---

### Task 7: `maintain()` — tiered merge policy

**Files:**
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp` (append `test_maintain_policy`)

- [ ] **Step 1: Failing test — append and call from `main()`:**

```cpp
/*
	TEST_MAINTAIN_POLICY()
	----------------------
	Tier trigger: merge_factor segments in one size tier collapse to one.
	Tombstone trigger: a segment above the deletion ratio gets rewritten.
*/
static void test_maintain_policy(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16];
long long i, batch;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->set_merge_factor(3);

/*
	Three 4-doc segments (same size tier) -> tier trigger fires -> 1 segment
*/
for (batch = 0; batch < 3; batch++)
	{
	for (i = 0; i < 4; i++)
		{
		sprintf(key, "doc-%lld", batch * 4 + i);
		unique_term(letters, batch * 4 + i);
		sprintf(doc, "<DOC>common %s</DOC>", letters);
		CHECK(index->add_document(key, doc) >= 0);
		}
	CHECK(index->flush() == 0);
	}
CHECK(index->disk_segment_count() == 3);
CHECK(index->maintain() == 0);
CHECK(index->disk_segment_count() == 1);
CHECK(index->get_document_count() == 12);
strcpy(query, "common");
CHECK(index->search(query, 100) == 12);

/*
	Tombstone trigger: delete 7 of 12 (58% > default 25%) -> maintain
	rewrites the segment without them
*/
for (i = 0; i < 7; i++)
	{
	sprintf(key, "doc-%lld", i);
	CHECK(index->delete_document(key) == 0);
	}
CHECK(index->maintain() == 0);
CHECK(index->disk_segment_count() == 1);
CHECK(index->get_document_count() == 5);
strcpy(query, "common");
CHECK(index->search(query, 100) == 5);
/*
	The rewrite dropped the tombstones physically: no .del file remains
*/
long long only_generation = index->disk_segment_generation(0);
char del_name[4096];
snprintf(del_name, sizeof(del_name), "%s/seg_%06lld.del", dir, only_generation);
CHECK(access(del_name, F_OK) != 0);

/*
	Idempotent: nothing left to do
*/
CHECK(index->maintain() == 0);
CHECK(index->disk_segment_count() == 1);

delete index;
delete [] dir;
printf("test_maintain_policy OK\n");
}
```

- [ ] **Step 2:** run → FAIL (`set_merge_factor`/`disk_segment_count`/`maintain` undeclared).

- [ ] **Step 3: Implement.** Header additions (public):

```cpp
	long maintain(void);								// run the merge policy to quiescence; 0 = success
	void set_merge_factor(long segments_per_tier) { merge_factor = segments_per_tier; }
	void set_tombstone_compact_ratio(double ratio) { tombstone_compact_ratio = ratio; }
	void set_auto_maintain(long on) { auto_maintain = on; }
	long long disk_segment_count(void) { return segment_count; }
	long long disk_segment_generation(long long which) { return segments[which].generation; }
```
Private members: `long merge_factor;` (ctor: 10), `double tombstone_compact_ratio;` (ctor: 0.25), `long auto_maintain;` (ctor: 0).

Implementation:

```cpp
/*
	ATIRE_SEGMENT_INDEX::TIER_OF()
	------------------------------
	Size tier = number of decimal digits in the live-document count
	(1-9 -> tier 1, 10-99 -> tier 2, ...).
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
	Run the merge policy until no trigger fires (or the safety cap).
	Triggers, per the Phase 2 design spec section 4:
	1. any segment whose tombstone ratio exceeds tombstone_compact_ratio
	   joins the merge set (alone if nothing else fires);
	2. any size tier holding >= merge_factor segments merges entirely.
*/
long ATIRE_segment_index::maintain(void)
{
long long candidates[1024];
long long candidate_count, which, other;
long iteration;

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
		return 0;					// quiescent
	if (compact(candidates, candidate_count) != 0)
		return 1;
	}
return 0;						// safety cap: good enough, next maintain() continues
}
```

Wire auto-maintain into `flush()` (after `start_new_writer()` succeeds): `if (auto_maintain) maintain();` — best-effort, failure not propagated from flush (comment it).

Design check while implementing: `compact()` with a SINGLE input (tombstone trigger alone) must work — it is just a 1-way merge; confirm `ANT_index_merger` handles `engine_count == 1` (it should — the N-way loops degrade naturally; if Task 2 special-cased N ≥ 2, fix it here and note it).

- [ ] **Step 4:** `make test_segment_index && ./bin/test_segment_index` → all + `test_maintain_policy OK` + PASSED. `make internal` exit 0.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat: maintain() tiered merge policy with tombstone-ratio trigger"
```

---

### Task 8: Full equivalence, composability, and regression sweep

**Files:**
- Test: `tests/test_segment_index.cpp` (append `test_compaction_equivalence`)

- [ ] **Step 1: The test — append and call from `main()`:**

```cpp
/*
	TEST_COMPACTION_EQUIVALENCE()
	-----------------------------
	The parent spec's section 5 equivalence, now at postings level: a messy
	history compacted to one segment must match a one-shot index of the
	surviving collection on document count, per-term df AND cf -- not just
	visible search results.  Also proves merge-of-merges composability.
*/
static void test_compaction_equivalence(void)
{
char *dir_messy = make_index_dir();
char *dir_oneshot = make_index_dir();
char query[64], key[64], doc[256], letters[16];
long long i;

/*
	Messy history: 20 docs over several flushes, evens 0-8 updated,
	15-19 deleted; two rounds of compaction (composability).
*/
ATIRE_segment_index *messy = new ATIRE_segment_index();
CHECK(messy->open(dir_messy) == 0);
messy->set_flush_threshold(6);
messy->set_merge_factor(2);
for (i = 0; i < 20; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common body%s</DOC>", letters);
	CHECK(messy->add_document(key, doc) >= 0);
	}
for (i = 0; i < 10; i += 2)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common revised%s</DOC>", letters);
	CHECK(messy->update_document(key, doc) >= 0);
	}
for (i = 15; i < 20; i++)
	{
	sprintf(key, "doc-%lld", i);
	CHECK(messy->delete_document(key) == 0);
	}
CHECK(messy->flush() == 0);
CHECK(messy->maintain() == 0);				// round 1
CHECK(messy->maintain() == 0);				// round 2: idempotent / composes
CHECK(messy->disk_segment_count() == 1);

/*
	One-shot: the surviving logical collection, single segment
*/
ATIRE_segment_index *oneshot = new ATIRE_segment_index();
CHECK(oneshot->open(dir_oneshot) == 0);
for (i = 0; i < 15; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	if (i < 10 && i % 2 == 0)
		sprintf(doc, "<DOC>common revised%s</DOC>", letters);
	else
		sprintf(doc, "<DOC>common body%s</DOC>", letters);
	CHECK(oneshot->add_document(key, doc) >= 0);
	}
CHECK(oneshot->flush() == 0);

/*
	Postings-level comparison via the underlying engines
*/
CHECK(messy->get_document_count() == oneshot->get_document_count());

/* both have exactly one disk segment; compare per-term stats */
ANT_search_engine_btree_leaf messy_leaf, oneshot_leaf;
/* implementer: expose each index's single segment engine for the test --
   simplest is a test-only accessor already present: disk_segment_generation
   gives the gen; add
       ANT_search_engine *disk_segment_engine(long long which)
   returning segments[which].engine->get_search_engine() (one-liner, header) */
ANT_search_engine *m = messy->disk_segment_engine(0);
ANT_search_engine *o = oneshot->disk_segment_engine(0);

CHECK(m->get_postings_details((char *)"common", &messy_leaf) != NULL);
CHECK(o->get_postings_details((char *)"common", &oneshot_leaf) != NULL);
CHECK(messy_leaf.local_document_frequency == oneshot_leaf.local_document_frequency);
CHECK(messy_leaf.local_collection_frequency == oneshot_leaf.local_collection_frequency);

for (i = 0; i < 15; i++)
	{
	unique_term(letters, i);
	char probe[64];
	if (i < 10 && i % 2 == 0)
		sprintf(probe, "revised%s", letters);
	else
		sprintf(probe, "body%s", letters);
	CHECK(m->get_postings_details(probe, &messy_leaf) != NULL);
	CHECK(o->get_postings_details(probe, &oneshot_leaf) != NULL);
	CHECK(messy_leaf.local_document_frequency == oneshot_leaf.local_document_frequency);
	CHECK(messy_leaf.local_collection_frequency == oneshot_leaf.local_collection_frequency);
	}
/* dead terms absent from BOTH */
for (i = 15; i < 20; i++)
	{
	unique_term(letters, i);
	char probe[64];
	sprintf(probe, "body%s", letters);
	CHECK(m->get_postings_details(probe, &messy_leaf) == NULL);
	CHECK(o->get_postings_details(probe, &oneshot_leaf) == NULL);
	}

/*
	Search membership identical for every surviving doc
*/
for (i = 0; i < 15; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	if (i < 10 && i % 2 == 0)
		sprintf(query, "revised%s", letters);
	else
		sprintf(query, "body%s", letters);
	CHECK(messy->search(query, 10) == 1);
	CHECK(strcmp(messy->get_hit(0)->filename, key) == 0);
	}

delete messy;
delete oneshot;
delete [] dir_messy;
delete [] dir_oneshot;
printf("test_compaction_equivalence OK\n");
}
```

Add the one-line `disk_segment_engine` accessor to `atire/atire_segment_index.h` as described in the comment (include `search_engine.h` forward-decl as needed).

- [ ] **Step 2:** run to green. An equivalence failure here is a REAL merger bug (most likely cf accounting or `~length` filtering) — root-cause and fix in `source/index_merge.cpp`, don't weaken assertions.

- [ ] **Step 3: Full regression sweep, paste results:** all five test binaries PASSED (`test_segment_index` now with all functions), `./bin/test_index_merge` PASSED, `make internal` exit 0, `git status --short` clean after commit.

- [ ] **Step 4: Commit**

```bash
git add atire/atire_segment_index.h tests/test_segment_index.cpp
git commit -m "test: postings-level compaction equivalence and composability"
```

---

## Self-review record

- **Spec coverage:** §1 decisions (in-process, purpose-built merger, rescan remap, blocking) → Tasks 2/5/7; §2 merger mechanics incl. quantum alignment, df/cf recompute, empty-term drop, special variables, failure cleanup → Tasks 2/3; §3 compact() six-step ordering + marker + crash windows → Tasks 5/6; §4 maintain()/tiers/tombstone ratio/auto_maintain/setters/defaults → Task 7; §5 testing: full equivalence + composability (Task 8), crash windows (Task 6), policy (Task 7), post-compaction mutations (Task 5), scoring sanity (implicit in equivalence's df/cf + search assertions — impacts preserved verbatim per §2). §6 out-of-scope respected (no atire_merge changes, no threading, no binding).
- **Known adaptation risks flagged in-task:** `~trimpoint` value for unpruned segments (Task 2 note — verify against serialise), `SPECIAL_COMPRESSION` df≤2 re-entry after filtering (Task 2 item 11), single-input merge (Task 7 design check), the step-5 failure semantics decision (Task 5 note), reopened-count semantics in `test_compact_basic` (Task 5 step-1 note).
- **Type consistency:** `ANT_docid_renumberer` API used identically in Tasks 1/2; `merge(engines, tombstones, count, filename)` signature consistent across Tasks 2/3/5; `compact(long long*, long long)` consistent in Tasks 5/6/7; new accessors (`disk_segment_count/generation/engine`, `set_merge_factor`, `set_tombstone_compact_ratio`, `set_auto_maintain`) declared in Task 7 (engine accessor in Task 8) before use.

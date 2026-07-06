# Segment Index File Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the 2197-line `atire/atire_segment_index.cpp` into five focused sibling `.cpp` files with zero behavior change.

**Architecture:** One `ATIRE_segment_index` class, header unchanged. Move method *definitions* (verbatim, bodies untouched) into feature-grouped sibling files that all share `atire_segment_index.h`. Each file-scope `static` helper travels with the methods that call it (all are already group-local, verified). GNUmakefile auto-discovers `atire/*.cpp`, so no build wiring is needed.

**Tech Stack:** C++, GNU make, the existing C++ test suites (`bin/test_*`) + the Node addon JS tests.

---

## Ground rules for every task

- This is **behavior-preserving code motion**. Never edit a moved method's body, signature, comments, or the order of statements. Cut and paste verbatim.
- Identify methods by their signature, not by line number — line numbers shift as earlier tasks remove code. The "current line ~N" hints are as of the pre-split file and are approximate.
- **No header dependency tracking in this build.** Every rebuild after moving code MUST be `rm -f obj/*.o && make all && make tests`, or stale objects will mask errors. (Strictly, only `.cpp` changes here, but purge anyway to be safe — it is cheap insurance.)
- Every new file begins with this exact prologue (header comment + the full common include block copied from the original file — over-inclusion is deliberate: it guarantees no missing-symbol / incomplete-type surprises during the move; include pruning is explicitly out of scope):

```cpp
/*
	ATIRE_SEGMENT_INDEX_<UPPER>.CPP
	-------------------------------
	<one-line description>.  Part of ATIRE_segment_index, whose implementation is
	split across atire_segment_index*.cpp by feature (see
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
```

- **The full verification command block** (run at Step "verify" of every task):

```bash
cd /data/tyolab/code/antelope
rm -f obj/*.o
make all 2>&1 | tail -5
make tests 2>&1 | tail -5
# all eight C++ suites must pass:
for t in test_segment_index test_index_keymap test_index_manifest test_index_tombstones \
         test_index_merge test_vector_store test_wal test_memory_engine_ownership; do
  printf "%-28s " "$t:"; ./bin/$t >/tmp/split_$t.out 2>&1 && tail -1 /tmp/split_$t.out || { echo FAILED; tail -8 /tmp/split_$t.out; }
done
# addon relink + JS tests:
make engine_lib 2>&1 | tail -2
( cd nodejs && npm run build:segment 2>&1 | tail -2 && npm run test:segment 2>&1 | tail -6 )
```

Expected every time: `make all`/`make tests` succeed with **no duplicate-symbol and no undefined-symbol** link errors; `test_segment_index` prints `PASSED` (34 functions); the other seven suites print `PASSED`; JS reports `pass 11  fail 0`.

---

## Task 1: Extract compaction into `atire_segment_index_compaction.cpp`

Compaction is the cleanest cut — the three definitions are contiguous (`compact`, then the `tier_of` static, then `maintain`, currently ~lines 1215–1551).

**Files:**
- Create: `atire/atire_segment_index_compaction.cpp`
- Modify: `atire/atire_segment_index.cpp` (remove the three definitions)

- [ ] **Step 1: Create the new file**

Write `atire/atire_segment_index_compaction.cpp` with the standard prologue (description: "Compaction and tiered maintenance."), then paste — verbatim — these three definitions in this order:
- `static long tier_of(long long live_documents)` (the maintain() tier helper, ~line 1433)
- `long ATIRE_segment_index::compact(long long *input_generations, long long input_count)` (~line 1215)
- `long ATIRE_segment_index::maintain(void)` (~line 1469)

Put `tier_of` **above** `maintain` (its only caller) so no forward declaration is needed.

- [ ] **Step 2: Remove them from the original**

Delete those same three definitions (`compact`, `tier_of`, `maintain`) from `atire/atire_segment_index.cpp`. Delete nothing else.

- [ ] **Step 3: Verify**

Run the full verification command block. Expected: all green; link succeeds (proves `compact`/`maintain` are found by their callers — `flush`, the coordinator — and that `tier_of` is not referenced anywhere else).

- [ ] **Step 4: Commit**

```bash
git add atire/atire_segment_index_compaction.cpp atire/atire_segment_index.cpp
git commit -m "refactor: extract compaction into atire_segment_index_compaction.cpp"
```

---

## Task 2: Extract durability + global-stats into `atire_segment_index_durability.cpp`

Doing this before the vector task removes the WAL-setter methods that are currently interleaved among the vector methods in the ~196–334 region, leaving a cleaner vector cut.

**Files:**
- Create: `atire/atire_segment_index_durability.cpp`
- Modify: `atire/atire_segment_index.cpp`

- [ ] **Step 1: Create the new file**

Write `atire/atire_segment_index_durability.cpp` with the standard prologue (description: "WAL durable-mode setters and global ranking-statistics push."), then paste — verbatim — these five definitions:
- `long ATIRE_segment_index::set_durable(long on)` (~line 236)
- `void ATIRE_segment_index::set_wal_fsync(long on)` (~line 251)
- `long ATIRE_segment_index::wal_healthy(void)` (~line 265)
- `void ATIRE_segment_index::refresh_global_statistics(void)` (~line 1659)
- `void ATIRE_segment_index::set_global_stats(long on)` (~line 1720)

- [ ] **Step 2: Remove them from the original**

Delete those five definitions from `atire/atire_segment_index.cpp`.

- [ ] **Step 3: Verify**

Run the full verification command block. Expected: all green. This link proves the cross-file callers resolve: `refresh_global_statistics` is called from `open`/`flush`/`rebuild_writer_engine` (spine) and `compact`/`maintain` (compaction, already moved in Task 1).

- [ ] **Step 4: Commit**

```bash
git add atire/atire_segment_index_durability.cpp atire/atire_segment_index.cpp
git commit -m "refactor: extract WAL/global-stats setters into atire_segment_index_durability.cpp"
```

---

## Task 3: Extract vectors into `atire_segment_index_vector.cpp`

All vector state management and vector/hybrid search, plus the two vector comparators and the fused-candidate struct.

**Files:**
- Create: `atire/atire_segment_index_vector.cpp`
- Modify: `atire/atire_segment_index.cpp`

- [ ] **Step 1: Create the new file**

Write `atire/atire_segment_index_vector.cpp` with the standard prologue (description: "Per-document vectors, vector search, and hybrid (RRF) search."), then paste — verbatim — these definitions. Keep each `static` helper / struct **above its first caller** to avoid forward declarations:
- `struct ANT_fused_candidate` (~line 2035) — put near the top of the file
- `void ATIRE_segment_index::reset_writer_vectors(void)` (~line 196)
- `long ATIRE_segment_index::set_vector_config(long long dimension, long metric)` (~line 213)
- `long ATIRE_segment_index::load_vector_config(void)` (~line 277)
- `long ATIRE_segment_index::save_vector_config(void)` (~line 303)
- `long ATIRE_segment_index::writer_vector_append(long long docid, const float *vector_or_null)` (~line 334)
- `static int vector_candidate_compare(const void *a, const void *b)` (~line 1966)
- `long long ATIRE_segment_index::vector_candidates(const float *query, long long top_k, ANT_vector_candidate *best)` (~line 1895)
- `long long ATIRE_segment_index::search_vector(const float *query, long long top_k)` (~line 1988)
- `static int ANT_fused_candidate_compare(const void *a, const void *b)` (~line 2047)
- `long long ATIRE_segment_index::search_hybrid(char *query_text, const float *query_vector, long long top_k)` (~line 2076)

Ordering within the file: `struct ANT_fused_candidate` and both `static` comparators must precede the methods that use them (`vector_candidate_compare` before `vector_candidates`/`search_vector`; `ANT_fused_candidate_compare` + the struct before `search_hybrid`).

- [ ] **Step 2: Remove them from the original**

Delete those definitions (the eight methods, the struct, and the two statics) from `atire/atire_segment_index.cpp`.

- [ ] **Step 3: Verify**

Run the full verification command block. Expected: all green — especially `test_segment_index`'s vector tests (`test_vector_config_and_add`, `test_vector_search_nrt_and_persistence`, `test_vector_compaction_equivalence`, `test_hybrid_search_rrf`, `test_vector_metrics_and_compat`). The link proves spine→vector calls resolve (`add_document_core`→`writer_vector_append`; ctor/`start_new_writer`/`flush`→`reset_writer_vectors`; `open`/`flush`→`load_vector_config`/`save_vector_config`) and vector→search calls resolve (`search_hybrid`→`search_one_segment`/`resolve_hit_filename`/`reset_results`/`append_result`, still in the spine until Task 4).

- [ ] **Step 4: Commit**

```bash
git add atire/atire_segment_index_vector.cpp atire/atire_segment_index.cpp
git commit -m "refactor: extract vector + hybrid search into atire_segment_index_vector.cpp"
```

---

## Task 4: Extract lexical search into `atire_segment_index_search.cpp`

Lexical search, the shared results plumbing, the hit comparator, and hit-filename resolution.

**Files:**
- Create: `atire/atire_segment_index_search.cpp`
- Modify: `atire/atire_segment_index.cpp`

- [ ] **Step 1: Create the new file**

Write `atire/atire_segment_index_search.cpp` with the standard prologue (description: "Lexical search, results buffer, and hit-filename resolution."), then paste — verbatim — these definitions, keeping the `static` comparator above its caller:
- `void ATIRE_segment_index::reset_results(void)` (~line 1735)
- `ATIRE_segment_index::hit *ATIRE_segment_index::append_result(void)` (~line 1751)
- `static int ATIRE_segment_index_hit_cmp(const void *a, const void *b)` (~line 1837)
- `void ATIRE_segment_index::search_one_segment(ATIRE_API *engine, ANT_index_tombstones *tombstones, long long generation, char *query, long long top_k, long use_filename_index)` (~line 1777)
- `long long ATIRE_segment_index::search(char *query, long long top_k)` (~line 1857)
- `char *ATIRE_segment_index::resolve_hit_filename(long long generation, long long docid, char *buffer, long long buffer_size)` (~line 1939)

Put `ATIRE_segment_index_hit_cmp` above `search` (its `qsort` caller).

- [ ] **Step 2: Remove them from the original**

Delete those definitions from `atire/atire_segment_index.cpp`.

- [ ] **Step 3: Verify**

Run the full verification command block. Expected: all green. The link proves the search methods resolve for their cross-file callers now living in `atire_segment_index_vector.cpp` (`search_hybrid` uses `search_one_segment`, `resolve_hit_filename`, `reset_results`, `append_result`) and the spine (`get_document_count` and the coordinator flow are unaffected).

- [ ] **Step 4: Commit**

```bash
git add atire/atire_segment_index_search.cpp atire/atire_segment_index.cpp
git commit -m "refactor: extract lexical search into atire_segment_index_search.cpp"
```

---

## Task 5: Tidy the spine and annotate the header

After Tasks 1–4, `atire/atire_segment_index.cpp` holds only the lifecycle/write-path spine (~900 lines). Final polish only — no code motion.

**Files:**
- Modify: `atire/atire_segment_index.cpp` (top comment only)
- Modify: `atire/atire_segment_index.h` (one navigation note)

- [ ] **Step 1: Update the spine file's top comment**

In `atire/atire_segment_index.cpp`, extend the existing top comment block to note the split, e.g. add a line:

```
	The implementation is split by feature across atire_segment_index*.cpp:
	this file holds the lifecycle + write path (open/add/update/delete/flush/
	compact-driver plumbing); see _compaction, _vector, _search, _durability
	for the rest.
```

Do **not** prune the spine's includes (over-inclusion is fine and pruning risks a missing-symbol surprise; explicitly out of scope per the design).

- [ ] **Step 2: Add the header navigation note**

In `atire/atire_segment_index.h`, add one line to the top comment block (below the existing description):

```
	Implementation is split across atire_segment_index*.cpp by feature
	(core write-path here-adjacent; compaction, vector, search, durability
	in their own files).
```

No declarations change.

- [ ] **Step 3: Verify**

Run the full verification command block. Expected: all green (this task only touches comments, so a clean pass confirms nothing was disturbed).

- [ ] **Step 4: Confirm the size win**

```bash
wc -l atire/atire_segment_index*.cpp
```
Expected: five files; the largest (`atire_segment_index.cpp`) is roughly 850–950 lines, down from 2197; each other file is under ~500.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.cpp atire/atire_segment_index.h
git commit -m "refactor: annotate segment-index split; spine is now write-path only"
```

---

## Final verification (after all tasks)

Run the full verification command block one last time from a clean state, and confirm the git log shows five focused refactor commits. There should be no `.o`/binary artifacts staged. The diff of the whole series, viewed with `git diff --stat <before>..HEAD`, should show only file creations + deletions of the moved regions in `atire_segment_index.cpp` and the two comment edits — no changes inside any moved method body (spot-check with `git log -p` on one moved method to confirm it moved unchanged).

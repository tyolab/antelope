# Splitting atire_segment_index.cpp — Design

**Date:** 2026-07-06
**Status:** Approved (design), pending plan
**Goal:** Break the 2197-line `atire/atire_segment_index.cpp` into focused, single-
responsibility source files without changing any behavior.

---

## 1. Problem

`ATIRE_segment_index` grew by accretion across four shipped features (Phase 1 core
write-path, Phase 2 compaction, Vector V1, Lexical Phase 3 WAL/global-stats/keymap
compaction). The implementation file is now 2197 lines holding ~40 methods spanning five
distinct concerns. It no longer fits in a single reading/context window, and unrelated
features share one file.

## 2. Chosen strategy: mechanical partition (Approach A)

Keep the **single `ATIRE_segment_index` class exactly as it is** — same header, same
members, same method declarations, same behavior. Move only the method *definitions* into
sibling `.cpp` files, each qualifying its methods with `ATIRE_segment_index::` and sharing
the unchanged `atire_segment_index.h`. This is pure, compiler-verified code motion.

**Rejected alternatives:**
- **B — extract collaborator classes** (`ANT_segment_compactor`, `ANT_segment_vector_index`,
  …). More architecturally "pure" and matches the repo's one-class-per-file idiom, but the
  class's state (writer, segments, keymap, tombstones, WAL, vector buffers) is genuinely
  interwoven across crash-safe recovery paths — the extraction seams would be forced and
  carry real behavior risk on heavily-tested code. Not worth it for the stated goal.
- **C — section banners only.** Doesn't address the size/context problem.

## 3. File boundaries

One class, five files, unchanged header.

| File | Methods | Feature-local statics |
|---|---|---|
| `atire_segment_index.cpp` (spine, ~900) | ctor, dtor, `segment_filename`, `delete_segment_files`, `start_new_writer`, `rebuild_keymap`, `open` (incl. inline WAL replay + recovery), `add_document_core`, `add_document`×2, `tombstone`, `update_document`×2, `delete_document`, `flush`, `append_segment`, `rebuild_writer_engine`, `get_document_count`, `disk_segment_engine`, `writer_memory_index_for_test` | — |
| `atire_segment_index_compaction.cpp` (~340) | `compact`, `maintain` | `tier_of` |
| `atire_segment_index_vector.cpp` (~470) | `reset_writer_vectors`, `set_vector_config`, `load_vector_config`, `save_vector_config`, `writer_vector_append`, `vector_candidates`, `search_vector`, `search_hybrid` | `vector_candidate_compare`, `ANT_fused_candidate_compare`, `struct ANT_fused_candidate` |
| `atire_segment_index_search.cpp` (~250) | `reset_results`, `append_result`, `search_one_segment`, `search`, `resolve_hit_filename` | `ATIRE_segment_index_hit_cmp` |
| `atire_segment_index_durability.cpp` (~120) | `set_durable`, `set_wal_fsync`, `wal_healthy`, `refresh_global_statistics`, `set_global_stats` | — |

## 4. Why mechanical motion is correct here

1. **No file-scope static helper crosses a boundary.** Each of the four `static` helpers
   (`tier_of`, `ATIRE_segment_index_hit_cmp`, `vector_candidate_compare`,
   `ANT_fused_candidate_compare`) is called only from methods within its own group, so it
   moves with its users and keeps `static` (internal) linkage. `struct ANT_fused_candidate`
   is used only by hybrid search and moves with it. No symbol becomes newly extern.
2. **All cross-file references are ordinary method calls** on the one shared class, resolved
   at link time. Examples that cross the new boundaries — all fine:
   - spine `add_document_core` → vector `writer_vector_append`
   - spine ctor/`start_new_writer`/`flush` → vector `reset_writer_vectors`
   - spine `open`/`flush` → vector `load_vector_config`/`save_vector_config`
   - vector `search_hybrid` → search `search_one_segment`, `resolve_hit_filename`,
     `reset_results`, `append_result`
   - compaction `compact`/`maintain` → spine `append_segment`, `delete_segment_files`,
     durability `refresh_global_statistics`
   - spine `open`/`flush`/`rebuild_writer_engine` + compaction `compact` →
     durability `refresh_global_statistics`
3. **Build and consumers need no wiring.** `GNUmakefile` discovers sources via
   `$(shell ls atire/*.cpp source/*.cpp)`, so the new files compile automatically; the
   `engine_lib` archive (`SOURCES_OBJECTS`) and the Node addon that links it inherit them
   with no edit.

## 5. Include strategy

Each new file starts with its own header comment block and includes only what its group
needs, plus the common core:
- All: `atire_segment_index.h`, `atire_api.h`, `indexer.h`.
- `_compaction.cpp`: `index_merge.h`, `index_manifest.h`, `index_tombstones.h`.
- `_vector.cpp`: `vector_store.h`.
- `_search.cpp`: `search_engine.h`, `search_engine_result.h`, `search_engine_accumulator.h`.
- `_durability.cpp`: `wal.h`, `search_engine.h` (global-stats setter).
- Spine keeps the full recovery/lifecycle include set it needs.

The header (`atire_segment_index.h`) is unchanged except for a one-line navigation note in
its top comment ("implementation split across atire_segment_index*.cpp by feature").

## 6. Execution: one feature file at a time, green between each

Extract in this order, rebuilding and running the full test surface after **each** step so
any breakage is isolated to the file just moved:
1. `_compaction.cpp` (move `compact`, `maintain`, `tier_of`).
2. `_vector.cpp` (move the eight vector methods + two statics + struct).
3. `_search.cpp` (move the five search/results methods + one static).
4. `_durability.cpp` (move the five WAL/global-stats setters).
5. The spine is whatever remains; tidy its includes and add the header nav note.

Per step: `rm -f obj/*.o && make all && make tests`, run all eight C++ suites, then
`make engine_lib`, rebuild the addon, and run the 11 JS binding tests.

## 7. Testing

No new tests. This is behavior-preserving code motion, so the existing suites are the
correctness proof. Success criteria per step and at the end:
- Clean compile and link — **no duplicate-symbol and no missing-symbol errors** (the two
  failure modes a bad partition would produce).
- All eight C++ suites pass with identical results to pre-split
  (`test_segment_index` — 34 functions — plus keymap/manifest/tombstones/merge/vector_store/
  wal/memory_engine_ownership).
- All 11 JS binding tests pass after the addon relinks the regenerated archive.

## 8. Out of scope

- Any behavior change, signature change, or member reorganization.
- Approach-B collaborator-class extraction (revisit only if true encapsulation is later
  wanted).
- Splitting the header or touching unrelated large files.

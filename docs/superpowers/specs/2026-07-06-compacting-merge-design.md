# Compacting Merge (Incremental Index Phase 2) — Design

**Date:** 2026-07-06
**Status:** Draft for review
**Parent spec:** `2026-07-05-incremental-index-design.md` §2.6 (approved). This document
pins the decisions §2.6 left open and adjusts them to what Phase 1 actually shipped.

**Problem:** Phase 1 segments only accumulate. Tombstoned documents occupy postings and
inflate the search over-fetch forever; many small segments mean many per-segment engine
opens and per-segment ranking-stat drift. Compaction merges segments, physically dropping
deleted documents and renumbering survivors.

---

## 1. Decisions pinned (vs. parent §2.6)

| Open question | Decision | Rationale |
|---|---|---|
| Execution model | **In-process, synchronous `maintain()`** | Phase 1 is single-threaded by design; a background thread would force locking onto every structure. A thin CLI wrapper can be added later for cron use — same library code. |
| Merge implementation | **New purpose-built `ANT_index_merger` in `source/`; legacy `atire_merge` untouched** | `merge_index()` calls `exit()` on errors and relies on globals — unusable in-process. Our segments are a constrained subset (non-quantized, unpruned, single stemmer config, no stored document text), so a dedicated merger is smaller and safer than generalizing the 1200-line tool. |
| Keymap remap | **No remap-table file.** After the merge, append keymap A-records by scanning the output segment's filename index (docid ascending) | Every document in the merge output is, by construction, the keymap's live copy for its key (tombstoned copies are dropped by the merge). An unconditional `add(filename, out_gen, docid)` per output doc is a correct remap and reuses the Phase 1 rebuild pattern. |
| Concurrent searches during compaction | **None — `maintain()` blocks** | Single-threaded consumer (Node.js binding). Segment swap happens between searches by construction. |

## 2. `ANT_index_merger` (`source/index_merge.h/.cpp`)

**Inputs:** N open `ANT_search_engine` instances (the segments' engines, already open in
`ATIRE_segment_index`), their `ANT_index_tombstones`, and the output `.aspt` path.
**Output:** a standard non-quantized `.aspt` segment containing exactly the live
documents, renumbered densely; returns 0 on success, nonzero on any failure (no `exit()`,
no partial output left behind — the output file is deleted on failure).

Mechanics (adapted from `atire_merge`'s proven structure, specialized to our format):

1. **Docid renumbering table.** Per input segment, a prefix-sum array over live
   (non-tombstoned) docids: `new_docid(seg, old) = live_before(seg, old) + sum(live(earlier segs))`.
   Built once up front from the tombstone bitmaps; O(total docs).
2. **Term walk.** One `ANT_btree_iterator` per input; advance all iterators in
   lexicographic lock-step (the same N-way string merge `merge_index()` uses). For each
   term present in ≥1 input:
   - decode each input's postings (impact-ordered, IMPACT_HEADER layout — decompress via
     `ANT_compression_factory`),
   - drop tombstoned docids, map survivors through the renumbering table,
   - merge across inputs by aligning impact quanta on equal impact values (as
     `merge_index()` does); within an aligned quantum the inputs' docid lists concatenate
     (disjoint renumbered ranges) and are delta re-encoded — impacts are preserved
     verbatim, no score recomputation,
   - recompute document frequency / collection frequency from what survived,
   - if nothing survived, the term is dropped entirely,
   - re-encode and write postings + impact header to the output.
3. **Special variables rebuilt, not copied blindly:** `~length` vectors, document length
   sums, `~documentlongest`, document count, and the filename index — all filtered to live
   docs and renumbered. Filenames come from each input's filename index
   (`get_document_filename`), written in new-docid order.
4. **Vocabulary/B-tree serialization** reuses the same node-writing layout the memory
   index and `atire_merge` produce (this is the one place code is adapted rather than
   invented; the plan will specify exactly which functions).

Constraints honored: inputs must be non-quantized (they always are — flush guarantees it);
identical stemmer configuration (they share one writer config); output is itself a valid
Phase 1 segment, so compaction composes (merge of merges works).

## 3. Compaction driver (`ATIRE_segment_index::compact`)

`long compact(long long *input_generations, long long input_count)` — merges the named
segments into one new segment and swaps it in. Ordering, with the crash consequence at
each boundary:

| Step | Crash after this step leaves |
|---|---|
| 1. `take_generation()` for the output + `manifest->save()` | nothing (generation number burned) |
| 2. `ANT_index_merger` writes `seg_OUT.aspt` completely | unmanifested output → orphan-swept at next `open()`; inputs intact |
| 3. create marker file `<dir>/compacting` | same as above, plus next `open()` does a full keymap rebuild (safe, correct) |
| 4. append keymap A-records for every output doc (scan output filename index) | keymap points at an unmanifested output; marker forces rebuild at next open → consistent |
| 5. atomic manifest swap: inputs removed, output added, one `save()` | index is fully consistent on disk; stale keymap-input entries were already overwritten in step 4 |
| 6. remove marker; close input engines; delete input files (`.aspt`/`.del`) | leftover input files are unmanifested → orphan-swept at next open |

In-memory swap (same step 5–6 window): input entries removed from `segments[]`, output
appended via `append_segment()`. The output starts with no `.del` (all dead docs were
dropped); the inputs' tombstone objects are freed with their segments.

**`open()` change:** if the `compacting` marker exists, delete `keymap.log`, load an empty
keymap, and run `rebuild_keymap()` (Phase 1 machinery) instead of `retain_generations()`;
then remove the marker. The marker discriminates "compaction died mid-swap" (needs full
rebuild — stale entries may shadow live documents) from the everyday "unflushed writer
lost" case (cheap `retain_generations` suffices).

Failure handling: any step failing → `compact()` returns nonzero, deletes the partial
output and the marker if not yet past step 5, and leaves the index exactly as it was
(inputs still manifested, still open, still searchable). After step 5 there is no failure
path that loses data — steps 6's deletions are best-effort.

## 4. `maintain()` and the tiered policy

```
long maintain(void)      // run the merge policy until no trigger fires; 0 = success
void set_merge_factor(long segments_per_tier)      // default 10
void set_tombstone_compact_ratio(double ratio)     // default 0.25
```

Policy loop (over **disk** segments only; the live writer is never merged):
1. **Tombstone trigger:** any segment with `tombstones->count() / document_count > ratio`
   joins the next merge (or is compacted alone if no tier fires — rewriting one segment
   without its dead docs is still a win).
2. **Tier trigger:** segments are bucketed by live-doc count into decades
   (1–9, 10–99, 100–999, …). Any bucket holding ≥ `merge_factor` segments has its members
   merged into one.
3. After each `compact()`, re-evaluate; stop when neither trigger fires or after a safety
   cap of 10 iterations.

`maintain()` is also called opportunistically after `flush()` when the caller opts in via
`set_auto_maintain(long on)` (default off — Phase 1 behavior unchanged unless requested).

## 5. Testing

- **Full equivalence (parent spec §5, now achievable):** build a messy history
  (adds across several flushes, updates, deletes), `maintain()` down to one segment, and
  compare against a one-shot index of the surviving collection: identical search
  membership for every probe term, identical `get_document_count`, and identical per-term
  document frequency / collection frequency (via `get_postings_details`) — postings-level
  equivalence, not just visible-results equivalence.
- **Composability:** merge of merges (three rounds of flush+compact) stays correct.
- **Crash windows:** simulate steps 2–6 crashes by constructing the on-disk states
  directly (orphan output; marker + remapped keymap + old manifest; marker + new
  manifest); reopen and assert consistency (no lost live docs, no resurrected dead docs,
  update/delete by key still work).
- **Policy:** unit-test the trigger logic (tier counts, tombstone ratio) with synthetic
  segment inventories; assert `maintain()` convergence and the iteration cap.
- **Post-compaction mutations:** update and delete documents whose entries were remapped;
  verify against reopen.
- **Scoring sanity:** impact-ordered searches on merged segments return the same ranking
  as pre-merge for single-segment queries (impacts are preserved verbatim per doc).

## 6. Out of scope (unchanged from parent spec)

- Background/concurrent compaction (needs the Phase 3+ locking story).
- WAL durability, global ranking stats (Phase 3).
- Changes to the legacy `atire_merge` tool.
- Node.js binding exposure (tracked separately with the rest of the binding work).

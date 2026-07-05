# Incremental Index Update & Growth — Design

**Date:** 2026-07-05
**Status:** Draft for review
**Problem:** Antelope/ATIRE indexes are immutable, built in one shot. There is no way to
update a document or add new documents to an existing index short of a full rebuild;
`atire_merge` can combine complete indexes but knows nothing about deletions or duplicates.

**Chosen approach:** Segment-based index with tombstones (Lucene-style). One architecture
covers all freshness profiles — near-real-time, batched, and nightly — purely through
configuration (flush thresholds and merge policy).

---

## 1. Architecture Overview

"The index" becomes a *collection* of parts described by a manifest, instead of a single
`index.aspt`:

```
index-dir/
  manifest              # ordered list of live segments + generation counter (atomic rename updates)
  seg_000017.aspt       # immutable disk segment (existing .aspt format, unchanged)
  seg_000017.del        # tombstone bitmap for that segment (absent = no deletions)
  seg_000021.aspt
  seg_000021.del
  keymap.log            # append-only external_key -> (segment, docid) log (rebuildable)
  [live memory segment] # ANT_memory_index, RAM only, searchable via ANT_search_engine_memory_index
```

- **Add** a document → goes into the live memory segment; searchable immediately (NRT mode)
  or after flush (batch mode).
- **Update** a document → add the new version to the live memory segment, then tombstone
  the old `(segment, docid)` found via the key map (ordering rationale in §2.7).
- **Delete** a document → tombstone only.
- **Flush** → serialize the memory segment as a new `.aspt` (non-quantized — `merge_index`
  refuses quantized inputs), append it to the manifest, start a fresh memory segment.
- **Compact** → extended `atire_merge` combines segments, *skipping tombstoned docids and
  renumbering*, reclaiming space; replaces inputs in the manifest atomically.
- **Search** → fan the query across all disk segments plus the live memory segment, filter
  tombstoned docids during top-k selection, merge results by score.

Freshness profiles are settings, not code paths:

| Profile | Settings |
|---|---|
| Near-real-time | search memory segment: on; flush on size threshold; tiered merge |
| Frequent batches | flush per batch; tiered merge |
| Occasional/nightly | flush immediately after each batch; merge nightly |

## 2. Components

### 2.1 Segment manager & manifest
Owns the segment list. The manifest is a small text file listing segment filenames in
order plus a monotonically increasing generation number used to name new segments.
Every mutation (flush, compaction) writes a new manifest to a temp file and `rename()`s
it into place — readers always see a consistent set. Old segments are unlinked only after
the manifest no longer references them and no open searcher uses them (refcount per open
segment).

### 2.2 Live memory segment
An `ANT_memory_index` paired with `ANT_search_engine_memory_index` (both exist today).
Single writer; searches and the writer are serialized with a readers-writer lock (writes
are short — one document). Flush swaps in a fresh `ANT_memory_index` under the write lock,
then serializes the old one to disk outside the lock.

Durability: documents in the memory segment are lost on crash. Two configurable modes:
- **relaxed** (default): callers may re-submit; they hold the external keys.
- **durable**: an append-only write-ahead log of raw `(op, key, document)` records,
  replayed into a fresh memory segment on startup, truncated on flush.

### 2.3 Tombstones
One bitmap per disk segment (`seg_N.del`), sized to the segment's document count, loaded
into memory with the segment, persisted with write-temp-and-rename after each mutation
(batched — a delete is durable once its `.del` write completes; group-commit is fine).
Tombstoning a doc in the *memory* segment is handled the same way with an in-memory bitset.

Query-time filtering happens **during top-k selection**, not after: `ANT_search_engine`
gains an optional exclusion bitset consulted where accumulators are walked to pick the
top-k (heap insertion point). Filtering after selection would silently return fewer than
k results. Deleted documents still contribute to df/IDF until compaction — the standard,
accepted inaccuracy of every segment-based engine.

### 2.4 Key map
`external_key → (segment_generation, local_docid)`, held as an in-memory hash, persisted
as an append-only log (`keymap.log`: add/tombstone/remap records), compacted opportunistically.
It is *rebuildable*: each segment's doclist stores the document filename, and we store the
external key as that filename — so a corrupt or missing keymap is recovered by scanning
segment doclists (newest wins, older duplicates tombstoned).

The write API returns the assigned `(segment, docid)` so callers can keep their own
mapping, per requirement.

### 2.5 Multi-segment searcher
A reader object that opens one `ANT_search_engine` per disk segment (plus the memory
engine), runs the query against each, applies each segment's tombstone filter, and merges
the per-segment top-k lists by score into a global top-k — the same shape as
`ATIRE_broke`'s result merging, but in-process and without sockets. Results carry
`(segment, docid)` plus the filename/key.

Ranking-stat consistency (phase 3): per-segment IDF and average-document-length drift
slightly until segments merge. Optional global-stats mode sums `N`, term df, and lengths
across segments at open time and feeds them to the ranking function instead of the
per-segment values.

### 2.6 Compacting merge (extend `atire_merge`)
`merge_index()` is already N-way and offset-remaps docids. Extensions:
1. Accept a `.del` bitmap per input segment; while re-encoding each postings list, skip
   tombstoned docids and adjust the difference encoding (decode → filter → re-encode; the
   decode/re-encode already happens for the offset remap, so this is a filter in that loop).
2. Skip tombstoned entries in doclist/filename-index and document-store copying.
3. Correct per-term df and collection stats as postings shrink; drop terms whose postings
   become empty.
4. Emit a remap table `(old_segment, old_docid) → (new_segment, new_docid)` for the key map.

Merge policy: tiered — group segments into size tiers, merge a tier when it holds ≥ *k*
(default 10) segments, plus "compact if tombstone ratio > threshold". Runs in a background
thread or as the existing standalone tool; output swaps in via the manifest.

### 2.7 Write API
On `ATIRE_API` (and exposed through the server/Node.js binding):

```
long long add_document(key, document)        // returns global handle (segment, docid)
long long update_document(key, document)     // tombstone old (if any) + add; upsert semantics
long      delete_document(key)
void      flush()                            // force memory segment to disk
void      maintain()                         // run merge policy now
```

Update ordering: **add the new version first, then tombstone the old**. A crash between
the two leaves a transient duplicate (both versions returned momentarily; recovery
detects duplicate keys via keymap scan and tombstones the older) — preferable to the
reverse order, which could lose the document entirely.

## 3. Error handling
- Manifest and `.del` writes: write-temp + `rename()`; a crash never yields a torn file.
- Flush crash: memory segment lost (relaxed) or replayed from WAL (durable); manifest
  never references a partially written segment because it's updated after the segment
  file is fully written and fsynced.
- Compaction crash: inputs remain in the manifest; the orphan output file is deleted on
  next startup (any `seg_*.aspt` not in the manifest is garbage).
- Keymap corruption: rebuilt from segment doclists.

## 4. Implementation phases
1. **Multi-segment read path**: manifest, segment manager, multi-segment searcher with
   tombstone filtering in top-k selection, key map. Delivers add/update/delete + growth,
   with flush-per-batch. (No merge changes yet — segments just accumulate.)
2. **Compacting merge**: tombstone-skipping + renumbering in `merge_index()`, remap-table
   output, tiered merge policy, background scheduling.
3. **Polish**: global ranking stats across segments, durable WAL mode, keymap log
   compaction.

## 5. Testing
- **Tombstone filter**: top-k with exclusions returns exactly k live docs; scores match a
  from-scratch index without the deleted docs (modulo df drift, asserted separately).
- **Equivalence**: index collection A∪B as (a) one shot, (b) two segments + compact, and
  (c) two segments + updates + compact — compacted postings, doclists, and stats must
  match the one-shot build of the equivalent final collection.
- **Crash injection**: kill between add/tombstone, during flush, during compaction —
  reopen and assert invariants (no torn manifest, duplicate-key recovery, orphan cleanup).
- **Profiles**: NRT (doc searchable before any flush), batch, nightly-merge configurations
  driven through the same API.

## 6. Out of scope
- In-place mutation of postings (rejected: compressed difference-encoded chains make it
  rewrite-per-term anyway; compaction achieves the same end safely).
- Merging quantized/impact-pruned segments (existing `merge_index` restriction stands;
  flush always writes non-quantized segments).
- Distributed/sharded indexes — `ATIRE_broke` already covers multi-node federation.

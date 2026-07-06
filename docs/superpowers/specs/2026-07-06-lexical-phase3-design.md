# Lexical Phase 3: WAL Durability, Global Ranking Stats, Keymap Compaction — Design

**Date:** 2026-07-06
**Status:** Draft for review
**Parent spec:** `2026-07-05-incremental-index-design.md` — this delivers its Phase 3 items
(§2.2 durable mode, §2.5 ranking-stat consistency, §2.4 keymap log compaction), adjusted
to everything shipped since (Phase 2 compaction, hybrid vector search V1, the Node-API
binding).

**Decisions pinned with Eric:** WAL at fflush-per-record durability (process-crash safe)
with an optional fsync toggle; global stats = N + average document length only (per-term
df stays per-segment and converges under `maintain()`).

---

## 1. WAL durable mode

### 1.1 Contract
Opt-in: `set_durable(long on)` called before `open()` (like `set_vector_config`).
When on, every successful `add_document`/`update_document`/`delete_document` is appended
to `<dir>/wal.log` before the call returns. On `open()`, a non-empty WAL is replayed, so
documents that were only in the lost memory segment are reconstructed. The WAL is
truncated only by a successful `flush()` (manual or auto) — at that point its contents
are durable in a manifested segment. Relaxed mode (default) is byte-for-byte today's
behavior; a relaxed open ignores but does not delete an existing `wal.log` (switching
modes across sessions is the operator's affair; replay only happens in durable mode).

### 1.2 Record format (binary, length-prefixed)
```
uint8   op              'A' add | 'U' update | 'D' delete
int32   key_length      followed by key bytes (no NUL)
int64   document_length followed by document bytes ('D' records: length 0, no bytes)
uint8   has_vector      1 -> followed by dimension float32s (dimension from vector.config)
```
Appended after engine-side success only — the log is exactly the applied operations
(rejected adds never appear). `fflush` after each record; `set_wal_fsync(long on)`
additionally `fsync`s per record for power-loss durability. All parsed values are
bounds-checked on replay (key 1..8192 bytes, document 0..256MB, has_vector 0/1, vector
only when the index is vector-enabled); the first short read or invalid field ends
replay — the torn-tail rule: a crash can lose at most the final partial record, never
corrupt earlier ones.

### 1.3 Replay
Runs at the end of `open()` (after all Phase 1/2/compaction recovery, after
`start_new_writer()`), in durable mode, when `wal.log` is non-empty. Records are applied
through the normal public methods with an internal `replaying` flag that suppresses
re-logging. Idempotence against the durable state holds by construction: re-tombstoning
sets an already-set bit, keymap adds overwrite, and the memory segment being rebuilt is
exactly the state that was lost. A crash during replay is safe — the WAL is untouched
until the next successful flush, so the next open replays again. Replay failures on
individual records (e.g. a document the engine now rejects) are skipped, matching what
the original call would have surfaced to the caller.

### 1.4 Failure handling
A WAL append/flush failure never fails the triggering operation — the engine state is
the source of truth and remains correct. The WAL is marked unhealthy for the session
(`wal_healthy()` accessor returns 0); further appends are skipped. The next successful
`flush()` recreates a fresh healthy WAL (its content became durable in the segment, so
nothing is lost by the reset). This mirrors the keymap log's cache-not-truth posture.

## 2. Global ranking statistics

### 2.1 Mechanism
New engine hook: `ANT_search_engine::set_global_document_statistics(long long documents,
double mean_document_length)` overriding the two members the ranking functions read for
N and length normalization (the implementation plan locates the exact members —
`documents` and `mean_document_length`, the ranking function's N and length
normalization inputs; the default ranker in this build is DFR divergence I(ne)B2, not
BM25 — and the setter writes those, nothing else).

Coordinator method `refresh_global_statistics()`:
- global N = Σ `document_count()` over disk segments + `writer_documents`
  (raw counts, matching what a single pre-compaction merged segment would report;
  tombstones are excluded from results, not from stats, exactly as within one segment
  today).
- global mean length = Σ (segment documents × segment mean length) / global N, with each
  segment's mean read from its engine (same source its own ranking uses today); the
  writer's contribution uses its engine wrapper's values when one exists, else the
  writer segment is included on the next refresh after a search materializes it —
  the plan pins the exact accessor.
- Pushes both values into every open disk-segment engine and the NRT wrapper.

Refresh points: end of `open()`, end of successful `flush()`, end of `compact()`
(success path), and inside `rebuild_writer_engine()` after a wrapper is constructed
(rebuilt wrappers must re-receive the override). Cost: O(segments) reads and writes of
two numbers — negligible.

### 2.2 Default and opt-out
Default ON. `set_global_stats(long on)` before or after open; turning it off re-pushes
each engine's own local values (engines remember their originals; the setter with
`documents = 0` restores locals — the plan pins the exact restore mechanism).
Per-term df remains per-segment by design (pinned decision); the drift it causes is
bounded by `maintain()` and documented in the header.

## 3. Keymap log compaction

`ANT_index_keymap` gains: counters filled during `load()` replay — `replayed_records`
and live-entry count — plus `double log_dead_ratio(void)` and `long compact_log(void)`.
`compact_log()` writes a fresh log (temp + rename) containing one `A` record per live
entry, then reopens the append handle; on any failure the old log stays (it is merely
uncompacted, never lost).

Coordinator calls it opportunistically:
- during `open()`, after keymap consistency handling, when `log_dead_ratio() > 0.5`;
- after any `maintain()` invocation in which at least one `compact()` succeeded
  (compaction floods the log with remap records).

## 4. API summary (all on `ATIRE_segment_index`)
```cpp
long set_durable(long on);          // before open(); 0 on success
void set_wal_fsync(long on);        // any time; applies to subsequent appends
long wal_healthy(void);             // 1 = healthy or WAL disabled
void set_global_stats(long on);     // default on; re-pushes stats immediately when open
```
Node binding rider: `durable`, `walFsync`, `globalStats` booleans in the existing
`SegmentIndexOptions`, forwarded before/after open per the rules above; `d.ts` and
README updated. No other binding surface changes.

## 5. Testing
- **WAL:** add/update/delete with and without vectors → destroy without flush → reopen
  → everything recovered (search, counts, vector search, deletes honored); WAL truncated
  after flush (file empty/absent); torn tail (truncate the file mid-record, append
  garbage) → replay stops cleanly at the tear, earlier records intact; default-off mode
  leaves every existing test's behavior unchanged; unhealthy-WAL path (make the WAL
  unwritable mid-session) → operations still succeed, `wal_healthy()==0`, next flush
  restores health; fsync toggle smoke test.
- **Global stats (headline):** the same document set indexed as one segment vs three
  segments, scored with the default ranker (DFR divergence in this build — not BM25;
  per-term df/cf stays per-segment per section 6, so full score identity for terms
  shared across segments is out of reach by design). Three-part contract:
  1. **Strict equality** (1e-4) for any query term whose df is layout-invariant (its
     matching documents all live in one segment): every input to the ranking formula is
     then identical. Includes the NRT case — a term confined to the memory segments of
     both layouts scores strictly equally after the writer-engine rebuild.
  2. **Rank order + constant ratio** for a term shared across segments: identical rank
     order, and the multi/single score ratio is constant across all documents (relative
     spread < 1e-3) — a constant ratio proves N and length normalization are globalized,
     with only the per-term df/cf factor (the same for every document) differing.
  3. **Negative control:** with `set_global_stats(0)` the ratio is measurably NOT
     constant (relative spread > 0.01; measured ≈ 0.47 in the fixture) — per-segment
     N/mean drift varies by segment, proving the mechanism does something.
- **Keymap compaction:** heavy update churn → `log_dead_ratio()` high → after reopen (or
  post-maintain trigger) the log file shrinks measurably and a fresh reload reproduces
  the identical mapping; failure injection (read-only dir) leaves the old log usable.
- **Regression:** all eight C++ suites + the 11 JS binding tests; the binding rider gets
  a durable-reopen JS test.

## 6. Out of scope
- Per-term global df (pinned out; revisit only on demonstrated ranking-quality need).
- Group/timed fsync (would introduce a timer thread).
- WAL for anything beyond the memory segment (disk segments already have their own
  crash contracts).
- Mode migration tooling (switching durable on/off between sessions just works by the
  rules above; no converter needed).

# Dense PQ — Resident-Tier Policy (realize the replace-mode memory win) Design

**Status:** approved 2026-07-09, ready for implementation planning. Issue #19.

**Goal:** Realize the RAM win that dense PQ (Phase 1, fe3d5a7) does not yet deliver. Today, even in PQ replace/rerank
posture the float `.vec` is loaded resident (as the degraded-`.pq` fallback and the rerank exact tier), so PQ's
codes save disk but not memory. This adds a **resident-tier policy** that lets a PQ index keep only a small
resident tier — int8 or nothing — beside the `.pq` codes, dropping the float from RAM (it stays on disk).

**Architecture (one sentence):** A per-index `set_pq_resident_tier({FLOAT,INT8,NONE})` selects what accompanies
the resident `.pq` codes — the float `.vec` (today, default), a persistent per-segment int8 rerank sidecar
(`.pqr`), or nothing (pure ADC) — and `append_segment` populates `segments[].vectors` accordingly, so the
existing replace/rerank gatherers reuse it unchanged as "the rerank/fallback exact-ish tier."

**Tech stack:** C++ engine (`source/`, `atire/`); reuses `ANT_pq_store` (`.pq`, ADC), the V4 int8
`ANT_vector_store`/`ANT_vector_store_writer` (QUANT_REPLACE, per-dimension ranges), and the per-segment
sidecar + compaction + backfill + eager lifecycle.

**Scope:** engine-only, dense vectors. The token-pool analogue (drop the resident `.mvec` float, PQ-backed token
graph) is **issue #24**, separate. Deferred here: OPQ, global codebooks, `k != 256`, runtime tier switching,
lazy per-query float loading, bindings.

---

## 1. Resident-tier policy & config

- **New setter `set_pq_resident_tier(long tier)`** with `enum { PQ_TIER_FLOAT = 0, PQ_TIER_INT8 = 1,
  PQ_TIER_NONE = 2 }`. Default `PQ_TIER_FLOAT` (Phase-1 behavior, unchanged). POST-open config setter.
- **Semantics** (what is resident beside the always-resident `.pq` codes):
  - `FLOAT` — the float `.vec` is resident (today). No RAM win. The safe default.
  - `INT8` — a persistent per-segment int8 rerank sidecar `.pqr` is resident; the float `.vec` is **not**
    resident (it stays on disk). `.pqr` is the existing int8 `ANT_vector_store` on-disk format, written under a
    distinct extension so it is never conflated with a V4 primary `.qvec` (which does not exist in PQ mode,
    since PQ and V4-int8 are mutually exclusive).
  - `NONE` — only the `.pq` codes are resident; pure ADC replace. Maximum compression, approximate ranking only.
- **Persistence:** the tier is stored in `pq.config`. **Bump the `pq.config` version** and append the tier field;
  a Phase-1 `pq.config` written without the field (old version) loads as `FLOAT` (back-compat — an existing PQ
  index behaves exactly as today).
- **Immutability:** tier is set once (immutable, like posture/`m`); a config change to a different tier is
  rejected. Changing tier means rebuilding the index (YAGNI for v1).
- **Rejected combination:** `NONE` + `PQ_POSTURE_RERANK` is rejected at config time (`set_pq_config` /
  `set_pq_resident_tier`, whichever completes the pair) — `NONE` is a replace-only tier (no resident store to
  rescore against).
- **Test accessor** `disk_segment_resident_tier(long long which)` returns `FLOAT`/`INT8`/`NONE` for a loaded
  segment, derived from `segments[which].vectors` (float store / int8 store / NULL), so tests can assert the
  float is not resident under `INT8`/`NONE`.

## 2. Segment load & store population (`append_segment`)

The resident dense store `segments[].vectors` (an `ANT_vector_store *`) becomes the tier-typed "rerank/fallback
exact-ish tier"; `segments[].pq_vectors` (the `ANT_pq_store`) is always resident when `.pq` is valid. The
per-segment load decision is made once at open, keyed on `.pq` validity and the tier:

- **valid `.pq` + `INT8`** → `vectors` = the resident int8 `.pqr` store; the float `.vec` is **not** loaded.
- **valid `.pq` + `NONE`** → `vectors` = NULL.
- **valid `.pq` + `FLOAT`** → `vectors` = float `.vec` (today's behavior).
- **degraded/missing `.pq`** (any tier) → `vectors` = float `.vec` resident — the exceptional correctness
  fallback, identical to today. (A segment can always answer dense queries: either ADC via a valid `.pq`, or an
  exact float scan of the resident float fallback.)

No query-time lazy loading — `INT8`/`NONE` never resident-load the float on the hot path. `exact_vectors` stays
NULL in PQ mode (it is a V4-QUANTIZE_EXACT concept). The float `.vec` **always remains on disk** (never deleted);
this is a RAM optimization, not a disk one.

## 3. Search paths (all gated on `pq_configured()`; byte-identical when PQ is off)

- **replace** (`vector_candidates_pq`): `pq_vectors` valid → ADC scan; else the float `vectors` scan (only
  reachable on the degraded-`.pq` fallback, where `vectors` is float). Shape unchanged.
- **rerank** (`vector_candidates_pq_rerank`): ADC shortlist via `pq_vectors` → rescore each candidate with
  `segments[].vectors->score(docid, query, metric)` — which is now **float (FLOAT tier) or int8 (INT8 tier)**.
  This is what makes `RERANK_QUANT_INT8` a genuine, smaller-footprint exact-ish tier instead of aliasing float.
  Edge case: if the `.pqr` is absent under `INT8` (backfill not yet run / degraded), rescore falls back to
  reconstructing the candidate from the PQ codes (`pq_vectors->reconstruct` → score), i.e. ADC-equivalent
  precision, with no float load. `NONE` + rerank is unreachable (rejected at config time).
- **Resident RAM per segment:** `FLOAT` = codes + float; `INT8` = codes + int8 (¼ the float rows); `NONE` =
  codes only.

## 4. Build, compaction & lifecycle

- **`.pqr` build:** `build_pq()` gains a second step — when `tier == INT8`, after writing `.pq` it also writes
  `.pqr` by int8-quantizing the float `.vec` with the existing `ANT_vector_store_writer` in QUANT_REPLACE mode
  (per-dimension ranges over the segment's vectors). `FLOAT`/`NONE` build only `.pq`. Idempotent, per-segment
  best-effort (a failed `.pqr` leaves that segment's `INT8` rerank to reconstruct-from-PQ). Eager policy builds
  both at flush.
- **Compaction** rebuilds `.pq` (already) plus `.pqr` when `INT8`, retraining/renumbering both over the merged
  `.vec`, refreshing the resident `pq_vectors` and `vectors` **after** the sidecars are finalized, and freeing
  the old resident stores on the Step-6 shuffle teardown (the dense-PQ C1 leak lesson, now also covering the
  int8-tier `vectors` store — which is already freed there as `vectors`, so verify it still is).
- **Float `.vec` on disk is mandatory** for `INT8`/`NONE` (build + compaction retrain read it). Flush already
  writes float `.vec` in PQ mode (quantization OFF); compaction writes the merged float `.vec`. No change needed
  — just never delete it.

## 5. Error handling

- Degraded/missing `.pq` on load (any tier) → resident float `.vec` fallback (correctness over RAM; exceptional).
- `INT8` tier with degraded/missing `.pqr` → rerank rescore reconstructs from PQ codes (no float load); replace
  unaffected.
- `NONE` + rerank posture → rejected at config time (nonzero return).
- Old `pq.config` without the tier field → `FLOAT` (back-compat).
- `set_pq_resident_tier` before PQ/vectors configured, or to a different tier after set → nonzero, config
  unchanged.

## 6. Testing

- **Config:** tier round-trips across reopen; a Phase-1 `pq.config` (no tier field) loads as `FLOAT`;
  `NONE`+rerank rejected; tier immutable (same ok, different rejected); `set_pq_resident_tier` requires PQ
  configured.
- **`.pqr` build:** under `INT8`, `build_pq()` writes `.pqr`; the loaded segment's resident `vectors` store
  `is_quantized()` is true and the float `.vec` is **not** resident (`disk_segment_resident_tier == INT8`).
  Under `FLOAT`, no `.pqr`; under `NONE`, `vectors == NULL` (`disk_segment_resident_tier == NONE`).
- **Search correctness:** `INT8`-tier rerank recall@10 vs exact ≥ the replace-ADC recall AND ≤ the `FLOAT`-tier
  rerank recall (int8 rescore is between ADC and float precision), and ≥ a sane floor; `NONE`-tier replace
  returns sane approximate top-k and rejects rerank; degraded-`.pq` → float fallback still returns correct
  top-k.
- **Compaction:** `INT8` merge rebuilds `.pq` + `.pqr`, renumbering correct (merged segment answers correctly);
  delete `.pqr` → reopen → reconstruct-from-PQ rerank still correct; delete `.pq` → reopen → float fallback
  correct; teardown-free under ASan `detect_leaks=1`.
- **Byte-identicalness:** the `FLOAT` default tier is byte-identical to Phase-1 (fixed query, identical top-k
  gen/docid/score vs a Phase-1-built index), and PQ-unconfigured paths unchanged.
- **Recall sanity** at default `m`: report `FLOAT`-tier rerank, `INT8`-tier rerank, and `NONE`/replace recall on
  the same data; `INT8`-tier rerank ≥ 0.9.
- **ASan/UBSan** clean on the new paths (`.pqr` build/load, int8-tier rescore, compaction rebuild) — the known
  `ANT_file::setvbuff` leak and legacy-lexical misalignment are out of scope; targeted `detect_leaks=1` on the
  compaction test.

## 7. Sequencing

Each step is a shippable increment:

1. `set_pq_resident_tier` + `enum {PQ_TIER_FLOAT,INT8,NONE}` + `pq.config` version bump/back-compat +
   `NONE`⊥rerank rejection + `disk_segment_resident_tier` accessor.
2. `.pqr` int8 sidecar built by `build_pq()` under `INT8` (reuse the V4 int8 `ANT_vector_store_writer`) + eager.
3. Segment-load tier-driven resident population (int8 `.pqr` / NULL / float; degraded-`.pq` float fallback) +
   teardown; `disk_segment_resident_tier` reflects the loaded store.
4. Rerank rescore through the resident int8 tier (`vectors->score`) + reconstruct-from-PQ fallback when `.pqr`
   absent + `NONE` replace-only; recall lock.
5. Compaction rebuilds `.pqr` + renumber + refresh-after-finalize + teardown free.
6. Recall sanity (int8 vs float vs replace) + `FLOAT`-default byte-identical lock + ASan/UBSan sweep.

Phase-1 remains the default (FLOAT); this is a shippable opt-in memory win.

## 8. Repo constraints (carried from the vector stack)

Whole repo `-fPIC`; **NO header dependency tracking → `rm -f obj/*.o lib/libantelope_engine.a` after any header
change** (stale-object SEGV); after an ASan sweep a full clean rebuild is required before a normal (non-ASan)
link; `source/*.cpp` auto-discovered by the makefile; tests build to `bin/<name>` via `make <name>` (`CHECK()`
macro, exit 0 on pass); config setters are POST-open; hits read via `get_hit(i)->{filename,generation,docid,
score}`; a fresh worktree needs `mkdir -p obj bin lib` + the prebuilt `external/**/*.a` copied from the main
checkout.

# Vector V2: Binary-Signature Prefilter — Design

**Date:** 2026-07-06
**Status:** Approved (design), pending plan
**Parent:** `2026-07-06-hybrid-vector-search-design.md` (V1). This delivers the "V2 ideas
(spec'd out of scope)" from V1 §: a TopSig-style binary-signature prefilter over the exact
dense scan.

**Goal:** Speed up `search_vector` / `search_hybrid` on large collections with an opt-in
approximate path — a SimHash Hamming prefilter that shortlists candidates, exact-reranked
by V1's dense kernel — without changing V1's exact results.

**Decisions pinned with the user:**
- **Opt-in approximate.** The exact scan stays the default; existing `search_vector`/
  `search_hybrid` results are bit-identical. The prefilter runs only via new
  `*_approx` methods.
- **Cosine + dot only.** SimHash sign bits track *angular* distance, so the prefilter is
  used for cosine and dot; an approximate **L2** query transparently falls back to the
  exact scan (no recall claim we cannot honor).

---

## 1. Architecture and data flow

Approximate ANN with exact rerank, per query:

1. Sign the query with the index-wide projection → query signature (S bits).
2. For each **disk segment that has signatures**: Hamming-scan its present, non-tombstoned
   documents (popcount over `S/64` words each), keeping the `M` smallest-Hamming docids as
   a candidate pool (`M = top_k * candidate_multiplier`).
3. Exact-rerank those `M` candidates with V1's dense `kernel()` (dot/cosine), inserting into
   the global top-k candidate set.
4. Segments **without** signatures (not yet built, or corrupt sidecar), and the live
   **memory buffer**, are exact-scanned exactly as V1 does. An approximate **L2** query
   runs the full exact path for every segment.
5. Merge/sort and (for hybrid) RRF fusion are identical to V1.

The V1 exact methods and their code paths are untouched.

### Unit boundaries
- `ANT_signature` (new, `source/signature.{h,cpp}`): the index-wide projection + signing.
  What it does: build S deterministic hyperplanes from a seed, sign a dense vector into a
  packed bit-signature, and Hamming-compare two signatures. Depends on: the seed, the
  dimension, and the Mersenne twister. Testable in isolation (monotonicity, determinism).
- `ANT_signature_store` / `ANT_signature_store_writer` (new, `source/signature_store.{h,cpp}`):
  the per-segment `.vsig` sidecar — load/scan and buffered atomic write. Mirrors
  `ANT_vector_store` in shape and forgiving-load posture. Testable in isolation (round-trip,
  corrupt-file degradation, Hamming top-M scan).
- `ATIRE_segment_index` gains the coordinator glue: config, lifecycle hooks (flush/compact/
  open/backfill), and the `*_approx` search methods. These live in
  `atire_segment_index_vector.cpp` (the vector feature file from the split).

## 2. Signatures (SimHash)

`bit_i = 1` iff `dot(vector, hyperplane_i) >= 0`, for `i` in `[0, S)`. `S` = signature bits
(the width), **default 256** (32 bytes/document). Hamming distance between two signatures is
a monotone proxy for the angle between the two vectors, so a smaller Hamming distance means a
larger cosine similarity.

**Projection is index-wide and deterministic.** One 64-bit `seed` seeds an
`ANT_mersenne_twister` that generates all `S * dimension` hyperplane components (each a
standard-normal-ish value; a uniform in `[-1, 1)` is sufficient for random-hyperplane LSH and
avoids needing a Gaussian transform). The full `S * dimension` projection matrix is
**materialized in RAM once at open** (`S=256, dim=768` → ~0.75 MB of float) and reused for
every query and every build; only the `seed` is persisted. This guarantees every segment and
every query share one identical projection — a prerequisite for signatures to be comparable
across segments.

**Metric handling at signing time:**
- **Cosine:** stored vectors are already normalized at insertion (V1); the query is
  normalized before signing (V1 already normalizes it for the cosine kernel).
- **Dot:** sign bits discard magnitude by construction. That is acceptable because the
  prefilter only *shortlists*; stage 3 reranks with the exact dot kernel, which restores
  magnitude. The shortlist is angular; recall is controlled by `candidate_multiplier`.

## 3. Config: `signature.config`

A new index-wide file (sibling of `vector.config`):
```
magic    "ANTSIG01"
version  uint32
bits     int64        signature width S
seed     uint64       projection seed
```
Written the first time approximate mode is enabled on an index (`set_approximate_config`
with no existing file), and **immutable** thereafter — changing `bits`/`seed` would
invalidate every existing `.vsig`. Its presence is also what allows V2 to be enabled on an
existing V1 index: the config is created, then `build_signatures()` backfills sidecars.
Loaded at `open()`; a corrupt/short file is treated as "approximate not configured"
(defensive-parse posture, matching `vector.config`).

## 4. Storage: per-segment `seg_G.vsig` sidecar

```
uint64  magic ("ANTSIG01")
uint32  version
int64   bits              (must equal signature.config bits; else degrade)
int64   document_count
byte[]  presence bitmap   ((document_count + 7) / 8 bytes) -- mirrors the .vec presence
byte[]  signatures        (document_count * bits/8, packed; absent rows zeroed)
```

`ANT_signature_store::load` validates magic, version, bits (against the index-wide config),
document_count, and exact file size before allocating; **any failure degrades to an empty
store** (that segment is exact-scanned), the same forgiving posture as a missing `.del` or
`.vec`. A doc's signature is meaningful only when its presence bit is set (mirrors `.vec` —
a lexical-only document has no vector, hence no signature).

**Lifecycle mirrors `.vec`:**
- **flush:** when approximate is configured, write `seg_G.vsig` alongside `seg_G.vec`,
  signing each present vector.
- **compaction:** rewrite `.vsig` for the merged output using the *same*
  `ANT_docid_renumberer` the `.vec` rewrite uses (deterministic, identical renumbering), so
  signatures stay aligned to the compacted docids. A `.vsig` write failure aborts the
  compaction pre-marker, exactly like a `.vec` failure.
- **orphan sweep:** unmanifested `.vsig` files are deleted on `open()` like other
  per-generation sidecars.
- **backfill:** `build_signatures()` (idempotent) builds a `.vsig` for every manifested disk
  segment that has a `.vec` but no valid `.vsig`, from the segment's dense vectors. Returns 0
  on success; a per-segment failure leaves that segment signature-less (still exact-scanned),
  never corrupt.

## 5. Search flow

`search_vector_approx(query, top_k)` and `search_hybrid_approx(query_text, query_vector,
top_k)`:
- If the metric is **L2**, or approximate is not configured: delegate to the exact
  `search_vector` / `search_hybrid` (transparent fallback; same results).
- Otherwise, sign the query once. For each disk segment: if it has a valid signature store,
  Hamming-scan to a per-segment pool of `M = top_k * candidate_multiplier` smallest-Hamming
  docids (replace-max insertion, O(pool) per doc, mirroring
  `ANT_vector_candidate_insert`), then exact-rerank those `M` with `ANT_vector_store::kernel`
  and insert into the global `ANT_vector_candidate` top-k. A segment without signatures is
  exact-scanned (V1 `scan`).
- The live memory buffer is always exact-scanned and merged (as V1).
- Final sort and filename resolution identical to V1. Hybrid: identical RRF (k=60) over the
  approximate vector ranking and the exact lexical ranking.

`candidate_multiplier` is the recall knob (**default 4**), settable any time.

## 6. API

On `ATIRE_segment_index`:
```cpp
long set_approximate_config(long long bits);   // before/after open; persists signature.config on first enable; 0 on success. bits default 256 if 0 passed.
void set_candidate_multiplier(long long n);    // recall/speed knob; default 4
long build_signatures(void);                   // idempotent backfill for existing segments; 0 on success
long long search_vector_approx(const float *query, long long top_k);
long long search_hybrid_approx(char *query_text, const float *query_vector, long long top_k);
long approximate_configured(void);             // 1 if signature.config is loaded
```
Exact `search_vector` / `search_hybrid` are unchanged.

**Node binding rider:** `approximate: { bits, multiplier }` in `SegmentIndexOptions`
(forwarded to `set_approximate_config` / `set_candidate_multiplier`); async
`buildSignatures()` (AsyncWorker + busy-guard, like `maintain()`); sync
`searchVectorApprox(query, k)` and `searchHybridApprox(text, vector, k)` returning the same
`{key, score, generation, docid}` hits. `d.ts` + README updated.

## 7. Testing

- **SimHash monotonicity** (`ANT_signature` unit): for synthetic vectors at known angles to a
  base, expected-Hamming increases with angle; signing is deterministic across two builds
  from the same seed.
- **Signature store** (`ANT_signature_store` unit): write→load round-trip preserves
  signatures and presence; corrupt magic/bits/short-file all degrade to empty; Hamming top-M
  scan returns the M smallest-Hamming present, non-tombstoned docids.
- **Recall** (e2e): on a fixture of random vectors, approximate top-k vs exact top-k recall
  is >= 0.9 at multiplier 4, and recall is non-decreasing as the multiplier increases
  (measured, not asserted exact). This is the headline correctness signal.
- **L2 fallback**: `search_vector_approx` with an L2 index returns results identical to
  `search_vector` (byte-for-byte ranking).
- **Lifecycle**: signatures survive flush; after compaction the approximate results match the
  pre-compaction approximate results (same recall behavior, aligned docids); `build_signatures`
  backfills a V1 segment created before approximate was enabled; orphan `.vsig` swept.
- **Backward compat**: a pre-V2 index (no `signature.config`) served by
  `search_vector_approx` returns correct results via exact fallback; the exact `search_vector`
  path is bit-identical to pre-V2.
- **Regression**: all C++ suites + JS binding tests, plus a JS approximate-search test.

## 8. Out of scope

- int8 / packed dense-vector layouts (a separate memory-footprint effort).
- HNSW / graph indexes (V3).
- Multi-probe LSH, learned/data-dependent projections.
- Signing the live memory buffer (it is bounded by the flush threshold and cheap to exact-scan).
- Changing V1 exact behavior, formats, or the `.vec` sidecar.

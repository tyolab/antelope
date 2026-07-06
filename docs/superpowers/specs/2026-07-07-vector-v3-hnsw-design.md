# Vector V3: HNSW Graph Index — Design

**Date:** 2026-07-07
**Status:** Approved (design), pending plan
**Parent:** `2026-07-06-vector-v2-signature-prefilter-design.md` (V2) and
`2026-07-06-hybrid-vector-search-design.md` (V1). This delivers the "V3: HNSW" idea spec'd
out of scope in V1/V2: a hand-rolled, per-segment hierarchical navigable small-world graph
for higher-recall / faster approximate vector search on large collections.

**Goal:** An opt-in graph-based approximate vector path that coexists with V1 (exact scan)
and V2 (SimHash prefilter), with transparent exact fallback so V1 results stay bit-identical.

**Decisions pinned with the user:**
- **Per-segment graphs.** One `seg_G.hnsw` sidecar per disk segment, mirroring how `.vec`
  (V1) and `.vsig` (V2) already ride the segment lifecycle. This sidesteps HNSW's hard
  incremental-delete problem: the segment + tombstone + compaction model already gives cheap
  updates/deletes via immutable-segment rebuild, so the graph is built once per segment and
  discarded/rebuilt on compaction — no live per-node deletion machinery.
- **Hand-rolled HNSW.** Consistent with the codebase's self-contained, dependency-free
  posture. The exact-score + exact-fallback safety net (below) means a graph imperfection
  only costs recall, not correctness, so a clean textbook implementation is low-risk.
- **Coexist, opt-in, exact fallback.** V3 is a third path alongside V1 exact and V2
  `*_approx`. Opt-in per index; unsupported/unconfigured queries transparently fall back to
  the untouched V1 exact path. V2 stays for its tiny 32-byte/doc footprint; V3 for higher
  recall. The caller picks by method name.
- **Cosine + L2 native; dot → exact.** Cosine (normalized → angular) and L2 are true metrics
  where HNSW navigation is sound, so both get the graph path — L2 finally gets an approximate
  path (V2's SimHash could not). Raw dot/MIPS violates the triangle inequality and degrades
  HNSW recall, so it transparently falls back to exact. This neatly complements V2 (which
  handles dot).
- **Separate `*_hnsw` methods.** Parallel to V2's `*_approx` family, so both algorithms are
  usable on the same index and V2's just-shipped surface is undisturbed.

---

## 1. Architecture and data flow

Approximate ANN with an exactly-scored graph, per query (cosine or L2, configured):

1. Sign nothing — HNSW navigates using the **exact** metric. For each **disk segment with a
   graph**: run HNSW search over its `seg_G.hnsw` — greedy descent through the upper layers to
   find an entry into layer 0, then an `ef_search`-width best-first search at layer 0. Every
   distance is computed by V1's exact `ANT_vector_store::kernel` against the segment's existing
   `.vec`. The returned candidates therefore carry **exact scores** (this is the key
   difference from V2, whose Hamming prefilter needed a separate exact rerank: here the
   candidate *set* is approximate but every score is already exact, so no rerank stage).
2. Segments **without** a graph (not yet built, or corrupt sidecar), and the live **memory
   buffer**, are exact-scanned exactly as V1/V2. A **dot** query, or an index where HNSW is
   not configured, runs the full exact path for every segment.
3. Merge each segment's top-`ef_search` (≥ top_k) exactly-scored candidates + the live-buffer
   exact scan into the global top-k via the existing `ANT_vector_candidate` machinery.
   Tombstoned docids are filtered from the result set (they may still be traversed as routing
   nodes — a standard HNSW soft-delete, reconciled at the next compaction rebuild).
4. Merge/sort and (for hybrid) RRF fusion (k=60) are identical to V1/V2.

The V1 exact methods and the V2 SimHash methods and their code paths are untouched.

### Unit boundaries
- `ANT_hnsw` (new, `source/hnsw.{h,cpp}`): the graph itself. What it does: build a
  hierarchical navigable small-world graph over a segment's dense vectors, search it for the
  `ef_search` nearest candidates to a query (exactly scored), and serialise / forgiving-load
  the `seg_G.hnsw` sidecar. Depends on: `ANT_vector_store` (for the vectors and the distance
  kernel), `ANT_index_tombstones` (result filtering), and `ANT_mersenne_twister` (level
  assignment). Testable in isolation (recall vs brute force, save/load round-trip, corrupt-file
  degradation, tombstone filtering).
- `ATIRE_segment_index` gains the coordinator glue: config, lifecycle hooks (flush / compact /
  open / backfill), a cached per-segment graph, and the `*_hnsw` search methods. These live in
  `atire_segment_index_vector.cpp` (the vector feature file).

## 2. The graph (hand-rolled HNSW)

Textbook hierarchical HNSW (Malkov & Yashunin 2016).

**Parameters.**
- `M` — max neighbours per node per layer; layer 0 gets `M0 = 2*M`. **Default 16.**
- `ef_construction` — dynamic candidate-list width during insertion. **Default 200.**
- `ef_search` — dynamic candidate-list width during query; the recall/speed knob. **Default
  64**, clamped to `>= top_k` at query time.

**Construction.** Insert the segment's present vectors in docid order. Each node's top level is
`floor(-ln(U) * (1 / ln M))` for `U` uniform in `(0,1]`, drawn from an `ANT_mersenne_twister`
seeded with a **fixed constant** — so a build over a given vector set is **deterministic**
(reproducible for testing; compaction rebuilds a fresh, deterministic graph over the merged
vectors). Per-layer neighbour selection uses the paper's diversity heuristic (Algorithm 4:
keep a candidate only if it is closer to the new node than to any already-selected neighbour),
with reverse edges added and pruned back to the degree cap. The graph stores **only
adjacency** — vectors are never duplicated; every distance reads the `.vec`.

**Metric handling.**
- **Cosine:** vectors are already normalised at insertion (V1); the query is normalised once
  before search (V1 already normalises for the cosine kernel). Cosine then behaves as an
  angular/L2 metric — sound for HNSW.
- **L2:** used directly — a true metric, sound for HNSW. This is V3's new capability over V2.
- **Dot:** not graphed. `search_vector_hnsw` / `search_hybrid_hnsw` transparently delegate to
  the exact `search_vector` / `search_hybrid` when the metric is dot.

Determinism note: unlike V2 (whose signatures had to align to docids across a *shared*
projection), each segment's HNSW graph is independent, so the fixed seed is only for
reproducibility — a compaction rebuild produces a fresh graph over the merged vectors and need
not match any prior graph bit-for-bit.

## 3. Config: `hnsw.config`

A new index-wide file (sibling of `vector.config` / `signature.config`):
```
magic            "ANTHNSW1"   uint64 little-endian
version          uint32
M                int64
ef_construction  int64
```
Written the first time HNSW is enabled on an index (`set_hnsw_config` with no existing file),
and **immutable** thereafter — changing `M` / `ef_construction` would invalidate every existing
`seg_G.hnsw`. Its presence is what allows V3 to be enabled on an existing V1/V2 index: the
config is created, then `build_hnsw()` backfills graphs. Loaded at `open()`; a corrupt/short
file is treated as "HNSW not configured" (defensive-parse posture, matching `vector.config`
and `signature.config`). `ef_search` is a runtime knob (not persisted); it defaults to 64 and
is set via `set_ef_search`. The metric is inherited from `vector.config` (not re-stored).

## 4. Storage: per-segment `seg_G.hnsw` sidecar

A CSR-style (compressed sparse row) adjacency dump:
```
uint64  magic ("ANTHNSW1")
uint32  version
int64   M
int64   ef_construction
int64   document_count
int64   entry_point           docid of the graph entry (top of the hierarchy); -1 if empty
int64   max_level
int32   levels[document_count]        per-node top level; -1 if the doc is not in the graph
                                       (lexical-only / absent vector — mirrors .vec presence)
int64   offsets[document_count + 1]    prefix sums into the neighbour stream (int32 units)
int32   neighbours[ offsets[document_count] ]
        per node i in the graph, for layer 0..levels[i]:
          int32 count
          int32 neighbour_docid[count]
        concatenated; offsets[i]..offsets[i+1] is node i's slice.
```

**Forgiving load — two-phase size validation** (the recurring corrupt-header-allocation
bug-class in this codebase: validate before allocating). Phase 1: read and validate
magic / version / `M` / `ef_construction` / `document_count` / `entry_point` (in
`[-1, document_count)`) / `max_level` against the index-wide config and sane bounds, and
confirm the file is at least large enough for the fixed header + `levels[]` + `offsets[]`.
Phase 2: read `levels[]` and `offsets[]`, then compute the **exact** expected file size
(`header + 4*count + 8*(count+1) + 4*offsets[count]`) and confirm it equals the real file size
**before** allocating the neighbour blob; also confirm `offsets` is non-decreasing and
`offsets[count]` is within a sane bound. **Any** failure degrades to an empty graph (that
segment is exact-scanned), the same forgiving posture as a missing `.del` / `.vec` / `.vsig`.
A node is in the graph only when `levels[docid] >= 0` (a lexical-only document has no vector,
hence no graph node — mirrors `.vec` presence).

**Lifecycle mirrors `.vec` / `.vsig` (all seven sites):**
- **flush:** when HNSW is configured, build `seg_G.hnsw` alongside `seg_G.vec`, from the
  segment's present vectors. Best-effort: a build/write failure leaves the segment graph-less
  (exact-scanned) rather than failing the flush.
- **open:** load each manifested segment's `seg_G.hnsw` into a cached `segment.hnsw_graph`.
- **compaction:** rebuild `seg_G.hnsw` for the merged output from the merged `.vec` (reload the
  freshly-written output `.vec` and build a graph over it), and refresh the in-memory cache
  before the input-drop shuffle. Best-effort — a `.hnsw` failure never aborts an otherwise
  successful merge (a missing graph just means exact-scan), unlike the `.vec` rewrite which
  aborts pre-marker.
- **orphan sweep:** unmanifested `seg_*.hnsw` files are removed on `open()` by the existing
  extension-agnostic `seg_*` sweep (no new code).
- **backfill:** `build_hnsw()` (idempotent) builds a `seg_G.hnsw` for every manifested disk
  segment that has a `.vec` but no valid `.hnsw`, from the segment's dense vectors. Returns 0
  on success; a per-segment failure leaves that segment graph-less (still exact-scanned), never
  corrupt.
- **teardown:** the cached graph is freed in the segment destructor, in `delete_segment_files`
  the on-disk `.hnsw` is removed, and the compaction input-drop frees each dropped input's
  cached graph.

Per-segment build cost is bounded by the flush threshold (~10k docs by default), so a graph
build at flush / backfill / compaction stays within the cost envelope of an already-heavy
flush or merge.

## 5. Search flow

`search_vector_hnsw(query, top_k)` and `search_hybrid_hnsw(query_text, query_vector, top_k)`:
- If the metric is **dot**, or HNSW is not configured: delegate to the exact `search_vector` /
  `search_hybrid` (transparent fallback; identical results).
- Otherwise, for each disk segment: if it has a valid cached graph, HNSW-search it with
  `ef = max(ef_search, top_k)` to get up to `ef` exactly-scored candidates, filtering
  tombstoned docids, and insert them into the global `ANT_vector_candidate` top-k. A segment
  without a graph is exact-scanned (V1 `scan`).
- The live memory buffer is always exact-scanned and merged (as V1/V2).
- Final sort and filename resolution identical to V1/V2. Hybrid: identical RRF (k=60) over the
  HNSW vector ranking and the exact lexical ranking (the `search_hybrid_hnsw` body is
  `search_hybrid`'s body verbatim with the one `vector_candidates(...)` call replaced by
  `vector_candidates_hnsw(...)`).

`ef_search` is the recall/speed knob (**default 64**), settable any time via `set_ef_search`.

## 6. API

On `ATIRE_segment_index`:
```cpp
long set_hnsw_config(long long M, long long ef_construction);  // M<=0 => 16, ef_construction<=0 => 200; persists hnsw.config on first enable (immutable); 0 on success
void set_ef_search(long long ef);                              // recall/speed knob; clamps >= 1; default 64
long build_hnsw(void);                                         // idempotent backfill for existing segments; 0 on success
long hnsw_configured(void);                                    // 1 if hnsw.config is loaded
long long search_vector_hnsw(const float *query, long long top_k);
long long search_hybrid_hnsw(char *query_text, const float *query_vector, long long top_k);
```
Exact `search_vector` / `search_hybrid` and V2's `*_approx` are unchanged.

**Node binding rider:** `hnsw: { M, efConstruction, efSearch }` in `SegmentIndexOptions`
(forwarded to `set_hnsw_config` after open / `set_ef_search`); async `buildHnsw()` (AsyncWorker
+ busy-guard, like `maintain()` / `buildSignatures()`); sync `searchVectorHnsw(query, k)` and
`searchHybridHnsw(text, vector, k)` returning the same `{key, score, generation, docid}` hits.
`d.ts` + README updated (note cosine/L2 only; dot falls back to exact).

## 7. Testing

- **Graph unit** (`ANT_hnsw`): build over N random vectors; search recall vs brute-force exact
  `>=` target at a given `ef_search`, and recall non-decreasing as `ef_search` grows (measured);
  save→load round-trip yields identical search results; corrupt magic / config-mismatch /
  short-file / bad-offsets all degrade to empty; tombstone filtering excludes deleted docids.
- **Recall** (e2e): approximate top-k vs exact top-k recall averaged **over many queries** at
  the default `ef_search`, asserting a margin-safe lower bound — **applying the V2 lesson: a
  single-query recall assertion is a coin-flip; average over a query set and leave headroom**.
  The build seed is fixed, but the assertion still averages queries and keeps margin.
- **L2 native** (e2e): `search_vector_hnsw` on an L2 index achieves the recall target — V3's
  new capability that V2 lacked.
- **Dot fallback**: `search_vector_hnsw` / `search_hybrid_hnsw` on a dot index return results
  **byte-for-byte identical** to `search_vector` / `search_hybrid`.
- **Lifecycle**: graph survives flush; after compaction the approximate results still meet the
  recall target (fresh graph over merged, aligned docids); `build_hnsw` backfills a V1/V2
  segment created before HNSW was enabled; orphan `.hnsw` swept.
- **Backward compat**: a pre-V3 index (no `hnsw.config`) served by `search_vector_hnsw` returns
  correct results via exact fallback; the exact `search_vector` and V2 `*_approx` paths are
  bit-identical to pre-V3.
- **Regression**: all C++ suites + JS binding tests, plus a JS HNSW-search test.

## 8. Out of scope

- A single global cross-segment graph (the per-segment model is the decision).
- Native dot/MIPS graph navigation (dot falls back to exact).
- Product-quantised / int8 / compressed vectors inside the graph (V3 uses the full `.vec`;
  a separate memory-footprint effort).
- Live per-node deletion from a built graph (rely on segment rebuild + tombstone filtering).
- Changing V1 exact behaviour, V2 SimHash behaviour, or the `.vec` / `.vsig` sidecars.

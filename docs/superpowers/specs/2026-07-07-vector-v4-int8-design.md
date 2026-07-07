# Vector V4 — int8 Quantized Vectors: Design

**Status:** approved design, pre-implementation
**Date:** 2026-07-07
**Predecessors:** V1 (exact float32 `.vec`), V2 (SimHash `.vsig` prefilter), V3 (HNSW `.hnsw` graph)

---

## Goal

Cut the memory and disk footprint of dense vectors on large indexes by storing them as **per-dimension scalar int8** (≈4× smaller) instead of float32, while keeping an opt-in path that preserves exact results. Product Quantization (higher compression, per-segment codebook training) is explicitly **out of scope** — deferred to a future V5.

## Mental model — a storage format, not a new tier

V2 and V3 added *sidecars consumed as extra query tiers*, each with its own `*Approx` / `*Hnsw` methods layered over an unchanged float32 source of truth. **V4 is different: it changes the storage format of the vectors themselves.** Quantization is a per-index configuration; when enabled, a segment's dense vectors are stored int8, and *every existing consumer* — the exact scan, HNSW graph build/search, and even V2 signature construction — reads them through one reconstruction path.

Consequently **V4 adds no new search methods.** `searchVector`, `searchHybrid`, and the HNSW variants operate unchanged on a 4×-smaller store. V4 is a config toggle plus an internal storage/decode path, not a new API surface.

## Modes

Two modes, selected per index when quantization is enabled:

- **`replace` (default).** The segment stores int8 only; float32 is dropped. **~4× smaller RAM and disk.** All search (exact scan, HNSW navigation) operates on reconstructed int8 vectors, so results are approximate with no exact fallback for quantized segments.
- **`exact` (opt-in; the "coexist" mode).** float32 stays **resident** as the source of truth and int8 is stored alongside. The heavy scan / HNSW navigation runs on the cheap int8 to build a candidate pool, and the final top-k is **re-scored on float32**, yielding **exact results and faster large-segment search at ~1.25× RAM** (int8 adds 0.25× on top of float32). This is a speed mode that preserves exactness, not a memory-reduction mode.

(A future variant could keep float32 **on disk only** and stream it solely to rerank the top pool — cutting resident RAM while staying near-exact — but that is not part of V4.)

When quantization is **unconfigured**, the engine uses today's pure float32 path unchanged (V1/V2/V3 bit-identical).

## Quantization scheme

**Per-dimension affine scalar int8**, computed per immutable segment at flush:

- For each dimension `d`, compute `min_d` / `max_d` over the segment's vectors.
- Quantize: `q = round((x - min_d) / (max_d - min_d) * 255) - 128` (signed int8), clamped to `[-128, 127]`; a degenerate dimension (`max_d == min_d`) maps to a fixed code.
- Reconstruct: `x ≈ min_d + ((q + 128) / 255) * (max_d - min_d)`.

Per-dimension (rather than per-vector) is required for correctness across metrics: it **preserves each vector's magnitude**, which `dot` and `l2` ranking depend on (per-vector scaling would normalize magnitude away and is only safe for cosine). The `2 × dim`-float header overhead is negligible against a whole segment. In cosine mode vectors are already unit-normalized at insertion, so quantizing normalized vectors and reconstructing yields near-unit vectors — the dot kernel still approximates cosine.

## Storage format — the `.qvec` sidecar

A new per-segment sidecar `seg_%06lld.qvec`:

```
magic u64 ("ANTQVC01")   dimension i64   count i64
float32 min[dimension]     // per-dimension quantization range
float32 max[dimension]
int8    codes[count * dimension]   // row-major, one row per doc
```

- **replace mode:** the segment has a `.qvec` and **no** `.vec`.
- **exact mode:** the segment has **both** `.vec` (float32 source of truth) and `.qvec`.

`.qvec` rides the same seven sidecar lifecycle sites as `.vec` / `.vsig` / `.hnsw`: flush-write, open-load (cached), dtor-free, delete_segment_files, orphan-sweep (generic `seg_*` match), compaction-rewrite, compaction-input-free. Load is validating (magic, dimension match, exact file size) and degrades cleanly on inconsistency.

## Interface — one distance primitive

The change that keeps every consumer uniform: `ANT_vector_store` becomes backend-agnostic (it loads either a float32 `.vec` or an int8 `.qvec`) and exposes two backend-independent primitives:

```cpp
void   reconstruct(long long docid, float *out);            // fill out[dimension] with the (dequantized) vector
double score(long long docid, const float *query, long metric);  // kernel(reconstruct(docid), query)
```

- **float32 backend:** `reconstruct` is a `memcpy` from the resident array; `score` is `kernel(get(docid), query, …)` — identical to today, no extra copy on the hot path.
- **int8 backend:** `reconstruct` applies the per-dimension affine dequant into `out`; `score` reconstructs into a stack buffer then calls `kernel`.

All consumers that must work on both backends go through these: the exact scan and HNSW `distance()` route through `score()`; **V2 signature construction** goes through `reconstruct()` (it needs the vector, not a distance). The raw `get()` (returns a `float*` into resident storage, no copy) remains available on the **float backend only** and is used where a zero-copy pointer is valid.

HNSW's insertion "query" is the vector being inserted, reconstructed **once** per insertion into a float buffer, so all of that insertion's distances are `score(candidate, q_reconstructed, metric)`. Reconstruct-then-`kernel` is used throughout for V4 (provably correct, reuses the existing kernel); an asymmetric int8 fast-path (integer dot with scale correction) is a later pure optimization. The V3 build distance-cache continues to apply (it caches `score` results between node pairs).

## Lifecycle & configuration

- **Config** (`quantization` = `off | replace | exact`) is persisted in a small config file and is **immutable** once set (like `signature.config` / `hnsw.config`), so the persisted config always matches what was built.
- **Flush:** new segments are quantized at write time per the configured mode.
- **Compaction:** merged output segments are (re)quantized; input `.qvec`/`.vec` freed at input-drop.
- **Backfill:** existing float32 segments are converted at the next compaction, or via an explicit backfill pass (`buildQuantized()`), mirroring `buildSignatures()` / `buildHnsw()`.
- **Live memory writer stays float32** — the newest, not-yet-flushed documents are always exact and are merged with quantized-segment results at search time.

## API surface (Node `SegmentIndex`)

- Constructor option `quantize?: 'int8'` or `quantize?: { mode: 'replace' | 'exact' }` (string form ⇒ `replace`).
- `buildQuantized(): Promise<void>` — idempotent backfill for pre-existing segments.
- **No new search methods** — existing `searchVector` / `searchHybrid` / `searchVectorHnsw` / `searchHybridHnsw` operate transparently on the quantized store.
- TypeScript definitions extended accordingly.

## Coexistence with V1/V2/V3

- V4 composes with **HNSW (V3)**: in `replace` mode the graph builds and searches over reconstructed int8 vectors (deterministic; slightly lower recall, acceptable for the memory win); in `exact` mode it navigates on int8 and reranks on float32.
- V4 composes with **SimHash (V2)**: signatures are built from whatever the store serves (reconstructed int8 in replace mode, float32 in exact mode).
- With quantization off, V1/V2/V3 behavior is bit-identical to today.

## Testing

- **Round-trip error bound:** quantize→dequantize a known vector set; assert per-component and cosine error within a documented bound.
- **Recall (replace mode):** averaged over many queries (seed-independent), assert recall vs the exact-float top-k ≥ a margin-safe threshold; single-query recall is noisy, so average (lesson from V2).
- **Exactness (exact mode):** after float32 rerank, assert the top-k is byte-identical to the pure-float baseline.
- **Determinism:** two builds from the same data produce identical quantized segments and graphs.
- **Metric coverage:** dot, cosine, l2 all exercised (per-dimension quant must preserve dot/l2 magnitude ranking).
- **Lifecycle:** flush, compaction rewrite, backfill, orphan sweep, delete — `.qvec` handled at all seven sites; no leaks (ASan).
- **Coexistence:** V4 + V2 + V3 together; quantization-off path unchanged.

## Out of scope (future work)

- **Product Quantization (V5):** per-segment codebook training, ADC lookup-table distance — the higher-compression path.
- **Asymmetric int8 distance fast-path:** integer dot with scale correction, avoiding full reconstruction.
- **Disk-streamed exact rerank:** float32 on disk only, streamed for the rerank pool, to cut resident RAM while staying near-exact.

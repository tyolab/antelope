# Product Quantization (PQ) — Dense Vectors (Phase 1) Design

**Status:** approved 2026-07-08, ready for implementation planning.

**Goal:** A product-quantization compression tier for the dense per-document vectors — a higher-compression
sibling to the V4 int8 codec — usable both as a flat ADC scan and (via the V6 `ANT_vector_source`
abstraction) as an HNSW distance backend, with configurable replace / shortlist-rerank query postures.

**Architecture (one sentence):** A new per-segment `ANT_pq_store` splits each vector into *m* subvectors,
learns a per-subspace k=256 k-means codebook over that segment's vectors, stores *m* code bytes per doc in
a forgiving `.pq` sidecar, and implements `ANT_vector_source` so the same codec serves an ADC scan, an HNSW
backend, and an exact-rerank shortlist — selected by the `quantize`/`pq` config.

**Tech stack:** C++ engine (`source/`, `atire/`); reuses `ANT_vector_source` (V6), `ANT_hnsw`, the
per-segment sidecar + compaction + backfill machinery, and the existing `exact_vectors` rerank tier.

**Scope:** engine-only. **Phase 1 = dense `.pq`** (this spec, a shippable milestone). **Phase 2 = token-pool
PQ over the V6 `.mvec` pool** — its own spec after Phase 1 merges. Deferred: OPQ (learned rotation), global
codebooks, `k != 256`, Node/Python bindings.

---

## 1. PQ codec & storage (`ANT_pq_store`, per-segment)

New unit `source/pq_store.{h,cpp}` (sibling to `ANT_vector_store`/`ANT_multivector_store`), plus the pure
codec helpers in `source/pq_codec.{h,cpp}` (k-means training, encode, ADC-table build, reconstruct — testable
in isolation).

- **Encoding:** split each D-dim vector into **m** contiguous subvectors of `D/m` dims (`m` must divide D).
  Per subspace, train a **k = 256**-centroid codebook by k-means over that segment's present vectors. A vector
  → **m code bytes** (one centroid id, 0..255, per subspace). Compression ≈ `D*4 / m` bytes (e.g. D=128 float
  512 B → m=16 → 16 B, 32×). `m` is the accuracy/size knob.
- **On-disk `.pq` sidecar** (per segment, atomic `.tmp`+rename; forgiving load like every other sidecar):
  header (`magic "ANTPQ001"`, version, `dimension`, `document_count`, `m`, `k`) + presence bitmap
  (`(doc_count+7)/8` bytes) + the codebook (`m * k * (D/m)` floats) + the codes (`doc_count * m` bytes). Any
  validation failure (bad magic/version, `m`/`k`/dim/doc-count mismatch, wrong file size, short read) ⇒
  **degraded empty store** (`document_count()==0`) so the segment falls back to float/int8, never crashes.
- **Distance = ADC (asymmetric):** per query, precompute an **`m × k`** table of query-subvector-to-centroid
  dot products once; each doc's score is **m table lookups summed**. This is the `score()` hot path. Metric:
  dot (and cosine = dot on normalized data, matching the rest of the engine); L2 = negated squared distance
  via per-subspace squared-distance tables so "higher is better" holds. `reconstruct(docid, out)` reassembles
  the approximate float vector from codes (for HNSW reuse / exact-rerank staging).
- **Determinism:** fixed k-means seed + fixed iteration count (e.g. 25), deterministic tie-breaking, so a
  rebuild produces a byte-identical codebook (matches the byte-identical-build discipline in V3/V4).
- **Config:** `m` configurable (must divide D; default `min(16, D)` rounded to a divisor — the plan pins the
  exact default rule); `k = 256` fixed for v1 (1-byte codes). Degenerate subspace (all vectors identical in a
  subspace) → single-centroid cluster, reconstructs exactly.

## 2. Integration via `ANT_vector_source` & query posture

- **`ANT_pq_store` implements `ANT_vector_source`** (`document_count/get_dimension/has/get/is_quantized/
  reconstruct/score`). `is_quantized()` returns true; `get()` returns NULL (no zero-copy float row — callers
  use `reconstruct`, exactly as the int8 branch of `ANT_hnsw` already does). This single seam means PQ serves
  **both** the flat ADC scan (candidate/exact path) **and** the existing **HNSW graph** as a distance backend
  (memory-light resident ANN), with no new graph code.
- **Query posture (both, configurable — mirroring V4's replace/exact):**
  - `pq='replace'` → the dense store *is* the PQ store; ranking uses ADC directly (max memory savings, float
    dropped, approximate final order). Mirrors `QUANTIZE_REPLACE`.
  - `pq={mode:'rerank', rerank:'float'|'int8'}` → PQ (ADC scan or PQ-backed HNSW) produces a shortlist of
    `top_k * candidate_multiplier`, then the retained exact **`exact_vectors`** tier (already in the engine)
    re-scores the top candidates to restore precision. Reuses the V6 candidate→rescore→publish shape.
- **Coexistence with V4 int8:** PQ is a **third, mutually-exclusive dense codec** selected via the `quantize`
  config (`'int8' | 'pq' | {mode,...}`). A segment's dense vectors are float **or** `.qvec` int8 **or** `.pq`
  — never layered. Existing int8/float paths are byte-identical when PQ is unconfigured (all changes gated on
  the PQ config / a non-NULL `.pq` store).

## 3. Lifecycle & build

- **On-demand backfill `build_pq()`** (mirrors `build_hnsw()`/`build_quantized()`/`build_token_index()`): for
  every disk segment with a dense `.vec` and no valid `.pq`, train codebooks + encode + write the sidecar, then
  swap the in-memory store. Per-segment failures skip (segment stays float/int8), never corrupt. Idempotent.
  Returns 0 (1 if PQ unconfigured / no dense vectors).
- **Eager option** (`pq` policy `'eager'|'ondemand'`, default `ondemand`, `set_pq_policy(int eager)` — a
  POST-open pre-use setter like the other config): eager builds `.pq` at flush for the new segment. Default
  on-demand because k-means is the heaviest build step.
- **Compaction:** rebuild `.pq` over the merged `.vec` with fresh per-segment codebooks + renumbered codes —
  best-effort, non-fatal (a failed `.pq` leaves the merged segment float/int8), reusing the renumbering
  discipline already used for `.vec`/`.mvec`/`.tann`, and refreshing the in-memory store (mirroring the
  `.vsig`/`.tann` in-memory refresh; **respect the borrowed-store ordering lesson from V6 — refresh any
  dependent in-memory pointer AFTER the store is finalized**).
- **Forgiving load on segment open:** validate-before-allocate; empty ⇒ fall back to the float/int8 store.
  Gated: `build_pq`/PQ config are no-ops when the index has no dense vectors (`vector_dimension_current == 0`).

## 4. Error handling

- `m` does not divide `dimension` → `set` config error (nonzero return, consistent with the config-setter
  convention); `build_pq` with PQ unconfigured or no dense vectors → nonzero no-op.
- Corrupt/mismatched `.pq` on load → silent degrade to empty (fallback to float/int8).
- k-means on a segment with 0 present vectors → empty store (no codebook), fallback.
- A code byte out of `[0,k)` from a corrupt-but-size-consistent file → rejected in load's content validation
  (like the HNSW/`.tann` CSR bounds checks) → degrade.

## 5. Testing

- **Codec (`source/pq_codec` unit):** k-means determinism (fixed seed → identical codebook across runs);
  encode→ADC-score vs exact dot within a PQ-error tolerance on a synthetic set; reconstruct round-trip error
  bounded; degenerate subspace (identical values) → exact; `m` not dividing D → error; L2 vs dot table
  correctness.
- **Store (`ANT_pq_store`):** save/load round-trip; forgiving load (missing / truncated / bad-magic /
  m-mismatch / dim-mismatch → empty); presence bitmap honored; `is_quantized()`/`reconstruct()`/`score()`
  contract; ADC score matches a reconstruct-then-dot within float tolerance.
- **Integration:** `.pq` as an HNSW backend — recall vs a float-backed graph on the same vectors ≥ threshold;
  **replace** posture returns sane approximate top-k; **rerank** posture recall ≥ replace and ≥ threshold vs
  exact; coexistence — an index configured int8 vs pq are mutually exclusive per segment and both correct;
  compaction retrain + renumber correctness (merged segment answers correctly; delete `.pq` → reopen →
  float/int8 fallback still correct); **PQ-unconfigured paths byte-identical** to today (fixed query, identical
  top-k generation/docid/score vs an index built without PQ).
- **Recall sanity** at default `m` (recall@10 vs exact ≥ threshold, e.g. ≥0.9 for rerank posture), tuned once.
- **ASan/UBSan** clean on the new paths (codebook/codes/ADC-table allocations, load validation, HNSW-over-PQ,
  compaction rebuild) — the `-fsanitize=address,undefined` sweep via `CC='g++ -fsanitize=...'`,
  `ASAN_OPTIONS=detect_leaks=0` (the known `ANT_file::setvbuff` leak is out of scope).

## 6. Sequencing

Phase 1 builds the codec first (isolated, unit-tested), then `ANT_pq_store` + forgiving load, then the
`ANT_vector_source`/ADC-scan + HNSW-backend integration, then the replace/rerank postures + config, then
`build_pq` backfill + eager policy, then compaction rebuild, then recall + sanitizer. Phase 1 is a shippable
milestone on its own. Phase 2 (token-pool `.mvec` PQ, reusing this codec) is specced separately after Phase 1
merges.

## 7. Repo constraints (carried from the vector stack)

Whole repo `-fPIC`; **NO header dependency tracking → `rm -f obj/*.o lib/libantelope_engine.a` after any
header change** (stale-object SEGV risk); `source/*.cpp` auto-discovered by the makefile; tests build to
`bin/<name>` via `make <name>` (`CHECK()` macro, exit 0 on pass); config setters are POST-open; default ranker
is DFR not BM25; hits read via `get_hit(i)->{filename,generation,docid,score}`.

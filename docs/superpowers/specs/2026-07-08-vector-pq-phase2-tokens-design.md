# Product Quantization (PQ) — Token Pool (Phase 2) Design

**Status:** approved 2026-07-08, ready for implementation planning. Issue #18.

**Goal:** A product-quantization compression tier for the V6 late-interaction **token pool** (`.mvec`) — the
token-level sibling of the Phase-1 dense `.pq` codec — so per-token vectors are PQ-compressed and scored via
ADC-MaxSim, with configurable replace / rerank query postures.

**Architecture (one sentence):** A new per-segment `ANT_multivector_pq_store` splits each token vector into
*m* subvectors, learns one per-subspace k=256 k-means codebook over that segment's whole token pool, stores
*m* code bytes per token in a forgiving ragged `.mvpq` sidecar, and implements the token-level accessors +
ADC-MaxSim so the codec serves both a direct ADC ranking (replace) and an exact float-MaxSim rerank shortlist
— selected by the `set_multivector_pq_config` config.

**Scope note (decided at planning):** the V6 token-HNSW (`.tann`) graph is NOT rebuilt over PQ codes in
Phase 2 — it stays over the resident float `.mvec`. A PQ-backed token graph saves no memory while the float
pool is kept resident (the exact/rerank/fallback tier), and it would require a non-trivial `ANT_token_index`
generalization; it is deferred and paired with dropping the resident float — the same reason Phase 1 deferred
dense HNSW-over-PQ to issue #20.

**Tech stack:** C++ engine (`source/`, `atire/`); reuses `ANT_pq_codec` (Phase 1, unchanged), the V6
`ANT_multivector_store` / `ANT_multivector_source` / `ANT_token_index` machinery, and the per-segment sidecar
+ compaction + backfill + eager lifecycle.

**Scope:** engine-only, a shippable milestone on its own. **Phase 1 = dense `.pq`** shipped at fe3d5a7
(`docs/superpowers/specs/2026-07-08-vector-pq-dense-design.md`). **This (Phase 2) = token-pool `.mvpq`.**
Deferred: the PQ-backed token-HNSW graph (`.tann` over PQ codes) paired with dropping the resident float pool
for the full replace-mode memory win (issue #19 class, tokens), OPQ, global codebooks, `k != 256`, Node/Python
bindings.

---

## 1. PQ codec & storage (`ANT_multivector_pq_store`, per-segment)

New unit `source/multivector_pq_store.{h,cpp}` — a **ragged, self-contained** PQ token store, the token-pool
sibling of `ANT_pq_store`. It reuses the pure codec in `source/pq_codec.{h,cpp}` unchanged (k-means training,
encode, ADC-table build, `adc_score`, reconstruct).

- **Encoding:** the V6 token pool is `total_tokens` L2-normalized `D`-dim vectors flattened across all docs in
  the segment (per-doc counts `M_d >= 0`). Split each token into **m** contiguous subvectors of `D/m` dims
  (`m` must divide D). Train **one** k=256-centroid codebook **per segment** over the whole token pool (tokens
  across all docs = abundant training data), then each token → **m code bytes**. Compression ≈ `D*4/m` bytes
  per token.
- **On-disk `.mvpq` sidecar** (per segment, atomic `.tmp`+rename; forgiving load like every other sidecar):
  header (`magic "ANTMVPQ1"`, version, `dimension`, `documents`, `total_tokens`, `m`, `k`) + per-doc `counts`
  (`documents` ints) + the codes (`total_tokens * m` bytes) + the codebook (`m * k * (D/m)` floats). Offsets
  are derived from `counts` at load (prefix sum). Any validation failure (bad magic/version, `m`/`k`/dim/doc
  mismatch, wrong file size, short read, a code byte outside `[0,k)`, or `sum(counts) != total_tokens`) ⇒
  **degraded empty store** (`token_count()==0`) so the segment falls back to the `.mvec` float/int8 pool, never
  crashes.
- **Distance = ADC (asymmetric):** the token-level `token_score(t, query, metric)` scores one token by ADC
  (build the query's `m × k` table once, then `m` lookups). `maxsim(docid, query_vecs, num_query_vecs)` is the
  **ADC-MaxSim** hot path: build `num_query_vecs` per-query-token ADC tables once, then for each of the doc's
  tokens do `m` lookups per query token, take the max per query token, and sum. Metric: dot (cosine = dot on
  normalized data, matching V6, whose tokens are L2-normalized on write); L2 via per-subspace squared-distance
  tables so "higher is better" holds. `token_reconstruct(t, out)` reassembles the approximate float token.
- **Token API (mirrors `ANT_multivector_store`'s V6 accessors):** `token_count`, `token_has`,
  `token_reconstruct`, `token_score`, `token_docid_of`, `vector_count(docid)`, `has(docid)`, `maxsim`,
  `max_vector_count`, `get_m`. (No `ANT_vector_source` adapter in Phase 2 — nothing builds an HNSW over PQ
  codes yet; that adapter is added by the deferred PQ-backed-token-graph work.)
- **`ANT_multivector_pq_store_writer`:** `create(path, dim, m, metric)`, `append(doc_vectors, M_d)` (accumulates
  the ragged pool + per-doc counts; `append(NULL, 0)` for a doc with no tokens), `finish()` (trains the
  codebook over the accumulated pool + encodes + writes atomically), `abandon()`.
- **Determinism:** the Phase-1 codec's fixed k-means seed + fixed iteration count give a byte-identical
  codebook on rebuild (matches the byte-identical-build discipline in V3/V4/dense-PQ). Degenerate subspace
  (all tokens identical in a subspace) → single-centroid cluster, reconstructs exactly. Empty pool
  (`total_tokens == 0`) → empty store, no k-means.
- **Config:** `m` configurable (must divide D; default `default_pq_m(D)` = largest divisor of D in
  `[1, min(16, D)]`, reusing Phase 1's rule); `k = 256` fixed (1-byte codes).

## 2. Integration & query posture

- **Per-segment field** `ANT_multivector_pq_store *multivector_pq` alongside `multivectors` (`.mvec`) and
  `token_index` (`.tann`); loaded on open when multivector-PQ is configured, torn down in the destructor and at
  every teardown site, forgiving fallback to `.mvec` when absent/degraded.
- **Token-HNSW backend — unchanged:** the V6 `.tann` graph stays built/loaded over the resident float `.mvec`
  (`ANT_multivector_store`) exactly as today. PQ affects only the *scoring* stage, not candidate generation, in
  Phase 2. (Rebuilding the graph over PQ codes is deferred — see the scope note above.)
- **Query posture (both, configurable — `set_multivector_pq_config(m, posture, rerank_quant)`, mirroring
  Phase 1's replace/rerank):**
  - `posture = replace` → `search_multivector` runs the V6 (float) token-HNSW / brute-force candidate
    generation → the final MaxSim scoring is **ADC-MaxSim** via `multivector_pq->maxsim` (approximate final
    order, the route to dropping the resident float later). Segments without a valid `.mvpq` fall back to exact
    float `maxsim`; the live buffer is always exact float.
  - `posture = rerank` → candidate generation → **exact float MaxSim rescore** via the resident `multivectors`
    (`.mvec`). Phase 2 **keeps `.mvec` resident** as the exact tier (correctness-first, mirroring Phase 1);
    `rerank_quant = RERANK_QUANT_INT8` therefore **aliases float** in Phase 2 (the resident float is strictly
    more precise and already present — a distinct int8 rerank tier only pays off once the resident float is
    dropped, deferred).
  - `search_rerank`'s existing candidate → MaxSim → publish shape is reused; the posture affects HOW candidates
    are finally scored.
- **Coexistence with the V6 int8 token pool:** a segment's token pool is float **or** `.mvec`-int8 **or**
  `.mvpq`-PQ — never layered. `set_multivector_pq_config` is rejected when the `.mvec` int8 mode is already
  configured, and vice-versa (mutually exclusive, mirroring dense PQ vs int8). Existing V5/V6 float/int8 paths
  (including the `.tann` graph) are **byte-identical** when multivector-PQ is unconfigured (all changes gated on
  the config / a non-NULL `.mvpq` store).
- **`multivector_pq` lifetime:** nothing borrows `multivector_pq` in Phase 2 (the `.tann` graph borrows
  `multivectors`, not the PQ store; the scoring gatherers read `multivector_pq` per-query, not retained), so
  there is no use-after-free hazard — but teardown must free `multivector_pq` at **every** site (the
  dense-PQ C1 lesson: free it at every segment-teardown site, including the compaction shuffle).

## 3. Lifecycle & build

- **On-demand backfill `build_multivector_pq()`** (mirrors `build_pq()`/`build_token_index()`): for every
  segment with a float `.mvec` token pool and no valid `.mvpq`, train the codebook + encode + write the
  sidecar, then swap the in-memory store. Per-segment failures skip (segment stays on `.mvec`), never corrupt.
  Idempotent. Returns 0 (1 if multivector-PQ unconfigured / no multivectors).
- **Eager option** (`set_multivector_pq_policy(int eager)`, default ondemand, POST-open pre-use setter): eager
  builds `.mvpq` at flush for the new segment. Default ondemand because k-means is the heaviest build step.
- **Compaction:** rebuild `.mvpq` over the merged token pool with a fresh per-segment codebook + renumbered
  ragged codes — best-effort, non-fatal (a failed `.mvpq` leaves the merged segment on the `.mvec` fallback),
  reusing the renumbering discipline already used for `.vec`/`.mvec`/`.tann`/`.pq`, and refreshing the
  in-memory `multivector_pq` after its `.mvpq` is finalized. The `.tann` rebuild is unchanged (built over the
  merged float `.mvec`), so there is no cross-store ordering constraint; just free the old `multivector_pq` on
  the shuffle teardown.
- **Forgiving load on segment open:** validate-before-allocate; empty ⇒ fall back to the `.mvec` float/int8
  store. Gated: `build_multivector_pq`/config are no-ops when the index has no multivectors
  (`rerank_dimension_current == 0`).

## 4. Error handling

- `m` does not divide `dimension` → `set_multivector_pq_config` error (nonzero, config-setter convention);
  `build_multivector_pq` with multivector-PQ unconfigured or no multivectors → nonzero no-op.
- Corrupt/mismatched `.mvpq` on load → silent degrade to empty (fallback to `.mvec` float/int8).
- k-means on a segment with 0 tokens → empty store (no codebook), fallback.
- A code byte outside `[0,k)`, or `sum(counts) != total_tokens`, from a size-consistent but corrupt file →
  rejected in load's content validation (like the HNSW/`.tann` CSR bounds checks) → degrade.

## 5. Testing

- **Store (`ANT_multivector_pq_store` unit):** save/load round-trip; forgiving load (missing / truncated /
  bad-magic / m-mismatch / dim-mismatch / counts-mismatch → empty); ragged per-doc `counts`/`vector_count`
  honored; `is_quantized()`/`token_reconstruct()`/`token_score()` contract; **ADC-MaxSim vs exact-float MaxSim
  within a PQ-error tolerance** on a synthetic ragged set; token-pool codebook determinism (byte-identical
  across runs).
- **Integration:** **replace** posture `search_multivector` scores on ADC-MaxSim and returns sane approximate
  top-k (top-1 == exact on well-separated data); **rerank** posture recall ≥ replace and ≥ 0.9 vs exact MaxSim;
  coexistence — an index configured `.mvec`-int8 vs `.mvpq`-PQ are mutually exclusive per segment and both
  correct; compaction retrain + renumber correctness (merged segment answers correctly; delete `.mvpq` → reopen
  → `.mvec` fallback still correct); **V5/V6-unconfigured paths byte-identical** (fixed query, identical
  `search_multivector`/`search_rerank` top-k generation/docid/score vs an index built without multivector-PQ).
- **`multivector_pq` teardown-leak regression under ASan (`detect_leaks=1`):** compact two segments that each
  carry a built `.mvpq`; the compacted-away segments' `multivector_pq` must be freed (no leak) — mirrors the
  dense-PQ compaction leak test.
- **Recall sanity** at the default `m` (recall@10 vs exact MaxSim ≥ threshold, e.g. ≥ 0.9 for rerank), tuned
  once.
- **ASan/UBSan** clean on the new paths (codebook/codes/counts allocations, load validation, ADC-MaxSim,
  compaction rebuild) — `-fsanitize=address,undefined` via `CC='g++ -fsanitize=...'`,
  `ASAN_OPTIONS=detect_leaks=0` (the known `ANT_file::setvbuff` leak is out of scope). Run a targeted
  `detect_leaks=1` pass on the compaction test to guard the teardown free (the dense-PQ C1 lesson).

## 6. Sequencing

Each step is a shippable increment:

1. `ANT_multivector_pq_store` + `.mvpq` save + forgiving load + ADC-MaxSim (unit-tested; codec reused from
   Phase 1).
2. `set_multivector_pq_config(m, posture, rerank_quant)` + `multivector_pq.config` persistence + mutual
   exclusion with the `.mvec` int8 mode.
3. Segment load `.mvpq` + teardown (every site incl. compaction shuffle) + `build_multivector_pq` backfill +
   `set_multivector_pq_policy` eager.
4. **Replace** posture (ADC-MaxSim scoring) wired into `multivector_candidates`; V5/V6-unconfigured
   byte-identical regression lock. (`.tann` graph unchanged, over float.)
5. **Rerank** posture (float candidate shortlist → exact float MaxSim rescore via resident `.mvec`).
6. Compaction retrain + renumber `.mvpq` + free `multivector_pq` on teardown.
7. Recall sanity at default `m` + ASan/UBSan sweep (incl. `detect_leaks=1` on compaction).

Phase 2 is a shippable milestone. Deferred: the **PQ-backed token-HNSW graph** (`.tann` over PQ codes), which
is paired with dropping the resident float pool for the full replace-mode memory win; a true int8 rerank tier;
OPQ; global codebooks; `k != 256`; and Node/Python bindings.

## 7. Repo constraints (carried from the vector stack)

Whole repo `-fPIC`; **NO header dependency tracking → `rm -f obj/*.o lib/libantelope_engine.a` after any
header change** (stale-object SEGV risk); after an ASan sweep the objects are ASan-instrumented, so a full
clean rebuild is required before a normal (non-ASan) link; `source/*.cpp` auto-discovered by the makefile;
tests build to `bin/<name>` via `make <name>` (`CHECK()` macro, exit 0 on pass); config setters are POST-open;
hits read via `get_hit(i)->{filename,generation,docid,score}`.

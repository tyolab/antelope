# Vector V6 — Token-Level ANN Retrieval Design

**Status:** approved 2026-07-08, ready for implementation planning.

**Goal:** Make multi-vector (late-interaction) retrieval a **first-class search path** in the
engine. Today V5 only *reranks* a candidate set produced by a lexical/dense first stage
(`search_rerank` requires text and/or a single query vector). V6 builds a per-segment
token-level ANN index over the existing ragged `.mvec` token pool so a query's token vectors
can *retrieve* candidate documents directly — no first-stage text/vector required — then rescores
them with the exact V5 MaxSim.

**Architecture (one sentence):** A per-segment HNSW graph whose nodes are the flattened,
L2-normalized document tokens of the `.mvec` pool (a `.tann` sidecar + a `token_docid[]` map)
generates candidate documents from the query tokens; the existing V5 `maxsim()` rescores the
candidates exactly, so the ANN affects *which* documents are considered but never the final score.
This is PLAID's shape minus centroid pruning.

**Tech stack:** C++ engine (`source/`, `atire/`); reuses `ANT_hnsw` (V3), `ANT_multivector_store`
(V5), `ANT_filter` (Filtered ANN), and the segment accumulator/compaction machinery.

**Scope:** engine-only (C++ + tests). Node/Python bindings, centroid/PLAID pruning, and PQ
residuals are explicit follow-ups.

---

## 1. Data flow

For `search_multivector(query_multivector Q, num_query_vecs, top_k, filter)`:

1. **Per query token** `q_i` (i = 0..num_query_vecs-1): search the segment's `.tann` graph for its
   top-`token_top_p` nearest **doc-tokens** (kernel = dot over L2-normalized vectors — identical to
   the MaxSim kernel).
2. **Gather → candidate docids:** map each retrieved token to its docid via `token_docid[]`, and
   accumulate into a `docid → provisional_score` map, where a doc's provisional score is the sum of
   its best token-kernel per query token. Keep the top `candidate_multiplier × top_k` candidates by
   provisional score. (The provisional score is used **only for pruning**, never for final ranking.)
3. **Exact rescore:** run the existing V5 `maxsim(docid, Q, num_query_vecs)` over each surviving
   candidate → exact late-interaction score → publish top-`top_k` through the accumulator.
4. **Filter:** when `filter != NULL`, admit only matching docids during accumulation, over-gathering
   the candidate pool so a selective filter never under-fills (same "no under-fill" principle as the
   shipped Filtered ANN). Unfiltered path (`filter == NULL`) is byte-identical to no-filter behavior.

**Ranking invariant:** V6's published top-`k` equals a brute-force MaxSim over the candidate set the
ANN produced. The ANN changes recall (which docs are considered), never the score of a considered
doc.

## 2. Token index structure (`.tann` sidecar) + HNSW generalization

- **New per-segment sidecar `seg_G.tann`:** an HNSW graph whose **nodes are the flattened tokens** of
  that segment's `.mvec` pool, plus a persisted `token_docid[]` array (one int per token; derivable
  from the `.mvec` `counts`/`offsets`, stored for O(1) hit→doc resolution).
- **Generalize `ANT_hnsw` (contained refactor of `source/hnsw.{h,cpp}`):** today `build()`/`search()`
  take a concrete `ANT_vector_store` and treat node index == docid. Introduce a minimal "point
  source" seam — an abstraction exposing `count()` and `distance(node, query, metric)` over
  L2-normalized vectors — so the *same* graph code indexes either:
  - per-doc vectors (`ANT_vector_store`, node == docid) — **V3 behavior must be byte-identical**, or
  - the token pool (`ANT_multivector_store`'s flat pool, node == token index).

  `search()` returns node ids; for the token graph the caller maps node→docid via `token_docid[]` and
  dedupes to candidate docs. The refactor keeps a single graph implementation (no duplicate
  token-HNSW).
- **Node id width:** node ids remain int32 (`ANT_HNSW_MAX_DOCUMENTS` = INT_MAX). Tokens exceed the
  docid count by the average tokens-per-doc factor, so the token graph hits this ceiling sooner; this
  is an explicit **per-segment token-count limit** (segments flush well below it under normal
  `flush_threshold`s). `build_token_index()` returns a nonzero error if a segment's token count would
  exceed the cap (degrades to brute-force fallback, never crashes).
- **Kernel:** dot product over L2-normalized vectors (the `.mvec` pool is normalized on write), so
  the graph's distance = -kernel, matching V3's convention.

## 3. Build strategy & configuration

- **`token_index` policy flag** (constructor option; POST-construct pre-open setter
  `set_token_index_policy(int eager)`): `'ondemand'` (default) or `'eager'`.
  - **`ondemand`:** flush stays cheap (no token graph built). `build_token_index()` constructs the
    graph per segment on demand, mirroring `build_hnsw()`/`build_signatures()`/`build_quantized()`.
    Rebuilt at compaction.
  - **`eager`:** every flush builds the token graph for the new segment when rerank is configured
    (like `.vec`/`.vsig`/`.hnsw` are built at flush when enabled). Always ready; every flush pays the
    token-graph build cost + memory.
- **`build_token_index(void) -> long`:** per-segment backfill; 0 = success, nonzero on
  allocation/cap failure (best-effort per segment). Idempotent (rewrites `.tann`).
- Gated on `rerank_configured()`: no rerank schema ⇒ no `.mvec` ⇒ token index is a no-op and
  `search_multivector` raises the same "rerank not configured" error the rest of the multi-vector
  path uses.

## 4. Candidate generation knobs

- **`token_top_p`** (default 32): nearest doc-tokens pulled per query token. Higher → more recall,
  more candidates, more cost.
- **`candidate_multiplier`** (reuses the existing member, default 4): candidate pool cap =
  `candidate_multiplier × top_k` (over-gathered further under an active filter).
- Both exposed as config with tuned defaults; a single tuning pass on a synthetic set sets the
  defaults.

## 5. Engine API surface

```cpp
// standalone first-class path (+ filtered overload, mirroring every other search)
long long search_multivector(const float *query_multivector, long long num_query_vecs, long long top_k);
long long search_multivector(const float *query_multivector, long long num_query_vecs, long long top_k, const ANT_filter *filter);

// backfill builder (ondemand) + policy setter
long build_token_index(void);            // per-segment token graph; 0 = success; rebuilt at compaction
long set_token_index_policy(int eager);  // POST-construct, pre-open (mirrors other config setters)

// search_rerank gains a token-ANN first stage when text AND vector are both absent:
//   search_rerank(NULL, NULL, query_multivector, num_q, first_stage_n, top_k[, filter])
//   -> was the both-NULL ValueError/SIGSEGV guard; now routes to search_multivector for stage 1,
//      then the existing MaxSim rerank publishes top_k.
//   When text and/or vector ARE present, search_rerank is unchanged.
```

- `query_multivector` is the flattened `num_query_vecs × rerank_dimension` row-major buffer already
  used by `search_rerank`.
- Results publish through the same accumulator/`get_hit` path as every search, so tombstones,
  generations, payloads, and cross-segment merge all work unchanged.

## 6. Persistence, compaction & lifecycle

- **Persistence:** `.tann` + `token_docid[]` written by `build_token_index()` (ondemand) or at flush
  (eager), alongside `.mvec`. Forgiving `load()`: any corruption/config mismatch → treated as "not
  built" → brute-force fallback (never crashes; mirrors `.hnsw`/`.vsig`).
- **Live buffer:** un-flushed docs have no `.tann`; they are scored by brute-force `maxsim_live` over
  the writer's live multi-vector buffer (already exists) and merged in, so NRT results are never
  missing.
- **Fallback when not built** (ondemand, pre-backfill): brute-force MaxSim over all docs in the
  segment (correct, slower) rather than erroring — `search_multivector` always returns correct
  results; `.tann` only makes it fast. `ondemand`-fallback results must equal `eager`/backfilled
  results (ranking equality).
- **Compaction:** when merging segments, rebuild `.tann` over the merged `.mvec` (best-effort,
  non-fatal — a failed `.tann` leaves the merged segment retrievable via fallback), reusing the
  renumbering discipline already in `atire_segment_index_compaction.cpp` for the `.mvec` block.

## 7. Error handling

- `search_multivector` / `build_token_index` when `rerank_configured()` is false → error consistent
  with the existing multi-vector path ("rerank not configured").
- `search_multivector` with `num_query_vecs == 0` or a NULL buffer → 0 hits (no candidates), not a
  crash.
- `build_token_index` allocation failure or token-count over the int32 cap → nonzero return, segment
  left in fallback mode (no crash, no partial/corrupt `.tann`).
- Corrupt/mismatched `.tann` on load → silent degrade to fallback.

## 8. Testing

- **Unit:**
  - HNSW generalization: V3 doc-HNSW `search()`/`build()` results **byte-identical** after the
    point-source refactor (regression lock on the existing V3 tests); the new token-source path
    builds and searches.
  - `token_docid[]` mapping correctness (token index → docid) against known `.mvec` layouts.
  - Candidate accumulation + `candidate_multiplier` pool cap.
  - **Ranking-equality invariant:** `search_multivector` top-k == brute-force MaxSim over the
    candidate set the ANN produced.
  - Filtered vs unfiltered candidate generation (no under-fill; unfiltered byte-identical).
  - Live-buffer merge: un-flushed docs appear in results via `maxsim_live`.
  - Ondemand-fallback result == eager/backfilled result.
  - Compaction rebuild: merged segment retrievable; renumbering correct.
  - Corrupt `.tann` → fallback (forgiving load).
  - `search_rerank(NULL, NULL, qmv, ...)` routes through token-ANN stage 1 instead of erroring.
- **Recall sanity:** on a small synthetic multi-vector set, `search_multivector` recall vs exhaustive
  MaxSim is above a set threshold at default knobs.
- **ASan/UBSan** clean on the new paths.

## 9. Out of scope (explicit follow-ups)

- Node addon (`searchMultivector`) and Python binding (`search_multivector`) exposure.
- Centroid/PLAID candidate pruning (centroid interaction before MaxSim) — the "IVF/centroid" and
  "full PLAID" options deferred from the architecture decision.
- PQ residual compression of tokens (its own sub-project, sibling to V4 int8).
- Multi-vector WAL durability (tracked separately).

# PQ-backed HNSW (tier-selected graph source) Design

**Status:** approved 2026-07-10, ready for implementation planning. Issue #20.

**Goal:** Give a PQ index a memory-light resident ANN by building and searching the HNSW graph over the
representation the #19 resident tier already selects — most importantly, over the m-byte PQ codes under the NONE
tier, where no float is resident. Plus fix the `ANT_pq_store::score()` blocker (review finding M1): a 128 KB
on-stack ADC table declared on every call.

**Architecture (one sentence):** One source-selection rule — the HNSW graph is built and searched over
`segments[].vectors != NULL ? segments[].vectors : segments[].pq_vectors` — makes HNSW + PQ + the #19 tier
compose with no new config: FLOAT navigates over float (byte-identical to today), INT8 over the int8 `.pqr`, NONE
over `pq_vectors` (ADC over codes), and non-PQ over float (unchanged).

**Tech stack:** C++ engine (`source/`, `atire/`). Reuses `ANT_hnsw` (build/search take an `ANT_vector_source *`
and reference it at search time — the int8 branch already scores via `reconstruct()`/`score()`, never `get()`),
`ANT_pq_store` (already an `ANT_vector_source`; `test_pq_hnsw` already proves a graph builds/searches over PQ
codes), and the per-segment sidecar + build/compaction/eager machinery. Builds directly on the #19 resident-tier
stack (`0d189ac`).

**Scope:** engine-only, dense vectors. Deferred: the token-pool analogue (#24), and the per-node ADC-rebuild
perf fix (a prepared-per-query-table seam — filed as a new follow-up; correct today, just not optimal).

---

## 1. `score()` minimal fix (review M1)

`ANT_pq_store::score()` (`source/pq_store.cpp`) declares `double stack_table[64*256]` (~128 KB) on the stack of
every call and rebuilds the ADC table each call. Fix the stack size only (per the chosen scope):

- Introduce a small cap `PQ_SCORE_STACK_CAP` (`8 * ANT_pq_codec::K` = 2048 doubles = 16 KB — safe on threaded
  `atire_api_server`/MCP worker stacks). Declare the inline buffer at that cap; when `m * K` exceeds it,
  heap-allocate the table (`new double[m*K]` / `delete[]`) as the code already does above the old 128 KB buffer.
- For the default `m ≤ 16` (`default_pq_m`), `m*K ≤ 4096` doubles — the table still fits inline at the 16 KB cap
  only up to `m ≤ 8`; `m` in (8,16] heap-allocates. That is acceptable: the alloc is bounded and the returned
  score is byte-identical either way. The behaviour and the returned value are unchanged — this is purely the
  stack-size / allocation-boundary fix.
- The per-call ADC rebuild is intentionally **not** changed here (out of scope by decision). It is a slowdown on
  NONE-tier graph navigation (one `adc_table` build per visited node), never a correctness issue. Filed as a
  separate perf follow-up: a `prepare_query(query,metric)->ctx` / `score_prepared(node,ctx)` seam on
  `ANT_vector_source`, threaded once per search through `ANT_hnsw`.

## 2. The source-selection rule

Every site that builds or searches the graph uses the single rule (only meaningful when a graph exists, i.e.
`hnsw_M_current != 0`):

```
graph_source(which) = segments[which].vectors != NULL ? segments[which].vectors : segments[which].pq_vectors
```

Why it composes with the #19 tier (which already populated `vectors`):
- **FLOAT** — `vectors` is the float store → build/search over float → **byte-identical to today's HNSW**.
- **INT8** — `vectors` is the int8 `.pqr` store → graph over int8 (`ANT_hnsw` reconstructs per node); memory-light.
- **NONE** — `vectors` is NULL → graph over `pq_vectors` (`ANT_pq_store`, ADC over the m-byte codes); zero float
  resident — the resident ANN this issue targets.
- **No PQ** — `pq_vectors` is NULL, `vectors` is float → float; **unchanged**.

Consistency invariant: `ANT_hnsw` stores only adjacency and binds the source at build **and** search time; the
graph must be searched over the same representation it was built over. The #19 tier is immutable per index, so
`graph_source(which)` returns the same store at build and search time — the invariant holds automatically.

## 3. Wiring (the three sites)

The graph **file** (`.hnsw`) is representation-agnostic adjacency, so segment load (`append_segment`) is
unchanged — it still loads the `.hnsw` when `hnsw_M_current != 0`. Only building and searching pick a source.

Entry point (unchanged routing): the graph is reached through `search_vector_hnsw(q,k)`, exactly as for float
HNSW today; the default `search_vector(q,k)` still routes to the PQ replace/rerank gatherers (linear ADC scan /
shortlist-rescore) when `pq_configured()`. This design only changes which **source** `search_vector_hnsw`'s
gatherer builds/searches the graph over — it does not change which top-level call selects the graph.

- **`vector_candidates_hnsw`** (`atire/atire_segment_index_vector.cpp`, the gatherer): replace the
  `if (segments[which].vectors == NULL) continue;` guard with the source rule — compute `src = graph_source(which)`,
  skip the segment only when `src == NULL` (NONE with no `pq_vectors`). Pass `src` to
  `hnsw_graph->search(query, metric, ef, ef, src, tombstones, ...)`. Final candidate scoring keeps today's logic:
  rescore via `exact_vectors` when present (non-PQ QUANTIZE_EXACT only), else use the graph's returned
  `cand_scores` — which under PQ tiers are already the best available resident precision (ADC under NONE, int8
  under INT8), so no extra rescore is added.
- **`build_hnsw` + eager** (`atire/atire_segment_index_vector.cpp`): build the graph over `graph_source(which)`
  instead of the reloaded float `.vec`. Under NONE this is `pq_vectors`, so `build_pq()` must have run first;
  best-effort — if the tier-selected source is absent (`NULL` or empty), skip the graph for that segment, exactly
  like any missing prerequisite today. (For FLOAT/INT8 the resident `vectors` is present; the graph builds over
  it as before.)
- **Compaction** (`atire/atire_segment_index_compaction.cpp`): build the merged graph over the merged segment's
  `graph_source`, ordered **after** the merged `.pq`/`.pqr` refresh (#19) so that under NONE `output_segment->
  pq_vectors` is already refreshed and available as the graph source. Best-effort as today.

Metric: `search_vector_hnsw` already transparently falls back to the exact path for `VECTOR_METRIC_DOT` (no
bounded kernel for graph navigation), so PQ-HNSW engages only for COSINE/L2 — no special handling needed.

## 4. Error handling

- **NONE tier, no `pq_vectors`** (e.g. `build_pq` not yet run): `graph_source` is NULL → the segment contributes
  nothing to the HNSW gather (guarded skip), and `build_hnsw`/compaction skip the graph for it. The segment still
  answers via the exact/replace path (the non-HNSW gatherers), so results stay correct, just not graph-accelerated.
- **Degraded/missing `.hnsw`**: existing per-segment fallback (the gatherer's `else` branch scans the resident
  source), unchanged.
- **`score()` large `m`**: heap-allocates the ADC table above the stack cap (no overflow), returns the correct
  score.
- **DOT metric**: exact fallback (existing), so no PQ-HNSW navigation for DOT.

## 5. Testing

New `tests/test_pq_hnsw_tiered.cpp` (the existing `tests/test_pq_hnsw.cpp` — graph-over-PQ smoke — stays green):

- **`score()` stack cap:** an `ANT_pq_store` with `m*K` above `PQ_SCORE_STACK_CAP` returns a score equal to a
  brute-force `adc_table`+`adc_score`, with no oversized stack frame (correctness proxy for the alloc boundary).
- **NONE-tier end-to-end:** `set_pq_config(m, REPLACE, FLOAT)` + `set_pq_resident_tier(NONE)` +
  `set_hnsw_config(M, ef)` with COSINE, add docs, `flush`, `build_pq`, `build_hnsw`; assert
  `disk_segment_resident_tier(0) == PQ_TIER_NONE` (zero float resident) AND `search_vector_hnsw(q,10)` recall@10
  vs an exact reference ≥ a sane floor (planted-nearest recalled) AND the HNSW gatherer engaged (graph answered,
  not the NULL-skip fallback — verify e.g. via a non-empty graph `disk_segment_has_hnsw(0)`).
- **FLOAT-tier byte-identical lock:** a PQ+FLOAT+HNSW index and a non-PQ float-HNSW index over identical data +
  fixed query return the identical top-k (`filename`, `docid`, `score`) — PQ-FLOAT does not perturb float HNSW.
- **INT8-tier:** PQ+INT8+HNSW builds the graph over the int8 `.pqr` and returns sane recall@10.
- **Compaction:** NONE-tier, two flushes, `build_pq`+`build_hnsw`, `compact`; the merged segment's graph is
  rebuilt over `pq_vectors` and `search_vector_hnsw` stays correct (planted doc recalled).
- **ASan/UBSan** clean on the new paths (graph build/search over `pq_vectors`, the gatherer source switch,
  compaction rebuild); the known out-of-scope `ANT_file::setvbuff` leak + legacy-lexical misalignment excluded;
  `detect_leaks=1` on the compaction test.

## 6. Sequencing (TDD tasks)

1. `score()` stack cap + heap-above (`PQ_SCORE_STACK_CAP`) + unit test.
2. Gatherer source rule: `graph_source` selection in `vector_candidates_hnsw` + NONE engages the graph
   (`vectors==NULL` no longer skips when `pq_vectors` valid) + NONE-tier end-to-end recall test + FLOAT
   byte-identical lock.
3. `build_hnsw` + eager build over the tier-selected source (NONE → `pq_vectors`, best-effort) + INT8-tier test.
4. Compaction builds the merged graph over the tier-selected source, ordered after the `.pq`/`.pqr` refresh +
   compaction recall test.
5. Recall sanity across tiers + ASan/UBSan sweep + file the per-node-ADC-rebuild perf follow-up.

## 7. Repo constraints (carried from the vector stack)

Whole repo `-fPIC`; **NO header dependency tracking → `rm -f obj/*.o lib/libantelope_engine.a` after any header
change**; after an ASan sweep a full clean rebuild is required before a normal link; `source/*.cpp` +
`tests/*.cpp` auto-discovered; tests build to `bin/<name>` via `make <name>` (`CHECK()` macro, exit 0 on pass);
config setters are POST-open; `set_vector_config`/`set_hnsw_config` are PRE-open, `set_pq_config`/
`set_pq_resident_tier` POST-open; hits via `get_hit(i)->{filename,generation,docid,score}`; `search_vector` /
`search_vector_hnsw` return the result count; `add_document(key, doc_string, float_vec)` is the 3-arg vector
overload; `segment_filename` zero-pads the generation to 6 digits (`seg_%06lld.<ext>`); default ranker DFR.

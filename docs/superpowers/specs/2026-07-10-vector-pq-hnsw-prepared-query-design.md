# Prepare-per-query ADC table seam (PQ-HNSW navigation) Design

**Status:** approved 2026-07-10, ready for implementation planning. Issue #26 (follow-up to #20).

**Goal:** Under the NONE resident tier, HNSW navigation scores each visited node with
`ANT_pq_store::score()`, which rebuilds the m*K ADC table (O(D*256)) on *every* call. Build the table
**once per search** instead of once per visited node, via a small optional seam on `ANT_vector_source` that
`ANT_hnsw::search` threads through. Pure performance change with a byte-identical result invariant.

**Architecture (one sentence):** Add three defaulted virtuals to `ANT_vector_source`
(`prepare_query` → opaque ctx, `score_prepared(node, query, metric, ctx)`, `free_query(ctx)`); the default
`score_prepared` ignores ctx and calls `score()`, so every existing source is unaffected; `ANT_pq_store`
overrides them to build/reuse/free the ADC table; `ANT_hnsw::search` calls `prepare_query` once, threads the
ctx through `distance()` at every node, and frees it on the single return path.

**Tech stack:** C++ engine (`source/`). Reuses `ANT_pq_codec::adc_table`/`adc_score` (unchanged), `ANT_hnsw`,
`ANT_pq_store`. Builds directly on the #20 PQ-backed HNSW stack (`7dcb9db`) and the #19 resident tier
(`0d189ac`).

**Scope:** engine-only, dense vectors, **search path only**. Deferred / explicitly out of scope: the `build()`
distance-cache path (its per-pair "query" is a different node's reconstructed vector every call, so a prepared
table cannot be reused — no win, and it would complicate the build memo); the token/multivector analogue
(that source keeps the default no-op — a token-PQ prepared seam belongs to #24); the flat `scan_adc` path
(already builds the table once — optimal, untouched).

---

## 1. The seam: three defaulted virtuals on `ANT_vector_source`

Add to `source/vector_source.h` (after `score`), all with default bodies so `ANT_vector_store` and
`ANT_multivector_source` need **zero** changes:

```cpp
/* Optional per-query precomputation. Default: no-op — score_prepared ignores ctx and
   calls score(). A source with a per-query-precomputable structure (PQ's ADC table)
   overrides all three: prepare_query builds it once, score_prepared reuses it, free_query
   releases it. Caller contract: prepare_query(q,metric) -> ctx; every score_prepared for
   that search passes the SAME q and metric; free_query(ctx) exactly once. */
virtual void  *prepare_query(const float *query, long metric) { (void)query; (void)metric; return NULL; }
virtual double score_prepared(long long node, const float *query, long metric, void *ctx)
                    { (void)ctx; return score(node, query, metric); }
virtual void   free_query(void *ctx) { (void)ctx; }
```

Rationale for `void *` (opaque ctx) rather than a typed scorer object: it keeps the base interface free of any
PQ-specific type, `ANT_hnsw` never needs to know what the ctx is, and ownership is trivial (allocated by
`prepare_query`, freed by the matching `free_query`). Float/int8/multivector return `NULL` and pay only one
virtual call per search plus a `NULL` check per node.

## 2. `ANT_pq_store` overrides

Add the three overrides to `source/pq_store.h`/`.cpp` plus a test-visibility counter (see §4):

```cpp
/* pq_store.h (public): */
long long adc_table_builds;   /* # of adc_table() builds; test proves the seam engaged. Init 0 in ctor. */
void  *prepare_query(const float *query, long metric);
double score_prepared(long long node, const float *query, long metric, void *ctx);
void   free_query(void *ctx);
```

```cpp
/* pq_store.cpp: */
void *ANT_pq_store::prepare_query(const float *query, long metric)
{
if (documents == 0 || codebook == 0)
    return NULL;                                       /* degraded store: no table, ctx==NULL falls back */
double *table = new double[m * (long long)ANT_pq_codec::K];
ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);
adc_table_builds++;
return table;
}

double ANT_pq_store::score_prepared(long long node, const float *query, long metric, void *ctx)
{
if (ctx == NULL)
    return score(node, query, metric);                 /* no prepared table -> per-call build (build path) */
if (!has(node))
    return 0.0;
return ANT_pq_codec::adc_score(codes + node * m, m, (double *)ctx);
}

void ANT_pq_store::free_query(void *ctx)
{
delete [] (double *)ctx;                               /* delete[] NULL is a no-op */
}
```

Also increment `adc_table_builds` inside the existing `ANT_pq_store::score()` at the point it calls
`ANT_pq_codec::adc_table` (so the counter reflects *every* table build, whether per-call or prepared) — this is
what lets the §4 test assert "1, not N". The ADC table encodes `(query, metric)`; `score_prepared` therefore
ignores its own `query`/`metric` when `ctx != NULL`. This is sound under HNSW because a single search uses one
fixed metric and one fixed query.

## 3. `ANT_hnsw`: prepare once, thread, free once (search only)

`distance()` gains a trailing defaulted `ctx` and routes through the prepared path:

```cpp
/* hnsw.h — private helper signature: */
double distance(long long a, const float *query, ANT_vector_source *vectors, long metric, void *ctx = NULL);

/* hnsw.cpp: */
double ANT_hnsw::distance(long long a, const float *query, ANT_vector_source *vectors, long metric, void *ctx)
{
return -vectors->score_prepared(a, query, metric, ctx);
}
```

`ANT_hnsw::search` (hnsw.cpp:300) prepares the ctx after the early-empty guard, threads it through all three
`distance()` call sites, and frees it on the single return:

- The early return `if (entry_point < 0 || documents == 0) return 0;` (line 304-305) stays **before**
  `prepare_query`, so nothing is allocated on that path.
- After it, `void *qctx = vectors->prepare_query(query, metric);`.
- The three `distance()` calls — greedy-descent entry `dep` (line 314), upper-layer neighbours (line 326),
  and layer-0 `ecand` (line 349) — each pass `qctx` as the final argument.
- Immediately before the terminal `return out;` (line 372), `vectors->free_query(qctx);`.

`search` has exactly two returns (the early `0` before prepare, and the terminal `out`), so a single
`free_query` before the terminal return covers every allocated path — no leak.

**`build()` is unchanged.** Its `dist_stored`/`dist_ids` helpers (hnsw.cpp:130-155) call `distance()` with the
default `ctx == NULL`, so `score_prepared` falls back to `score()` — byte-identical to today's build. No new
`prepare_query`/`free_query` in build.

## 4. Correctness invariant + testing

**Invariant:** for a valid `ANT_pq_store`, `score_prepared(node, q, metric, prepare_query(q, metric))` is
numerically identical to `score(node, q, metric)` — both compute `adc_score(codes+node*m, m, T)` for the same
deterministic `T = adc_table(q, metric, codebook)`. Therefore a NONE-tier HNSW search visits nodes in the same
order and returns the **byte-identical top-k** as #20, building the table once per search instead of once per
visited node. Float/int8/multivector sources return `NULL` ctx → default `score_prepared` → unchanged.

New `tests/test_pq_hnsw_prepared.cpp`:

- **Store-level equivalence:** build an `ANT_pq_store`, pick several nodes; assert
  `score_prepared(node, q, metric, ctx) == score(node, q, metric)` for `ctx = prepare_query(q, metric)`, and
  assert the `ctx == NULL` fallback also equals `score(node, q, metric)`. `free_query(ctx)` then `free_query(NULL)`
  both clean (ASan).
- **Seam-engaged proof:** on a NONE-tier PQ+HNSW index (`set_pq_config(m, REPLACE, FLOAT)` →
  `set_pq_resident_tier(NONE)` → `set_hnsw_config` → add docs → `flush` → `build_pq` → `build_hnsw`), record
  `pq_vectors->adc_table_builds`, run one `search_vector_hnsw(q, 10)` whose traversal visits more than one node,
  and assert the counter rose by **exactly 1** (prepared once), not once per visited node. (Sanity: also assert
  a bare linear `pq_vectors->score()` loop over N docs raises the counter by N, so the "1 vs N" contrast is real.)
- **End-to-end recall / byte-identical:** the NONE-tier HNSW still recalls the planted nearest and returns the
  same top-k (`filename`, `docid`, `score`) as the exact ADC reference — the #20 recall assertion carried forward,
  now over the prepared path.
- **Default source untouched:** an `ANT_vector_store` (float) returns `NULL` from `prepare_query`, and
  `score_prepared(node, q, metric, NULL) == score(node, q, metric)`.
- **ASan/UBSan** clean on the new paths (single `prepare_query` alloc, single `free_query`, early-return-before-
  prepare); the known out-of-scope `ANT_file::setvbuff` leak + legacy-lexical misalignment excluded;
  `detect_leaks=1` on the seam test.

## 5. Sequencing (TDD tasks)

1. `ANT_vector_source` three defaulted virtuals + `ANT_pq_store` overrides + `adc_table_builds` counter
   (counter bumped in both `prepare_query` and `score()`); store-level equivalence + fallback unit test.
2. `ANT_hnsw::distance` gains `ctx`; `search` prepares/threads/frees; seam-engaged counter test (1 not N) +
   end-to-end NONE-tier recall byte-identical lock + default-source (float) untouched test.
3. ASan/UBSan sweep on the seam test (`detect_leaks=1`); confirm `build()` byte-identical (unchanged), close #26.

## 6. Repo constraints (carried from the vector stack)

Whole repo `-fPIC`; **NO header dependency tracking → `rm -f obj/*.o lib/libantelope_engine.a` after the
`vector_source.h`/`pq_store.h`/`hnsw.h` changes**; after an ASan sweep a full clean rebuild is required before a
normal link; `source/*.cpp` + `tests/*.cpp` auto-discovered; tests build to `bin/<name>` via `make <name>`
(`CHECK()` macro, exit 0 on pass); config setters POST-open, `set_vector_config`/`set_hnsw_config` PRE-open,
`set_pq_config`/`set_pq_resident_tier` POST-open; `add_document(key, doc_string, float_vec)` 3-arg vector
overload; hits via `get_hit(i)->{filename,generation,docid,score}`; `search_vector_hnsw` returns the result
count and falls back to exact for `VECTOR_METRIC_DOT` (so PQ-HNSW navigation engages only for COSINE/L2);
`segment_filename` zero-pads the generation to 6 digits; default ranker DFR.

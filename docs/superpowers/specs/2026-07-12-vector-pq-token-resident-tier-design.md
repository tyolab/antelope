# PQ-backed token graph + drop resident float `.mvec` Design

**Status:** approved 2026-07-12, ready for implementation planning. Issue #24.

**Goal:** Realize the RAM win on the late-interaction (token / `.mvec`) side, the token analogue of the dense
`#19` (resident tier) + `#20` (PQ-backed HNSW) + `#26` (prepare-per-query ADC seam) work already shipped. Under
a new `NONE` resident tier, build and search the V6 token-HNSW (`.tann`) over the PQ-compressed token pool
(`.mvpq`) and **stop loading the resident float `.mvec` pool** — the float stays on disk (retrain/compaction
read it), but no float token pool is held in RAM.

**Architecture (one sentence):** Generalize `ANT_token_index` off its hard `ANT_multivector_store*` dependency
onto a new `ANT_token_source` abstraction (which extends `ANT_vector_source` so it IS the graph source, plus
`token_docid_of`/`num_documents`); add `ANT_multivector_pq_source` so the token graph can navigate over the
`.mvpq` codes (ADC); and add a `{FLOAT, NONE}` resident tier that selects, per segment at load, whether the
float `.mvec` pool is held resident (FLOAT, byte-identical to today) or dropped in favour of pure-ADC token
search over `.mvpq` (NONE).

**Tech stack:** C++ engine (`source/`, `atire/`). Reuses `ANT_hnsw` + the shipped `#26` prepare-per-query seam
on `ANT_vector_source` (so the token graph builds its ADC table once per query token for free), the
`ANT_multivector_pq_store` (`.mvpq`) and its ADC token API, and the V6 `ANT_token_index`/`.tann` machinery.
Builds directly on the token-PQ Phase 2 stack (`1e69f18`) and the V6 token-ANN stack (`b150e15`).

**Scope:** engine-only, token/multivector side. **Deferred** to follow-ups: a token-INT8 resident tier (an int8
`.mvec` analogue of the dense INT8 tier — Phase 2 already found int8-rerank aliases float, so YAGNI here); and
wiring replace-ADC into `search_rerank`'s two-stage rescore (Phase 2 review Minor #1). Token-PQ remains mutually
exclusive with the int8 `.mvec` mode (unchanged); the NONE tier is PQ-only, so that invariant is preserved.

---

## 1. `ANT_token_source` — generalize `ANT_token_index`

`ANT_token_index` today borrows a concrete `ANT_multivector_store*` and, at build/search, wraps it in a
stack-local `ANT_multivector_source` to feed `ANT_hnsw`. It uses the store for exactly three things: snapshot
`token_docid[]` at build (`token_docid_of`), read `document_count`/`get_dimension` (already snapshotted into
members), and construct the graph source. So the needed abstraction is small.

New abstract interface in `source/vector_source.h`, beside `ANT_vector_source` (no new header — both the float
and PQ token stores already pull in `vector_source.h`):

```cpp
class ANT_token_source : public ANT_vector_source   // IS the graph source: node == token
{
public:
    virtual long long num_documents(void) = 0;       // distinct DOCS (ANT_vector_source::document_count() == token count)
    virtual long long token_docid_of(long long t) = 0;
};
```

- `ANT_multivector_source` (in `multivector_store.h`) changes base from `ANT_vector_source` to
  `ANT_token_source`, adding `num_documents() { return store->document_count(); }` and
  `token_docid_of(t) { return store->token_docid_of(t); }`. Its existing `ANT_vector_source` methods are
  unchanged → **float path byte-identical**.
- New `ANT_multivector_pq_source : public ANT_token_source` over `ANT_multivector_pq_store*`:
  `document_count()`→`store->token_count()` (graph node count), `get_dimension()`→`store->get_dimension()`,
  `has(node)`→`store->token_has(node)`, `get(node)`→`NULL`, `is_quantized()`→`1`,
  `reconstruct(node,out)`→`store->token_reconstruct(node,out)`,
  `score(node,query,metric)`→`store->token_score(node,query,metric)` (ADC),
  `num_documents()`→`store->document_count()`, `token_docid_of(t)`→`store->token_docid_of(t)`. Plus the `#26`
  seam overrides (see §3).
- `ANT_token_index`: replace the `ANT_multivector_store *store` member with `ANT_token_source *source`
  (borrowed, not owned). `build(ANT_token_source*, M, ef, metric)`, `load(filename, ANT_token_source*, …)`, and
  `search_candidates` use `source` directly (pass it to `graph->build`/`graph->search`;
  `source->token_docid_of`, `source->num_documents`, `source->get_dimension`). No stack-local
  `ANT_multivector_source` is constructed any more. The two `.tann` save/load size/format are unchanged
  (`token_docid[]` snapshot is representation-agnostic).

**Ownership / lifetime:** the source object is **owned by the segment** (a new member, §2), constructed as the
float or PQ wrapper when the segment loads/builds its token index, and it borrows the underlying store. Teardown
order is fixed everywhere (dtor, compaction shuffle): delete `token_index` **before** `token_source` **before**
the stores. This is one new borrowed-lifetime edge on top of the existing borrowed-store class; it is documented
and ordering-pinned exactly like the V6 borrowed-store discipline.

## 2. Resident tier `{FLOAT, NONE}`

New knob `set_multivector_resident_tier(long tier)` with `enum { MV_TIER_FLOAT = 0 (default), MV_TIER_NONE = 1 }`
(in `atire_segment_index.h`). POST-open; requires token-PQ already configured (`multivector_pq_configured()`);
immutable once moved off the FLOAT default; idempotent for the same value. Persisted by **bumping
`multivector_pq.config` to add a trailing tier i64** — a config written before this change (no tier field) loads
as FLOAT (back-compat), mirroring the dense `pq.config` v2 change.

Segment members (`atire_segment_index.h`): keep `multivectors` (float `.mvec`), `token_index`,
`multivector_pq` (`.mvpq`); **add `ANT_token_source *token_source`** (owned by the segment).

Segment load (`append_segment`), when `multivector_pq_configured()`: load `multivector_pq` first, then select by
tier:
- **FLOAT** — load the float `.mvec` into `multivectors` as today; `token_source = new ANT_multivector_source(multivectors)`. Byte-identical to pre-#24 (rerank/exact/`maxsim` all read the resident float).
- **NONE** — do **not** load the float pool resident (`multivectors == NULL`); `token_source = new ANT_multivector_pq_source(multivector_pq)`. Exact/rerank rescoring routes through `multivector_pq->maxsim` (ADC), exactly as the token-PQ replace posture already does. If `.mvpq` is degraded/absent under NONE, `token_source` is NULL and the segment falls back to the float `.mvec` load (best-effort, same forgiving-degrade posture as the rest of the stack).

The float `.mvec` **always stays on disk** — `build_multivector_pq`/compaction retrain read it as ground truth
(the dense residency lesson: the tier is a query-time RAM view, retrain/merge always read the on-disk float).

`disk_segment_resident_tier_mv(which)` test accessor → NONE if `multivectors == NULL && multivector_pq != NULL`,
else FLOAT.

## 3. The `#26` prepare-per-query ADC seam for the token graph

`search_candidates` runs one `graph->search` **per query token** (`num_query_vecs` searches). Under NONE each
search navigates over `.mvpq` codes; without the seam every visited node rebuilds the m·256 ADC table. `#26`
already put `prepare_query`/`score_prepared`/`free_query` on `ANT_vector_source` and `ANT_hnsw::search` already
calls them once per search — so wiring the token PQ source into the seam makes each query token build its ADC
table exactly once.

- `ANT_multivector_pq_store` gains `void *token_prepare_query(const float *query)` (build the m·256 ADC table
  once, using its stored codebook/dimension/m/metric), `double token_score_prepared(long long t, void *ctx)`
  (`adc_score(token_codes(t), m, (double*)ctx)`; `ctx==NULL` → fall back to `token_score`), and
  `void token_free_query(void *ctx)` (`delete[]`). Add a public diagnostic counter `adc_table_builds`
  (incremented in `token_score`'s per-call build and in `token_prepare_query`), for the "built once per query
  token" test — mirrors the dense `ANT_pq_store::adc_table_builds`.
- `ANT_multivector_pq_source` overrides `prepare_query(query,metric)`→`store->token_prepare_query(query)`,
  `score_prepared(node,query,metric,ctx)`→`store->token_score_prepared(node,ctx)` (with the NULL fallback),
  `free_query(ctx)`→`store->token_free_query(ctx)`.
- `ANT_multivector_source` (float) inherits the `#26` no-op defaults → float token search unchanged.

Build path is untouched (build's per-pair "query" varies, `ctx==NULL` → falls back to `token_score`), so the
constructed token graph is byte-identical.

## 4. Lifecycle + stale-graph invalidation

- **`build_token_index()` / eager / compaction** build the `.tann` over the segment's `token_source`. Under NONE
  that is the PQ source, so `build_multivector_pq()` must have produced a valid `.mvpq` first — best-effort: if
  `token_source` is NULL/empty, skip the graph for that segment (same as any missing prerequisite today). The
  eager flush builds `.tann` AFTER `.mvpq` under NONE (tier source realized first), mirroring the dense
  `build_hnsw`-after-`build_pq` ordering.
- **Compaction** (`atire_segment_index_compaction.cpp`): refresh the merged segment's `token_source` **before**
  the `.tann` rebuild (borrowed-source UAF discipline — the exact V6 lesson), and rebuild after the `.mvpq`
  refresh. The float `.mvec` merge continues to read each input's on-disk float pool as ground truth (Phase 2
  already retrains `.mvpq` from the float `.mvec`), so the merge is lossless regardless of tier. Free
  `token_source` in the Step-6 shuffle teardown, ordered after `token_index`, before the stores.
- **Stale-graph invalidation (ported from the `#20` Important fix):** `build_token_index`'s idempotent
  "already built" guard cannot tell a float-geometry `.tann` from an ADC-geometry one. A `set_multivector_resident_tier`
  FLOAT→NONE change (supported: immutable only once off the default) must, on a real change, **delete each
  per-segment `.tann` and its `.tann.g` sidecar (`remove`) and null the in-memory `token_index`**, so the next
  `build_token_index`/compaction rebuilds over the new (PQ) source. Without this, a float-built graph would be
  searched with ADC scoring → silent recall regression (no crash — HNSW tolerates approximate distances).

## 5. Error handling

- **NONE, no valid `.mvpq`** (e.g. `build_multivector_pq` not yet run): `token_source` is NULL → the segment
  falls back to loading the float `.mvec` (best-effort), so results stay correct, just not the RAM-light path;
  `build_token_index`/compaction skip the graph for it.
- **Degraded/missing `.tann`**: existing per-segment forgiving-degrade (empty index → brute-force fallback via
  `multivector_candidates`), unchanged.
- **Borrowed-source lifetime**: teardown order fixed (token_index → token_source → stores) at the dtor and the
  compaction shuffle; an empty/absent token_index never derefs a freed source (the existing `!empty()` gate).
- **Token graph metric** stays DOT (tokens L2-normalized); the PQ ADC `token_score` uses the store's metric.

## 6. Testing

New `tests/test_pq_token_resident_tier.cpp` (existing V5/V6/token-PQ suites stay green):

- **FLOAT byte-identical lock:** a token-PQ index at FLOAT tier (and a V5/V6-only index) returns the identical
  `search_multivector` top-k (`filename`, `docid`, `score`) to the pre-#24 float path over identical data + a
  fixed multi-vector query — PQ+FLOAT does not perturb float token search.
- **NONE-tier end-to-end:** `set_multivector_pq_config` + `set_multivector_resident_tier(NONE)` + token-index
  config, add docs, `flush`, `build_multivector_pq`, `build_token_index`; assert `disk_segment_resident_tier_mv(0)
  == MV_TIER_NONE` (no float pool resident) AND `search_multivector` recall@10 vs an exact-MaxSim reference ≥ a
  sane floor (planted-nearest recalled) AND the token graph engaged (non-empty `.tann`).
- **Seam-engaged proof:** on a NONE-tier index, record `multivector_pq->adc_table_builds`, run one
  `search_multivector` with `num_query_vecs` query tokens over a graph that visits many nodes, and assert the
  counter rose by exactly `num_query_vecs` (one ADC table per query token, not per visited node).
- **Tier-change invalidates stale `.tann`:** build the `.tann` at FLOAT, then `set_multivector_resident_tier(NONE)`;
  assert the per-segment `.tann`/`.tann.g` were removed and the in-memory `token_index` nulled (fail-first: the
  test must fail before the invalidation is added).
- **Compaction under NONE:** two flushes, `build_multivector_pq`+`build_token_index`, `compact`; the merged
  segment's `.tann` is rebuilt over `multivector_pq` and `search_multivector` stays correct (planted doc
  recalled).
- **ASan/UBSan** clean on the new paths (PQ token source, borrowed-source teardown ordering, tier-select at load,
  compaction rebuild); `detect_leaks=1` on the compaction and teardown tests; the known out-of-scope
  `ANT_file::setvbuff` leak + legacy-lexical misalignment excluded.

## 7. Sequencing (TDD tasks)

1. `ANT_token_source` interface; retype `ANT_multivector_source` to it (+ `num_documents`/`token_docid_of`);
   `ANT_token_index` borrows `ANT_token_source*` (build/load/search_candidates) + a FLOAT byte-identical
   regression lock (float path unchanged).
2. `ANT_multivector_pq_source : ANT_token_source` (ADC score, token_docid_of) + the `#26` seam methods on
   `ANT_multivector_pq_store` (`token_prepare_query`/`token_score_prepared`/`token_free_query` + `adc_table_builds`)
   and the source overrides + unit tests (prepared==per-call equivalence, NULL fallback).
3. `set_multivector_resident_tier` + `multivector_pq.config` tier field + back-compat load + `disk_segment_resident_tier_mv`;
   `append_segment` tier-select (FLOAT loads float + float source; NONE skips float + PQ source);
   `set_multivector_resident_tier` deletes stale `.tann` on a real change + the invalidation regression test.
4. `build_token_index`/eager build the `.tann` over `token_source` (NONE → PQ source, best-effort ordering after
   `.mvpq`) + NONE-tier end-to-end recall test + seam-engaged (one table per query token) test.
5. Compaction rebuilds `.tann` over the merged `token_source`, refresh-before-rebuild ordering + teardown free +
   compaction-under-NONE recall test.
6. Recall sanity across tiers + ASan/UBSan sweep (`detect_leaks=1` on teardown/compaction).

## 8. Repo constraints (carried from the vector stack)

Whole repo `-fPIC`; **NO header dependency tracking → `rm -f obj/*.o lib/libantelope_engine.a` after any header
change** (`vector_source.h`, `multivector_store.h`, `multivector_pq_store.h`, `token_index.h`,
`atire_segment_index.h`); after an ASan sweep a full clean rebuild is required before a normal link; a fresh
worktree needs `mkdir -p obj bin lib` + a copy of the vendored `external/**/*.a`; `source/*.cpp` + `tests/*.cpp`
auto-discovered; tests build to `bin/<name>` via `make <name>` (`CHECK()` macro, exit 0 on pass); config setters
are POST-open (`set_multivector_pq_config`/`set_multivector_resident_tier`/`set_rerank_config` all require an open
directory); `set_rerank_config` must be called before token-PQ config; `add_document`'s multi-vector form is used
by `search_multivector`/rerank tests; `segment_filename` zero-pads the generation to 6 digits
(`seg_%06lld.<ext>`); the token graph metric is DOT; token ids are int32 (`ANT_HNSW_MAX_DOCUMENTS` cap → build
returns NULL → brute-force fallback).

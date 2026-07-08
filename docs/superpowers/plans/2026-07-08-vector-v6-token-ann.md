# Vector V6 — Token-Level ANN Retrieval Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make multi-vector (late-interaction) retrieval a first-class search path: a per-segment token-level HNSW over the existing `.mvec` token pool generates candidate documents from query tokens, which the exact V5 MaxSim then rescores.

**Architecture:** Generalize `ANT_hnsw` behind a tiny `ANT_vector_source` interface (so one graph implementation indexes either per-doc vectors or the flat token pool — V3 results byte-identical). A new `ANT_token_index` unit owns the token graph + a `token_docid[]` map and turns query tokens into candidate docids. The segment layer adds `search_multivector()` (candidate-gen → exact MaxSim rescore), a `.tann` sidecar with eager/on-demand build, filter + live-buffer + fallback integration, compaction rebuild, and a `search_rerank` both-NULL first-stage fallback.

**Tech Stack:** C++ (`source/`, `atire/`), reusing `ANT_hnsw` (V3), `ANT_multivector_store` (V5), `ANT_filter` (Filtered ANN), and the segment accumulator/compaction machinery. Engine-only; no bindings.

**Spec:** `docs/superpowers/specs/2026-07-08-vector-v6-token-ann-design.md`

**Milestones:** HNSW generalized + V3 regression-locked after Task 1; token graph + candidate generation unit-tested after Task 5; `search_multivector` first-class with ranking-equality after Task 6; filter + live + fallback after Task 8; eager/ondemand + backfill after Task 9; compaction + rerank-fallback after Task 11; tuned + sanitizer-clean after Task 12.

---

## Repo facts every task needs

- **Build:** `make all` (engine + externals) then `make engine_lib`. **NO header dependency tracking** — after editing ANY `.h`, run `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding or you link a stale/inconsistent archive (this has caused runtime SEGVs). Tasks that touch a header MUST `rm obj/*.o` in their build step.
- **Tests:** the engine's C++ tests live under the existing test harness (find the V5/HNSW tests with `grep -rln "search_rerank\|ANT_hnsw\|maxsim" test/ tests/ 2>/dev/null`; put V6 tests beside them, same harness/registration pattern). Each task's test step names the exact build+run command; mirror the nearest existing test's registration.
- **Gating:** everything multi-vector is gated on `engine->rerank_configured()` (`rerank_dimension_current != 0`). No rerank config ⇒ no `.mvec` ⇒ token index is a no-op and `search_multivector` errors like the rest of the multi-vector path.
- **Sidecars:** per segment `seg_<G>.vec/.qvec/.vsig/.hnsw/.mvec/.attr/.pay`. V6 adds `seg_<G>.tann` (token graph + token_docid map). Built at flush (eager) or by `build_token_index()` (ondemand); rebuilt best-effort at compaction; forgiving load (corrupt ⇒ absent ⇒ fallback).
- **Publish path:** searches store hits via the accumulator and expose them through `get_hit()`. `search_multivector` publishes the same way so tombstones/generations/payloads/cross-segment merge work unchanged. Read how `search_rerank` publishes (in `atire/atire_segment_index_vector.cpp`) and mirror it.
- **`.mvec` store (`source/multivector_store.{h,cpp}`):** ragged L2-normalized tokens; `pool_f` (float) or `pool_q`+`qmin`/`qmax` (int8); `counts[docid]`, `offsets[docid]` into the pool; `maxsim(docid, query_vecs, num_query_vecs)`, `copy_vectors(docid,out)`, `max_vector_count()`. Kernel = dot over normalized vectors.
- **`ANT_hnsw` (`source/hnsw.{h,cpp}`):** `build(source, M, ef_construction, metric, use_cache)`, `search(query, metric, ef_search, top_k, source, tombstones, out_docids, out_scores, filter_bits)`, `save/load`. Node ids int32, cap `ANT_HNSW_MAX_DOCUMENTS` (INT_MAX). Save/load persist topology only (already source-agnostic).
- **`ANT_vector_store` (`source/vector_store.h`)** already exposes the exact methods the graph needs: `document_count()`, `get_dimension()`, `has(id)`, `get(id)`, `is_quantized()`, `reconstruct(id,out)`, `score(id,query,metric)`.
- **`-fPIC` repo-wide; default ranker DFR not BM25; POST-open config setters; add_document handle = `make_handle(gen<<40|docid)`.**

---

## Task 1: Generalize `ANT_hnsw` behind `ANT_vector_source` (V3 regression-locked)

**Files:**
- Create: `source/vector_source.h`
- Modify: `source/vector_store.h` (make `ANT_vector_store` implement the interface)
- Modify: `source/hnsw.h`, `source/hnsw.cpp` (retarget `build`/`search`/`distance` to `ANT_vector_source*`)
- Test: the existing V3 HNSW test (regression lock — results must not change)

- [ ] **Step 1: Write `source/vector_source.h`** — the abstract seam, exactly the methods `hnsw.cpp` calls on the store:

```cpp
/*
	VECTOR_SOURCE.H -- read-only "N points of dimension D" abstraction that
	ANT_hnsw builds/searches over. Implemented by ANT_vector_store (node == docid)
	and ANT_multivector_source (node == token in the flattened .mvec pool).
*/
#ifndef VECTOR_SOURCE_H_
#define VECTOR_SOURCE_H_

class ANT_vector_source
{
public:
	virtual ~ANT_vector_source() {}
	virtual long long document_count(void) = 0;        // node count
	virtual long long get_dimension(void) = 0;
	virtual long has(long long node) = 0;              // 1 if node has a vector (token nodes: always 1 in range)
	virtual const float *get(long long node) = 0;      // float backend: zero-copy row; NULL/unused if quantized
	virtual long is_quantized(void) = 0;
	virtual void reconstruct(long long node, float *out) = 0;   // int8 backend -> float[dimension]
	virtual double score(long long node, const float *query, long metric) = 0;   // kernel; float+int8 backends
};
#endif /* VECTOR_SOURCE_H_ */
```

- [ ] **Step 2: Make `ANT_vector_store` implement it** — in `source/vector_store.h`: `#include "vector_source.h"`, change `class ANT_vector_store` to `class ANT_vector_store : public ANT_vector_source`, and mark the seven methods `virtual ... override` (their signatures already match exactly — `document_count/get_dimension/has/get/is_quantized/reconstruct/score`). No body changes.

- [ ] **Step 3: Retarget `ANT_hnsw`** — in `source/hnsw.h` forward-declare `class ANT_vector_source;` and change every `ANT_vector_store *vectors` parameter in `build()`, `search()`, and the private `distance()` to `ANT_vector_source *vectors`. In `source/hnsw.cpp` change the same signatures and `#include "vector_source.h"`; the bodies are unchanged (they only call the seven interface methods). Keep `#include "vector_store.h"` removed from hnsw.cpp if nothing else needs it (the distance-cache `bscratch` uses `is_quantized()/get_dimension()/reconstruct()/get()` — all on the interface).

- [ ] **Step 4: Update `ANT_hnsw` callers** — anywhere the segment code passes an `ANT_vector_store*` to `hnsw->build/search` still compiles (derived→base is implicit). Grep to confirm: `grep -rn "hnsw.*->build\|hnsw.*->search\|->hnsw_graph->" atire/ source/`. No cast needed.

- [ ] **Step 5: Rebuild (header changed)** — `rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib`. Expected: clean build.

- [ ] **Step 6: Run the V3 HNSW regression test** — build+run the existing HNSW test (locate via `grep -rln "ANT_hnsw" test*/`). Expected: **PASS with identical results** — this refactor is behavior-preserving; a single vtable indirection does not change graph topology or search output. If any V3 assertion changes, the refactor is wrong — fix before proceeding.

- [ ] **Step 7: Commit** — `git add source/vector_source.h source/vector_store.h source/hnsw.h source/hnsw.cpp && git commit` — message `refactor(v6): ANT_hnsw over ANT_vector_source interface (V3 byte-identical)`.

---

## Task 2: `ANT_multivector_source` over the token pool

**Files:**
- Modify: `source/multivector_store.h`, `source/multivector_store.cpp` (add the source adapter + token accessors)
- Test: new unit test `test/…` (mirror the nearest multivector test)

- [ ] **Step 1: Add token-level accessors to `ANT_multivector_store`** (public, in `source/multivector_store.h`) so an adapter can treat the pool as N points. Implement in the `.cpp` using the existing `pool_f`/`pool_q`/`qmin`/`qmax`/`offsets`/`counts`/`total_vectors`/`quantized` members:

```cpp
long long token_count(void) { return total_vectors; }               // total tokens across all docs
long token_has(long long t) { return t >= 0 && t < total_vectors; } // dense pool: every index in range is present
const float *token_get(long long t);          // float pool: &pool_f[t*dimension]; returns NULL if quantized
void token_reconstruct(long long t, float *out);   // int8: dequantize pool_q[t] via qmin/qmax -> out[dimension]; float: memcpy
double token_score(long long t, const float *query, long metric);   // dot kernel over normalized token t vs query
long token_docid_of(long long t);             // docid owning token t (binary search over offsets/counts) -- used to build token_docid[]
```
(For `token_docid_of`, precompute nothing here — a simple loop/upper_bound over `offsets` is fine; Task 3 materializes the full `token_docid[]` array once.)

- [ ] **Step 2: Add the source adapter** in the same files:

```cpp
class ANT_multivector_source : public ANT_vector_source
{
private:
	ANT_multivector_store *store;
public:
	ANT_multivector_source(ANT_multivector_store *s) : store(s) {}
	long long document_count(void) { return store->token_count(); }        // node == token
	long long get_dimension(void) { return store->get_dimension(); }
	long has(long long node) { return store->token_has(node); }
	const float *get(long long node) { return store->token_get(node); }
	long is_quantized(void) { return /* store->quantized */ store->tokens_quantized(); }
	void reconstruct(long long node, float *out) { store->token_reconstruct(node, out); }
	double score(long long node, const float *query, long metric) { return store->token_score(node, query, metric); }
};
```
Add a `long tokens_quantized(void) { return quantized; }` accessor to the store. `#include "vector_source.h"` in `multivector_store.h`.

- [ ] **Step 2b: Failing test** `test_v6_source.cpp` (or add to the multivector test): build a store with 2 docs (M=[2,3] tokens, known unit vectors), wrap in `ANT_multivector_source`, assert `document_count()==5`, `get_dimension()==D`, and `score(t, q, DOT)` for a token equals the direct dot product of that token with `q` (float case). Add a quantized-store case asserting `score`/`reconstruct` round-trip within int8 tolerance.

- [ ] **Step 3: Run → fail (adapter/accessors absent), implement, Step 4: rebuild (`rm obj/*.o` — header changed) + run → pass.**

- [ ] **Step 5: Commit** — `feat(v6): ANT_multivector_source + token accessors over the .mvec pool`.

---

## Task 3: `ANT_token_index` — build token graph + `token_docid[]`

**Files:**
- Create: `source/token_index.h`, `source/token_index.cpp`
- Modify: the engine makefile/objs list if it enumerates sources (`grep -n "multivector_store" GNUmakefile`)
- Test: new `test_v6_token_index.cpp`

- [ ] **Step 1: Declare `ANT_token_index`** (`source/token_index.h`) — owns the graph + the token→doc map + a borrowed store pointer:

```cpp
#ifndef TOKEN_INDEX_H_
#define TOKEN_INDEX_H_
class ANT_hnsw; class ANT_multivector_store; class ANT_index_tombstones; class ANT_filter;

class ANT_token_index
{
private:
	ANT_hnsw *graph;               // over the flattened token pool (nodes == tokens)
	int *token_docid;              // [token_count] node->docid map (int32; token_count <= INT_MAX)
	long long token_count;
	long long documents;
	long long dimension;
	long metric;
	long long M, ef_construction;
	ANT_token_index();
public:
	~ANT_token_index();
	// Build over `store` (borrowed); returns 0 on success, nonzero on alloc/cap failure (caller falls back).
	static ANT_token_index *build(ANT_multivector_store *store, long long M, long long ef_construction, long metric);
	long long get_token_count(void) { return token_count; }
	long empty(void);              // graph missing/empty
	// candidate generation defined in Task 5
	long save(const char *filename);
	static ANT_token_index *load(const char *filename, ANT_multivector_store *store, long long expected_M, long long expected_ef_construction, long metric);
};
#endif
```

- [ ] **Step 2: Implement `build`** (`source/token_index.cpp`): allocate `token_docid[token_count]` and fill it by walking `store` docs (`for docid: for j in counts[docid]: token_docid[t++] = docid`) — expose a helper on the store or use `offsets`/`counts`. Wrap the store in an `ANT_multivector_source` (stack), `graph = new ANT_hnsw; graph->build(&source, M, ef_construction, metric, true)`. If `store->token_count() > ANT_HNSW_MAX_DOCUMENTS` or `build()` returns nonzero, delete and return NULL (caller falls back to brute force). Store `documents/dimension/metric/M/ef_construction`.

- [ ] **Step 3: Failing test** `test_v6_token_index.cpp`: 3 docs with known token counts; `ANT_token_index::build(...)`; assert `get_token_count()` == sum of counts; assert internal `token_docid[]` maps boundary tokens to the right docids (expose a `long token_docid_at(long long t)` test accessor). Assert `build` returns NULL cleanly for an empty store (0 tokens) and the caller can handle NULL.

- [ ] **Step 4: Run → fail, implement, rebuild (`rm obj/*.o`), run → pass.**

- [ ] **Step 5: Commit** — `feat(v6): ANT_token_index build + token_docid map`.

---

## Task 4: `ANT_token_index` save / load (forgiving)

**Files:** Modify `source/token_index.h`, `source/token_index.cpp`; Test extends `test_v6_token_index.cpp`.

- [ ] **Step 1: Implement `save(filename)`** — write a small container: magic `"ANTTANN1"` (u64), version u32, `token_count` i64, `M`/`ef_construction` i64, then `token_docid[token_count]` (i32), then delegate the graph topology to a nested write **reusing `ANT_hnsw::save` semantics**. Simplest robust approach: write `token_docid[]` to `<filename>` container header, and call `graph->save("<filename>.g")` for the CSR (two files: `.tann` map+header, `.tann.g` graph). Atomic: write `.tmp` + rename each. (If you prefer one file, inline the CSR after the map — but reuse the exact byte layout `ANT_hnsw::save` uses so `ANT_hnsw::load` can read it; two-file is simpler and equally forgiving.)

- [ ] **Step 2: Implement `load(filename, store, expected_M, expected_ef_construction, metric)`** — forgiving factory (mirror `ANT_hnsw::load`): open the container; on missing file / bad magic / version / `token_count != store->token_count()` / short read / size mismatch → return an **empty** `ANT_token_index` (graph NULL, `empty()` true) so the caller uses brute-force fallback. On success, read `token_docid[]`, `graph = ANT_hnsw::load("<filename>.g", token_count, expected_ef_construction, token_count)` (note: HNSW `expected_documents` = token_count), and validate `graph` non-empty; any failure → empty index.

- [ ] **Step 3: Failing tests**: (a) build → save → load round-trips `token_count` and `token_docid[]`; (b) `load` of a nonexistent path returns `empty()==true`; (c) truncate/corrupt the container file → `load` returns `empty()==true` (no crash); (d) a `token_count` mismatch vs the store → `empty()`.

- [ ] **Step 4: Run → fail, implement, rebuild (`rm obj/*.o`), run → pass.**

- [ ] **Step 5: Commit** — `feat(v6): ANT_token_index forgiving save/load (.tann sidecar)`.

---

## Task 5: `ANT_token_index::search_candidates` — token retrieval + accumulation

**Files:** Modify `source/token_index.h`, `source/token_index.cpp`; Test extends `test_v6_token_index.cpp`.

- [ ] **Step 1: Declare** the candidate generator:

```cpp
// For each of num_query_vecs query tokens (row-major query[num*dimension]), retrieve the
// token_top_p nearest doc-tokens, map to docids, accumulate provisional scores (sum of best
// token-kernel per query token), and return up to `max_candidates` docids by provisional score.
// filter_bits (per-docid bitset) admits candidates when non-NULL (over-gathered by the caller).
// Returns the number of candidates written to out_docids (capacity max_candidates).
long long search_candidates(const float *query, long long num_query_vecs,
	long long token_top_p, long long max_candidates,
	ANT_index_tombstones *tombstones, const unsigned char *filter_bits,
	long long *out_docids);
```

- [ ] **Step 2: Implement** — if `empty()` return 0 (caller falls back). Use an `ANT_multivector_source` over the store for `graph->search`. Maintain a `docid -> provisional` map (a `std::vector<double>` of size `documents`, plus a small "touched docids" list to avoid an O(documents) scan; or a hash map). For each query token row `q_i`:
  - `graph->search(q_i, metric, ef_search=max(token_top_p, ...), token_top_p, &source, tombstones, tok_docids, tok_scores, /*filter_bits*/NULL)` → nearest **token** node ids + kernels. (Pass `filter_bits=NULL` to the graph — filtering is applied at the DOC level below, since a token's admission depends on its owning doc.)
  - For each returned token `t` with kernel `s`: `d = token_docid[t]`; skip if `tombstones->is_deleted(d)`; skip if `filter_bits && !bit(d)`; update `provisional[d] = max(prev_for_this_query_token, s)` accumulated as a running sum of per-query-token bests (track which query-token contributed to avoid double-counting within one query token — simplest correct rule: for each query token add only its single best kernel per doc).
  - Collect touched docids, partial-sort by provisional desc, write top `max_candidates` to `out_docids`. Return the count.

- [ ] **Step 3: Failing test**: construct 4 docs whose token sets make the nearest-token ordering knowable; call `search_candidates` with a 2-token query; assert the returned candidate docids are exactly the docs owning the nearest tokens, in provisional-score order, capped at `max_candidates`. Add a filtered case (a `filter_bits` excluding one candidate doc → it is absent). Add a tombstoned-doc case (deleted doc absent).

- [ ] **Step 4: Run → fail, implement, rebuild (`rm obj/*.o`), run → pass.**

- [ ] **Step 5: Commit** — `feat(v6): token-ANN candidate generation (search_candidates)`.

---

## Task 6: Segment wiring — `search_multivector` + ranking-equality + brute-force fallback

**Files:**
- Modify: `atire/atire_segment_index.h` (per-segment `ANT_token_index *token_index;` field in the segment struct; declare `search_multivector` + an impl helper), `atire/atire_segment_index_vector.cpp` (impl), `atire/atire_segment_index.cpp` (segment open loads `.tann`; segment dtor deletes `token_index`)
- Test: new `test_v6_search_multivector.cpp`

- [ ] **Step 1: Add the segment field + load** — in the per-segment struct (beside `multivectors`/`hnsw_graph`) add `ANT_token_index *token_index;` (init NULL). In the segment open path (where `.mvec`/`.hnsw` are loaded, `atire_segment_index.cpp`), when `rerank_configured()`, `token_index = ANT_token_index::load(tann_name, multivectors, rerank uses hnsw M/ef? -> use dedicated token graph M/ef defaults, see Step 5)`. Delete it in the segment teardown next to `delete hnsw_graph`.

- [ ] **Step 2: Declare public API** in `atire/atire_segment_index.h`:
```cpp
long long search_multivector(const float *query_multivector, long long num_query_vecs, long long top_k);
long long search_multivector(const float *query_multivector, long long num_query_vecs, long long top_k, const ANT_filter *filter);
private: long long search_multivector_impl(const float *query, long long num_query_vecs, long long top_k, const ANT_filter *filter);
```

- [ ] **Step 3: Implement `search_multivector_impl`** in `atire/atire_segment_index_vector.cpp`, mirroring how `search_rerank` gathers candidates and publishes:
  - Guard: if `!rerank_configured()` → error/return like the rest of the multi-vector path (match `search_rerank`'s guard). `num_query_vecs<=0 || query==NULL` → 0 hits.
  - Per disk segment with a `multivectors` store: if `token_index && !token_index->empty()`, call `search_candidates(query, num_query_vecs, token_top_p, candidate_multiplier*top_k, tombstones, filter_bits_or_NULL, cand_docids)`; **else brute-force fallback** — every docid in the segment is a candidate (respect tombstones + filter). For each candidate docid compute the exact `multivectors->maxsim(docid, query, num_query_vecs)` and insert into a cross-segment `ANT_vector_candidate` top-k (reuse `ANT_vector_candidate_insert`, `generation` = segment generation).
  - Merge across segments and publish top_k through the same accumulator/`get_hit` path `search_rerank` uses. (Filter + live buffer come in Tasks 7–8; here pass `filter=NULL` and skip live.)
  - Public `search_multivector(...)` (both overloads) delegate to the impl (filter overload passes the filter; plain passes NULL).

- [ ] **Step 4: Ranking-equality failing test** `test_v6_search_multivector.cpp`: build an index with rerank configured, add N docs with known multi-vectors, `flush()`, `build_token_index()` (Task 9 provides it — for THIS task, since build_token_index lands in Task 9, drive the token index by calling the fallback path first: assert `search_multivector` **without** a built `.tann` returns the exact brute-force MaxSim top-k). Assertion: `search_multivector(Q, k)` returns the same docids in the same order as an independently computed exhaustive `maxsim` over all docs. This locks the fallback + rescore correctness before the ANN is even built.

- [ ] **Step 5: Token-graph M/ef defaults** — add members `token_index_M` / `token_index_ef_construction` (default e.g. 16 / 200, reuse HNSW defaults) + `token_top_p` (default 32) to the engine; `candidate_multiplier` already exists (default 4). Wire them into `search_candidates`/build. (Config setters in Task 9.)

- [ ] **Step 6: Run → fail, implement, rebuild (`rm obj/*.o`), run → pass.**

- [ ] **Step 7: Commit** — `feat(v6): search_multivector (candidate-gen -> exact MaxSim) + brute-force fallback`.

---

## Task 7: Filter support in `search_multivector` (over-gather, no under-fill)

**Files:** Modify `atire/atire_segment_index_vector.cpp`; Test extends `test_v6_search_multivector.cpp`.

- [ ] **Step 1: Failing test** — index with an attribute schema (`{"tenant":"string"}`) + multi-vectors; `search_multivector(Q, k, filter={eq tenant=acme})` returns only matching docs, and returns a FULL k when ≥k matches exist even if the unfiltered nearest tokens belong to non-matching docs (no under-fill). Add a case where the filter is highly selective (1 match) → exactly that doc. Assert the unfiltered call (`filter=NULL`) is unchanged from Task 6.

- [ ] **Step 2: Implement** — in `search_multivector_impl`, when `filter != NULL`: build the per-segment match bitset with the existing `evaluate_filter_for_segment(which, filter)` (disk) / `evaluate_filter_for_live(filter)` (live, Task 8), pass `filter_bits` into `search_candidates` (which admits at the doc level), and **over-gather**: raise the candidate cap (e.g. `candidate_multiplier*top_k` computed against admitted docs, and increase `token_top_p`/pool when the admit rate is low) so a selective filter never under-returns — mirror the "no under-fill" discipline from the shipped Filtered ANN (read `search_vector_impl`'s filtered arm for the exact over-pull pattern). Brute-force fallback path: filter the candidate docid loop by the same bitset. `filter==NULL` path must be byte-identical to Task 6.

- [ ] **Step 3: Run → fail, implement, rebuild, run → pass.**

- [ ] **Step 4: Commit** — `feat(v6): filtered search_multivector (over-gather, no under-fill)`.

---

## Task 8: Live-buffer merge (un-flushed docs via `maxsim_live`)

**Files:** Modify `atire/atire_segment_index_vector.cpp`; Test extends `test_v6_search_multivector.cpp`.

- [ ] **Step 1: Failing test** — add docs with multi-vectors, `flush()`, then add MORE docs with multi-vectors WITHOUT flushing; `search_multivector(Q, k)` must include the un-flushed docs (ranked correctly by exact MaxSim) alongside flushed ones. Add a filtered variant asserting the live docs honor the filter.

- [ ] **Step 2: Implement** — in `search_multivector_impl`, after the disk segments, scan the writer's live multi-vector buffer: for each live docid (respecting tombstones + `evaluate_filter_for_live` when filtering) compute `maxsim_live(docid, query, num_query_vecs)` and insert into the same cross-segment top-k with the live generation. (Mirror exactly how `search_rerank` folds the live buffer in — read it and copy the discipline.)

- [ ] **Step 3: Run → fail, implement, rebuild, run → pass.**

- [ ] **Step 4: Commit** — `feat(v6): live-buffer merge in search_multivector`.

---

## Task 9: `token_index` policy (eager/ondemand) + `build_token_index()` backfill

**Files:** Modify `atire/atire_segment_index.h`, `atire/atire_segment_index_vector.cpp`, `atire/atire_segment_index.cpp` (flush path); Test new `test_v6_build_token_index.cpp`.

- [ ] **Step 1: Declare** in `atire/atire_segment_index.h`:
```cpp
long build_token_index(void);                 // per-segment: build/rewrite .tann; 0 = success
long set_token_index_policy(int eager);       // POST-construct pre-open; 1 = eager, 0 = ondemand (default)
private: long token_index_eager;              // default 0
```

- [ ] **Step 2: Implement `build_token_index`** — for each disk segment with a `multivectors` store and `token_index == NULL`/stale: `ANT_token_index::build(multivectors, token_index_M, token_index_ef_construction, vector_metric)`, `->save(tann_name)`, and swap the in-memory `token_index` pointer (delete old). Best-effort per segment (a failed segment stays in fallback). Idempotent. Mirror `build_hnsw()`'s per-segment loop (`atire_segment_index_vector.cpp`).

- [ ] **Step 3: Implement eager-at-flush** — in the flush path (`atire_segment_index.cpp`, where `.mvec`/`.hnsw` are written when configured), when `token_index_eager && rerank_configured()`, build + save the `.tann` for the freshly written segment and load it into the segment's `token_index` (non-fatal on failure — leaves fallback). `set_token_index_policy` sets the flag pre-open.

- [ ] **Step 4: Failing tests** `test_v6_build_token_index.cpp`: (a) ondemand default: after `flush()` the segment has no `token_index` (fallback), after `build_token_index()` it does and `search_multivector` returns the SAME top-k as the fallback did (ANN result == fallback result on a small set where recall is complete); (b) eager: constructing with `set_token_index_policy(1)` then `flush()` yields a ready `token_index` with no explicit backfill; (c) `build_token_index()` with rerank unconfigured is a no-op/error consistent with the multi-vector path.

- [ ] **Step 5: Run → fail, implement, rebuild (`rm obj/*.o`), run → pass.**

- [ ] **Step 6: Commit** — `feat(v6): eager/ondemand token_index policy + build_token_index backfill`.

---

## Task 10: Compaction rebuilds `.tann`

**Files:** Modify `atire/atire_segment_index_compaction.cpp`; Test new `test_v6_compaction.cpp`.

- [ ] **Step 1: Failing test** — configure rerank; add enough docs across multiple flushes to trigger a merge (or call `maintain()`); `build_token_index()`; `maintain()`; assert the merged segment answers `search_multivector` correctly (same top-k as exhaustive MaxSim over the surviving docs) — i.e. `.tann` was rebuilt over the merged `.mvec` with correct docid renumbering. Add a case asserting that if the `.tann` rebuild is skipped/fails, results are still correct via fallback (delete the merged `.tann` file, reopen, search → correct).

- [ ] **Step 2: Implement** — in the compaction path, right after the `.mvec` sidecar is rewritten for the merged output (find the `.mvec` block, ~`atire_segment_index_compaction.cpp:203`), rebuild the `.tann` over the fresh merged `.mvec`: `ANT_token_index::build(merged_multivectors, token_index_M, token_index_ef_construction, metric)->save(merged_tann_name)`. Best-effort, non-fatal (mirror the `.mvec`/`.hnsw` best-effort blocks: a failed `.tann` leaves the merged segment retrievable via fallback, never aborts the merge). Ensure the merged segment's in-memory `token_index` is loaded (or left NULL → fallback → reload on next open).

- [ ] **Step 3: Run → fail, implement, rebuild, run → pass.**

- [ ] **Step 4: Commit** — `feat(v6): compaction rebuilds the .tann token index`.

---

## Task 11: `search_rerank` both-NULL → token-ANN first stage

**Files:** Modify `atire/atire_segment_index_vector.cpp` (the `search_rerank` impl); Test extends the rerank test.

- [ ] **Step 1: Failing test** — with rerank configured + a built token index: `search_rerank(query_text=NULL, query_vector=NULL, query_multivector=Q, num_query_vecs, first_stage_n, top_k)` returns the top_k MaxSim results (previously this both-NULL case was a guard/error). Assert it equals `search_multivector(Q, top_k)` when `first_stage_n >= candidate pool` (i.e. the rerank over a token-ANN first stage of size first_stage_n produces the MaxSim top-k). Keep an assertion that with text/vector present, `search_rerank` is unchanged.

- [ ] **Step 2: Implement** — in `search_rerank` (both overloads), when `query_text==NULL && query_vector==NULL` (the current both-NULL guard site), generate the first-stage candidate set via the token-ANN path (`search_candidates` per segment producing `first_stage_n` candidates, honoring the filter overload's filter), then run the existing MaxSim rerank over those `first_stage_n` and publish `top_k` — reusing the existing stage-2 code. When text/vector are present, the path is unchanged.

- [ ] **Step 3: Run → fail, implement, rebuild, run → pass.**

- [ ] **Step 4: Commit** — `feat(v6): search_rerank token-ANN first stage when text+vector absent`.

---

## Task 12: Recall sanity, defaults tuning, sanitizer pass

**Files:** Test new `test_v6_recall.cpp`; possibly tweak default `token_top_p`/`candidate_multiplier` in `atire/atire_segment_index.cpp`.

- [ ] **Step 1: Recall test** `test_v6_recall.cpp` — generate a synthetic set (e.g. 500 docs, M≈8 tokens each, dim 32, random unit vectors; a handful of "planted" docs sharing tokens with the query). Build the token index; assert `search_multivector` recall@k vs exhaustive MaxSim ≥ a threshold (e.g. ≥0.9 at k=10) at default knobs. If recall is below threshold, raise `token_top_p`/`candidate_multiplier` defaults until it passes on this fixture, then record the chosen defaults.

- [ ] **Step 2: Run → observe recall, tune defaults if needed, rebuild, run → pass.**

- [ ] **Step 3: Sanitizer pass** — build the V6 tests with ASan+UBSan (match how the repo runs sanitizers for other vector tests — `grep -rn "fsanitize" GNUmakefile test*/`) and run `test_v6_*`. Expected: **clean** (no leaks from `ANT_token_index`/`token_docid[]`, no OOB in candidate accumulation or the token→doc map, no UB in the graph over the token source). Fix any report.

- [ ] **Step 4: Commit** — `test(v6): recall sanity + tuned defaults + ASan/UBSan clean`.

---

## Final review + finish

After Task 12: dispatch a holistic code review over the V6 delta (memory: `ANT_token_index`/`token_docid[]`/graph ownership + forgiving-load leak paths; correctness: ranking-equality invariant, filter no-under-fill, live-buffer merge, compaction renumbering, the V3-byte-identical claim from Task 1; the int32 token-count cap). Fix Critical/Important with regression tests. Then finishing-a-development-branch — **verify the full engine test suite green on a clean build (`rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && <run tests>`)** before merging locally to master.

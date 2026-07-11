# PQ-HNSW Prepare-Per-Query ADC Table Seam Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the m*K ADC table once per HNSW search instead of once per visited node, via an optional `prepare_query`/`score_prepared`/`free_query` seam on `ANT_vector_source` that `ANT_hnsw::search` threads through.

**Architecture:** Three defaulted virtuals are added to `ANT_vector_source`; the default `score_prepared` ignores its ctx and calls `score()`, so `ANT_vector_store` and `ANT_multivector_source` are unaffected. `ANT_pq_store` overrides them to build/reuse/free the ADC table. `ANT_hnsw::distance` gains a trailing `void *ctx = NULL`; `ANT_hnsw::search` calls `prepare_query` once, threads the ctx through every `distance()`, and frees it on its single return. `build()` is untouched (calls `distance()` with the default `ctx == NULL`, so it falls back to `score()` — byte-identical).

**Tech Stack:** C++ engine (`source/`). Reuses `ANT_pq_codec::adc_table`/`adc_score` (unchanged), `ANT_hnsw`, `ANT_pq_store`, `ANT_vector_store`. Tests via the repo `CHECK()` macro, built to `bin/<name>` with `make <name>`.

**Spec:** `docs/superpowers/specs/2026-07-10-vector-pq-hnsw-prepared-query-design.md`.

**Repo constraints (apply throughout):**
- Whole repo is `-fPIC`. **No header dependency tracking** → after ANY change to `source/vector_source.h`, `source/pq_store.h`, or `source/hnsw.h`, run `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding, or you link stale objects (silent SEGV).
- `source/*.cpp` and `tests/*.cpp` are auto-discovered; a test at `tests/test_X.cpp` builds to `bin/test_X` via `make test_X`.
- After an ASan/UBSan sweep the objects are instrumented → a full clean rebuild (`rm -f obj/*.o lib/libantelope_engine.a bin/test_pq_hnsw_prepared`) is required before a normal (non-ASan) link.
- `ANT_pq_store::load(path, expected_dim, expected_docs, metric)`; `ANT_pq_store_writer::create(path, dim, m, metric)` / `append(vec)` / `finish()`.
- `ANT_vector_store::load(path, expected_dim, expected_docs)` (no metric arg); `ANT_vector_store_writer::create(path, dim)` / `append(vec)` / `finish()`.
- Metric enums (identical values across both stores and the codec): `METRIC_DOT=0, METRIC_COSINE=1, METRIC_L2=2` (`ANT_pq_codec::METRIC_COSINE`, `ANT_vector_store::METRIC_COSINE`).
- `ANT_hnsw::build(vectors, M, ef_construction, metric, use_distance_cache=true)` returns 0 on success; `ANT_hnsw::search(query, metric, ef_search, top_k, vectors, tombstones, out_docids, out_scores, filter_bits=NULL)` returns the result count.

---

## File Structure

- **`source/vector_source.h`** — the abstract source interface. Gains three defaulted virtuals (the seam). Responsibility unchanged: read-only "N points of dimension D" abstraction.
- **`source/pq_store.h` / `source/pq_store.cpp`** — the PQ-code source. Gains the three overrides + a public `adc_table_builds` counter (bumped wherever `adc_table()` is built). Responsibility unchanged.
- **`source/hnsw.h` / `source/hnsw.cpp`** — the graph. `distance()` gains a trailing `ctx`; `search()` prepares/threads/frees it. Responsibility unchanged.
- **`tests/test_pq_hnsw_prepared.cpp`** (new) — self-contained tests at the `ANT_pq_store` + `ANT_hnsw` + `ANT_vector_store` level (no segment-index machinery): store-level prepared==plain equivalence, NULL-ctx fallback, the counter contract (1 per prepare / N per score / 0 per score_prepared-reuse), the default-source no-op, and the end-to-end "search builds the table once, not per visited node" proof with an ADC-argmax recall check.

---

## Task 1: The seam on `ANT_vector_source` + `ANT_pq_store` overrides + counter

**Files:**
- Modify: `source/vector_source.h` (add three defaulted virtuals after `score`)
- Modify: `source/pq_store.h` (add `adc_table_builds` member + three override decls)
- Modify: `source/pq_store.cpp` (init counter in ctor; bump in `score()`; implement the three overrides)
- Test: `tests/test_pq_hnsw_prepared.cpp` (new — store-level tests only in this task)

- [ ] **Step 1: Write the failing test file**

Create `tests/test_pq_hnsw_prepared.cpp`:

```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../source/pq_store.h"
#include "../source/pq_codec.h"
#include "../source/vector_store.h"
#include "../source/hnsw.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)

/* Build a PQ store of n random dim-vectors with m subspaces; caller deletes + removes path. */
static ANT_pq_store *make_pq(long long dim, long long m, long long n, long metric, const char *path)
{
	remove(path);
	float *data = new float[n*dim];
	srand(7); for (long long i=0;i<n*dim;i++) data[i]=(float)(rand()%200-100)/100.0f;
	ANT_pq_store_writer w;
	CHECK(w.create(path, dim, m, metric) == 0);
	for (long long d=0; d<n; d++) CHECK(w.append(data + d*dim) == 0);
	CHECK(w.finish() == 0);
	delete [] data;
	ANT_pq_store *pq = ANT_pq_store::load(path, dim, n, metric);
	CHECK(pq != 0 && pq->document_count() == n);
	return pq;
}

/* prepared score == per-call score, and the NULL-ctx fallback == score(). */
static void test_prepared_equivalence(void)
{
	const char *path = "/tmp/test_pq_prepared_eq.pq";
	long long dim=32, m=8, n=60;
	ANT_pq_store *pq = make_pq(dim, m, n, ANT_pq_codec::METRIC_COSINE, path);
	float q[32]; for (int i=0;i<dim;i++) q[i]=(float)(rand()%200-100)/100.0f;

	void *ctx = pq->prepare_query(q, ANT_pq_codec::METRIC_COSINE);
	CHECK(ctx != 0);
	for (long long d=0; d<n; d+=7)
		{
		double prep  = pq->score_prepared(d, q, ANT_pq_codec::METRIC_COSINE, ctx);
		double plain = pq->score(d, q, ANT_pq_codec::METRIC_COSINE);
		CHECK(fabs(prep - plain) < 1e-9);		/* prepared table gives identical math */
		}
	CHECK(fabs(pq->score_prepared(3, q, ANT_pq_codec::METRIC_COSINE, NULL)
	         - pq->score(3, q, ANT_pq_codec::METRIC_COSINE)) < 1e-9);	/* NULL ctx -> fallback */
	pq->free_query(ctx);
	pq->free_query(NULL);		/* delete[] NULL: no-op, clean under ASan */
	delete pq; remove(path);
	printf("test_prepared_equivalence PASSED\n");
}

/* adc_table_builds: +1 per prepare_query, +1 per score(), +0 per prepared reuse. */
static void test_counter_contract(void)
{
	const char *path = "/tmp/test_pq_prepared_ct.pq";
	long long dim=32, m=8, n=30;
	ANT_pq_store *pq = make_pq(dim, m, n, ANT_pq_codec::METRIC_COSINE, path);
	float q[32]; for (int i=0;i<dim;i++) q[i]=(float)(rand()%200-100)/100.0f;

	long long b = pq->adc_table_builds;
	for (long long d=0; d<10; d++) pq->score(d, q, ANT_pq_codec::METRIC_COSINE);
	CHECK(pq->adc_table_builds - b == 10);		/* each score() builds the table once */

	long long b2 = pq->adc_table_builds;
	void *ctx = pq->prepare_query(q, ANT_pq_codec::METRIC_COSINE);
	CHECK(pq->adc_table_builds - b2 == 1);		/* prepare builds exactly once */
	for (long long d=0; d<10; d++) pq->score_prepared(d, q, ANT_pq_codec::METRIC_COSINE, ctx);
	CHECK(pq->adc_table_builds - b2 == 1);		/* prepared reuse: no new builds */
	pq->free_query(ctx);
	delete pq; remove(path);
	printf("test_counter_contract PASSED\n");
}

/* A non-overriding source (ANT_vector_store) inherits the no-op default. */
static void test_default_source_noop(void)
{
	const char *path = "/tmp/test_pq_prepared_def.vec";
	long long dim=16, n=20;
	remove(path);
	float *data = new float[n*dim];
	srand(11); for (long long i=0;i<n*dim;i++) data[i]=(float)(rand()%200-100)/100.0f;
	ANT_vector_store_writer w;
	CHECK(w.create(path, dim) == 0);
	for (long long d=0; d<n; d++) CHECK(w.append(data + d*dim) == 0);
	CHECK(w.finish() == 0);
	ANT_vector_store *vs = ANT_vector_store::load(path, dim, n);
	CHECK(vs != 0);
	float q[16]; for (int i=0;i<dim;i++) q[i]=data[i];		/* query == doc 0 */
	void *ctx = vs->prepare_query(q, ANT_vector_store::METRIC_COSINE);
	CHECK(ctx == 0);		/* default: no per-query structure */
	CHECK(fabs(vs->score_prepared(2, q, ANT_vector_store::METRIC_COSINE, ctx)
	         - vs->score(2, q, ANT_vector_store::METRIC_COSINE)) < 1e-9);
	vs->free_query(ctx);	/* no-op on NULL */
	delete vs; delete [] data; remove(path);
	printf("test_default_source_noop PASSED\n");
}

/* Placeholder for Task 2's HNSW seam test; defined there. */
void test_hnsw_prepares_once(void);

int main(void)
{
	test_prepared_equivalence();
	test_counter_contract();
	test_default_source_noop();
	/* test_hnsw_prepares_once();  <-- enabled in Task 2 */
	printf("ALL test_pq_hnsw_prepared PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run it to verify it fails to compile**

Run: `make test_pq_hnsw_prepared`
Expected: FAIL — compile error, `'class ANT_pq_store' has no member named 'prepare_query'` (and `score_prepared`, `free_query`, `adc_table_builds`).

- [ ] **Step 3: Add the three defaulted virtuals to `ANT_vector_source`**

In `source/vector_source.h`, immediately after the `score` pure-virtual (line 19), before the closing `} ;`:

```cpp
	virtual double score(long long node, const float *query, long metric) = 0;

	/* Optional per-query precomputation. Default: no-op — score_prepared ignores ctx and
	   calls score(). A source with a per-query-precomputable structure (PQ's ADC table)
	   overrides all three: prepare_query builds it once, score_prepared reuses it, free_query
	   releases it. Caller contract: prepare_query(q,metric) -> ctx; every score_prepared for
	   that search passes the SAME q and metric; free_query(ctx) is called exactly once. */
	virtual void  *prepare_query(const float *query, long metric) { (void)query; (void)metric; return 0; }
	virtual double score_prepared(long long node, const float *query, long metric, void *ctx)
	                    { (void)ctx; return score(node, query, metric); }
	virtual void   free_query(void *ctx) { (void)ctx; }
```

- [ ] **Step 4: Add the counter + override decls to `ANT_pq_store`**

The counter is a **public** member so the test can read it. In `source/pq_store.h`, in the `public:` block, after `~ANT_pq_store();` (line 25), add:

```cpp
	long long adc_table_builds;	// # of adc_table() builds (score() + prepare_query); test proves the seam engaged
```

And after the `score(...) override;` decl (line 34), add the three overrides:

```cpp
	double score(long long docid, const float *query, long metric) override;
	void  *prepare_query(const float *query, long metric) override;
	double score_prepared(long long docid, const float *query, long metric, void *ctx) override;
	void   free_query(void *ctx) override;
```

(No change to the `private:` block.)

- [ ] **Step 5: Init the counter in the ctor + bump it in `score()`; implement the three overrides**

In `source/pq_store.cpp`, in the constructor (after `codes = NULL;`, line 45):

```cpp
codes = NULL;
adc_table_builds = 0;
```

In `ANT_pq_store::score()`, bump the counter right after the `adc_table` build (line 185):

```cpp
ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);
adc_table_builds++;
double result = ANT_pq_codec::adc_score(codes + docid * m, m, table);
```

Add the three override implementations (place them immediately after `ANT_pq_store::score()` ends, before `scan_adc`, around line 193):

```cpp
/*
	ANT_PQ_STORE::PREPARE_QUERY / SCORE_PREPARED / FREE_QUERY
	--------------------------------------------------------
	Build the m*K ADC table once per query (prepare_query), reuse it across every
	node (score_prepared), free it once (free_query). ANT_hnsw::search threads the
	returned ctx through distance() so NONE-tier navigation builds the table once
	per search instead of once per visited node. The table encodes (query, metric);
	score_prepared ignores its own query/metric when ctx != NULL, which is sound
	because a single search uses one fixed query and metric.
*/
void *ANT_pq_store::prepare_query(const float *query, long metric)
{
if (documents == 0 || codebook == 0)
	return 0;								/* degraded store: ctx==NULL -> score_prepared falls back */
double *table = new double[m * (long long)ANT_pq_codec::K];
ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);
adc_table_builds++;
return table;
}

double ANT_pq_store::score_prepared(long long docid, const float *query, long metric, void *ctx)
{
if (ctx == 0)
	return score(docid, query, metric);		/* no prepared table -> per-call build (e.g. build path) */
if (!has(docid))
	return 0.0;
return ANT_pq_codec::adc_score(codes + docid * m, m, (double *)ctx);
}

void ANT_pq_store::free_query(void *ctx)
{
delete [] (double *)ctx;					/* delete[] NULL is a no-op */
}
```

- [ ] **Step 6: Rebuild (headers changed) and run the test**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_hnsw_prepared && ./bin/test_pq_hnsw_prepared`
Expected: PASS — prints `test_prepared_equivalence PASSED`, `test_counter_contract PASSED`, `test_default_source_noop PASSED`, `ALL test_pq_hnsw_prepared PASSED`.

- [ ] **Step 7: Commit**

```bash
git add source/vector_source.h source/pq_store.h source/pq_store.cpp tests/test_pq_hnsw_prepared.cpp
git commit -m "feat(#26): prepare_query/score_prepared/free_query seam on ANT_vector_source + ANT_pq_store ADC-table override"
```

---

## Task 2: Thread the ctx through `ANT_hnsw` (search only) + seam-engaged proof

**Files:**
- Modify: `source/hnsw.h` (add trailing `void *ctx = NULL` to the `distance` decl)
- Modify: `source/hnsw.cpp` (`distance()` routes through `score_prepared`; `search()` prepares/threads/frees the ctx)
- Test: `tests/test_pq_hnsw_prepared.cpp` (add `test_hnsw_prepares_once` + enable it in `main`)

- [ ] **Step 1: Write the failing HNSW seam test**

Append this function to `tests/test_pq_hnsw_prepared.cpp` (above `main`), and replace its forward-declaration:

```cpp
/* End-to-end: one search over a PQ-code graph builds the ADC table exactly ONCE
   (not per visited node), and the prepared-path result recalls the true ADC argmax. */
void test_hnsw_prepares_once(void)
{
	const char *path = "/tmp/test_pq_prepared_hnsw.pq";
	long long dim=32, m=8, n=60;
	ANT_pq_store *pq = make_pq(dim, m, n, ANT_pq_codec::METRIC_COSINE, path);
	ANT_hnsw graph;
	CHECK(graph.build(pq, 16, 100, ANT_pq_codec::METRIC_COSINE) == 0);

	float q[32]; for (int i=0;i<dim;i++) q[i]=(float)(rand()%200-100)/100.0f;
	long long ids[10]; double sc[10];
	long long before = pq->adc_table_builds;
	long long got = graph.search(q, ANT_pq_codec::METRIC_COSINE, 50, 10, pq, NULL, ids, sc, NULL);
	CHECK(got > 1);								/* traversal returned many nodes -> visited many */
	CHECK(pq->adc_table_builds - before == 1);	/* ADC table built ONCE per search, not per node */

	/* brute-force ADC argmax over all docs; assert the graph returned it among top-k */
	double *table = new double[m*256];
	ANT_pq_codec::adc_table(q, dim, m, pq->get_codebook(), ANT_pq_codec::METRIC_COSINE, table);
	long long best_d = -1; double best_s = -1e30;
	for (long long d=0; d<n; d++)
		{ double s = ANT_pq_codec::adc_score(pq->codes_for(d), m, table); if (s > best_s){best_s=s;best_d=d;} }
	delete [] table;
	int found = 0; for (long long h=0; h<got; h++) if (ids[h]==best_d) found=1;
	CHECK(found);								/* prepared-path search recalls the true ADC nearest */
	delete pq; remove(path);
	printf("test_hnsw_prepares_once PASSED\n");
}
```

And enable the call in `main` (uncomment the line):

```cpp
	test_default_source_noop();
	test_hnsw_prepares_once();
	printf("ALL test_pq_hnsw_prepared PASSED\n");
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make test_pq_hnsw_prepared && ./bin/test_pq_hnsw_prepared`
Expected: FAIL at `pq->adc_table_builds - before == 1` — because `ANT_hnsw::search` still calls `distance()` → `score()` per node, so the counter jumps by the number of visited nodes (>1), not 1.

- [ ] **Step 3: Add the trailing `ctx` to `distance()` in `source/hnsw.h`**

Change the private helper decl (line 33):

```cpp
	/* build helpers (defined in the .cpp) */
	double distance(long long a, const float *query, ANT_vector_source *vectors, long metric, void *ctx = NULL);
```

- [ ] **Step 4: Route `distance()` through `score_prepared` and thread the ctx in `search()`**

In `source/hnsw.cpp`, change `distance()` (lines 27-30):

```cpp
double ANT_hnsw::distance(long long a, const float *query, ANT_vector_source *vectors, long metric, void *ctx)
{
return -vectors->score_prepared(a, query, metric, ctx);	// ctx==NULL -> falls back to score() (build path, float/int8)
}
```

In `ANT_hnsw::search()`, after the early-empty guard and before `long long ep = entry_point;` (line 313), prepare the ctx:

```cpp
long long ef = ef_search < top_k ? top_k : ef_search;

/* admit = live AND (no filter OR filter bit set); a non-admitted node still ROUTES via C for connectivity */
#define ANT_HNSW_ADMIT(docid) \
	((tombstones == NULL || !tombstones->is_deleted(docid)) && \
	 (filter_bits == NULL || (filter_bits[(docid) >> 3] & (1 << ((docid) & 7)))))

void *qctx = vectors->prepare_query(query, metric);		/* build any per-query structure ONCE (PQ: the ADC table) */

long long ep = entry_point;
double dep = distance(ep, query, vectors, metric, qctx);
```

Pass `qctx` to the two remaining `distance()` calls — the upper-layer neighbour loop (was line 326) and the layer-0 `ecand` (was line 349):

```cpp
			double d = distance(nb[e], query, vectors, metric, qctx);
```
```cpp
			double de = distance(ecand, query, vectors, metric, qctx);
```

Free the ctx immediately before the terminal `return out;` (was line 372):

```cpp
	out_scores[out] = -found[i].first;		/* back to kernel: higher = nearer */
		out++;
		}
	vectors->free_query(qctx);				/* release the per-query structure (no-op for float/int8) */
	return out;
#undef ANT_HNSW_ADMIT
}
```

(The early `if (entry_point < 0 || documents == 0) return 0;` stays before `prepare_query`, so nothing is allocated on that path — the single `free_query` above covers every allocated path.)

- [ ] **Step 5: Rebuild (headers changed) and run the test**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_hnsw_prepared && ./bin/test_pq_hnsw_prepared`
Expected: PASS — prints `test_hnsw_prepares_once PASSED` and `ALL test_pq_hnsw_prepared PASSED`.

- [ ] **Step 6: Commit**

```bash
git add source/hnsw.h source/hnsw.cpp tests/test_pq_hnsw_prepared.cpp
git commit -m "feat(#26): ANT_hnsw::search prepares the ADC table once per query, threads ctx through distance()"
```

---

## Task 3: ASan/UBSan sweep + build-path byte-identical confirmation

**Files:**
- Verify only (no code change unless a sanitizer finds a defect): `source/hnsw.cpp`, `source/pq_store.cpp`, `tests/test_pq_hnsw_prepared.cpp`

- [ ] **Step 1: Clean, then build the test under ASan + UBSan**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a bin/test_pq_hnsw_prepared
make CC='g++ -fsanitize=address,undefined -g' test_pq_hnsw_prepared
```
Expected: builds `bin/test_pq_hnsw_prepared` with sanitizers linked.

- [ ] **Step 2: Run under the sanitizers with leak detection**

Run:
```bash
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_hnsw_prepared
```
Expected: prints `ALL test_pq_hnsw_prepared PASSED`, no ASan/UBSan reports. The seam test builds only `ANT_pq_store`/`ANT_vector_store`/`ANT_hnsw` (no segment index, so no `ANT_file::setvbuff` allocation) — there is no known out-of-scope leak to exclude here; `prepare_query`'s `new double[]` must be matched by `free_query`'s `delete[]` with zero leaked bytes. If any leak/UB is reported, fix it (the likely culprit would be a missing `free_query` on a search return path) and re-run before committing.

- [ ] **Step 3: Confirm `build()` is byte-identical (unchanged path)**

Run:
```bash
git diff HEAD~2 -- source/hnsw.cpp | grep -nE 'dist_stored|dist_ids|build\(' 
```
Expected: no lines — the diff touched only `distance()`'s signature/body and `search()`. Verify by inspection that `build()`'s `dist_stored`/`dist_ids` still call `distance(a, ..., vectors, metric)` with **no** ctx argument (so they use the default `ctx == NULL` → `score_prepared` falls back to `score()`), meaning the constructed graph is byte-identical to before this change. No code change expected in this step.

- [ ] **Step 4: Clean rebuild for a normal (non-ASan) link and final run**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a bin/test_pq_hnsw_prepared
make test_pq_hnsw_prepared && ./bin/test_pq_hnsw_prepared
```
Expected: `ALL test_pq_hnsw_prepared PASSED` (confirms the instrumented objects were fully replaced).

- [ ] **Step 5: Commit (only if a sanitizer fix was applied in Step 2; otherwise skip)**

```bash
git add source/hnsw.cpp source/pq_store.cpp tests/test_pq_hnsw_prepared.cpp
git commit -m "fix(#26): address sanitizer finding in prepare-per-query seam"
```

---

## Self-Review

**1. Spec coverage:**
- Spec §1 (three defaulted virtuals, void* ctx rationale) → Task 1 Step 3. ✓
- Spec §2 (`ANT_pq_store` overrides + `adc_table_builds` counter bumped in `prepare_query` and `score()`) → Task 1 Steps 4–5. ✓
- Spec §3 (`distance()` gains `ctx`; `search` prepares after the early guard, threads all three call sites, frees before the single terminal return; `build()` untouched) → Task 2 Steps 3–4 + Task 3 Step 3. ✓
- Spec §4 store-level equivalence + NULL fallback → `test_prepared_equivalence`; seam-engaged "1 not N" + N-per-score sanity → `test_counter_contract`; end-to-end recall/argmax → `test_hnsw_prepares_once`; default source untouched → `test_default_source_noop`; ASan/UBSan → Task 3. ✓ (Realized at the `ANT_hnsw`/store level rather than via `search_vector_hnsw`, which is a more direct and deterministic proof of "built once"; the recall guarantee is preserved by the ADC-argmax check.)
- Spec §6 repo constraints (`rm obj/*.o` after header changes, POST/PRE-open setters, clean rebuild after ASan) → embedded in every build step. ✓

**2. Placeholder scan:** No "TBD"/"handle edge cases"/"similar to Task N" — every code step shows complete code. The Task 1 Step 4 note explicitly retracts its own location-pointer sub-bullet so nothing spurious is added to `private:`. ✓

**3. Type/signature consistency:** `prepare_query(const float*, long) -> void*`, `score_prepared(long long, const float*, long, void*) -> double`, `free_query(void*)`, and `distance(long long, const float*, ANT_vector_source*, long, void* = NULL)` are spelled identically in the header decls (Task 1 Step 3–4, Task 2 Step 3) and the implementations (Task 1 Step 5, Task 2 Step 4) and the test calls. Counter name `adc_table_builds` is consistent across header, ctor init, `score()` bump, `prepare_query` bump, and all three tests. Metric constant `METRIC_COSINE` used consistently (`ANT_pq_codec::` for PQ/codec, `ANT_vector_store::` for the float store). ✓

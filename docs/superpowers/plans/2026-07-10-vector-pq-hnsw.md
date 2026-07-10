# PQ-backed HNSW (tier-selected graph source) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and search the HNSW graph over `segments[].vectors ?: pq_vectors`, so HNSW + PQ + the #19 resident tier compose — FLOAT navigates over float (byte-identical), INT8 over the int8 `.pqr`, NONE over `pq_vectors` (ADC over m-byte codes) = a memory-light resident ANN with zero float in RAM — plus the M1 `score()` 128 KB-stack fix.

**Architecture:** One source-selection rule, `graph_source(which) = segments[which].vectors ? vectors : pq_vectors`, applied at every build/search site. The graph stores only adjacency and binds the source at build+search time; the #19 tier is immutable, so build-source and search-source always match. Reached via `search_vector_hnsw` (routing unchanged).

**Tech Stack:** C++ (`source/`, `atire/`). Reuses `ANT_hnsw` (build/search take `ANT_vector_source *`), `ANT_pq_store` (already an `ANT_vector_source`; `test_pq_hnsw` already builds/searches a graph over PQ codes), and the build/compaction/eager machinery. Builds on the #19 resident-tier stack.

**Spec:** `docs/superpowers/specs/2026-07-10-vector-pq-hnsw-design.md`

**Milestones:** score() capped after Task 1; graph builds over the tier source after Task 2; NONE/INT8/FLOAT search works after Task 3; compaction rebuilds the tier graph after Task 4; recall + sanitizer after Task 5.

---

## Repo facts every task needs

- **Build:** `make all && make engine_lib`. **NO header dependency tracking** — after editing ANY `.h`, `rm -f obj/*.o lib/libantelope_engine.a` first. **Worktree already set up:** branch `feature/vector-pq-hnsw` under `.worktrees/vector-pq-hnsw`, `obj/bin/lib` present, `external/**/*.a` copied. `cd` there for every command.
- **Tests:** `tests/*.cpp` auto-discovered; a new `source/*.cpp` auto-links. Build+run: `make <name> && ./bin/<name>`. `#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)`, a `main()` printing `... PASSED`, exit 0. Mirror `tests/test_pq_hnsw.cpp` and `tests/test_pq_resident_tier.cpp`.
- **ASan/UBSan:** `make all engine_lib <tests> CC='g++ -fsanitize=address,undefined -g'`, run with `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1`. Out of scope: the pre-existing `ANT_file::setvbuff` leak (`source/file.cpp:57`) and legacy-lexical (`memory_index*`, `bitstring`, etc.) misalignment. After an ASan sweep, `rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib` before a normal link. `detect_leaks=1` exits nonzero purely from the setvbuff leak and LSan's `_exit()` drops buffered stdout — confirm PASS on a normal (non-ASan) run.
- **Setup order:** `set_vector_config(dim, metric)` is PRE-open; then `open(dir)`; then the POST-open setters `set_hnsw_config(M, ef_construction)` (needs `directory` + dimension), `set_pq_config(m,posture,rq)`, `set_pq_resident_tier(tier)`. Then add docs → `flush` → `build_pq` → `build_hnsw`. So the exact order is: `new` → `set_vector_config` → `open` → `set_hnsw_config` → `set_pq_config` → `set_pq_resident_tier`. On reopen, `open` restores all config. `add_document(key, doc_string, float_vec)` is the 3-arg vector overload. `search_vector_hnsw(q,k)` / `search_vector(q,k)` return the result count; `search_vector_hnsw` transparently falls back to exact for `VECTOR_METRIC_DOT`, so test HNSW with COSINE (or L2). Hits: `get_hit(i)->{filename,generation,docid,score}`. Accessors: `disk_segment_has_hnsw(which)`, `disk_segment_has_pq(which)`, `disk_segment_resident_tier(which)`, `disk_segment_generation(which)`.
- **Key seam:** `ANT_hnsw::distance(a,query,vectors,metric) = -vectors->score(a,query,metric)` — the per-node hook. `ANT_hnsw::build(ANT_vector_source*, M, ef_construction, metric)` and `ANT_hnsw::search(query, metric, ef, top_k, ANT_vector_source*, tombstones, cand_docids, cand_scores, filter_bits)`. `ANT_vector_store *` and `ANT_pq_store *` both upcast to `ANT_vector_source *`. `ANT_pq_store::scan_adc(query, metric, tombstones, generation, best, &best_count, top_k, filter_bits)` is the linear ADC scan (mirrors `ANT_vector_store::scan`).
- **#19 tier reminders:** `segments[].vectors` is float under FLOAT, int8 `.pqr` under INT8, NULL under NONE; `pq_vectors` is the `ANT_pq_store` (resident whenever `.pq` valid). `PQ_TIER_FLOAT/INT8/NONE`. Enum `PQ_POSTURE_REPLACE/RERANK`.

---

## Task 1: `score()` stack cap + heap-above

**Files:** Modify `source/pq_store.cpp`, `source/pq_store.h` (add the cap constant); Test `tests/test_pq_hnsw_tiered.cpp` (create).

- [ ] **Step 1: Write the failing test** — `tests/test_pq_hnsw_tiered.cpp` starts with a `score()` correctness check at a large `m` (so `m*K` exceeds the cap and the heap path is taken). Build a small `ANT_pq_store` via its writer and compare `score()` to a brute-force `adc_table`+`adc_score`:

```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/pq_store.h"
#include "../source/pq_codec.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)

static const char *DIR = "/tmp/test_pq_hnsw_tiered_idx";

static void test_score_stack_cap(void)
{
	/* dim 64, m 32 -> m*K = 8192 doubles = 64KB table, above the stack cap -> heap path. */
	long long dim = 64, m = 32, n = 20, i, d;
	const char *path = "/tmp/test_pq_hnsw_tiered.pq";
	remove(path);
	float *data = new float[n*dim];
	srand(3); for (i = 0; i < n*dim; i++) data[i] = (float)(rand()%200-100)/100.0f;
	ANT_pq_store_writer w;
	CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT) == 0);
	for (d = 0; d < n; d++) CHECK(w.append(data + d*dim) == 0);
	CHECK(w.finish() == 0);
	ANT_pq_store *pq = ANT_pq_store::load(path, dim, n, ANT_pq_codec::METRIC_DOT);
	CHECK(pq->document_count() == n);

	float q[64]; for (i = 0; i < dim; i++) q[i] = (float)(rand()%200-100)/100.0f;
	/* brute-force reference ADC score for doc 0 */
	double *table = new double[m*256];
	ANT_pq_codec::adc_table(q, dim, m, pq->get_codebook(), ANT_pq_codec::METRIC_DOT, table);
	double ref = ANT_pq_codec::adc_score(pq->codes_for(0), m, table);
	double got = pq->score(0, q, ANT_pq_codec::METRIC_DOT);
	CHECK(fabs(got - ref) < 1e-6);		/* heap-path score == brute force */
	delete [] table; delete pq; delete [] data;
	remove(path);
}

int main(void)
{
	test_score_stack_cap();
	printf("test_pq_hnsw_tiered PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run to verify it fails/passes-for-wrong-reason** — `cd .worktrees/vector-pq-hnsw && make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered`. Expected: PASS already (the current 128 KB stack buffer happens to fit `m*K=8192`). That is fine — this test locks correctness; the real change is the cap. To make the test *drive* the cap, ALSO assert the buffer is bounded by using `m` large enough to exceed the NEW cap (m=32 → 8192 > 2048 cap → heap path). The value check above is the guard. (If you want a hard fail-first, temporarily set `m=4` and confirm PASS, then `m=32`; keep `m=32` in the committed test.)

- [ ] **Step 3: Implement** — in `source/pq_store.h`, add near the top of the class or as a file-scope enum: `enum { PQ_SCORE_STACK_CAP = 8 * 256 };` (2048 doubles = 16 KB). In `source/pq_store.cpp`, rewrite `ANT_pq_store::score()` (currently ~line 174) to cap the stack buffer:

```cpp
double ANT_pq_store::score(long long docid, const float *query, long metric)
{
if (!has(docid))
	return 0.0;

double stack_table[PQ_SCORE_STACK_CAP];			/* 16 KB, safe on worker stacks */
double *table = stack_table;
long long table_size = m * (long long)ANT_pq_codec::K;
if (table_size > (long long)PQ_SCORE_STACK_CAP)
	table = new double[table_size];				/* larger m: heap the ADC table */

ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);
double result = ANT_pq_codec::adc_score(codes + docid * m, m, table);

if (table != stack_table)
	delete [] table;

return result;
}
```
(Behaviour and the returned value are unchanged — this is purely the stack-size/allocation-boundary fix. The per-call `adc_table` rebuild is intentionally kept; it is deferred to a perf follow-up filed in Task 5.)

- [ ] **Step 4: Run** — `rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered` → PASS. `make test_pq_hnsw && ./bin/test_pq_hnsw` (existing graph-over-PQ smoke) → still PASS.

- [ ] **Step 5: Commit** — `git add source/pq_store.h source/pq_store.cpp tests/test_pq_hnsw_tiered.cpp && git commit -m "fix(pq): cap score() ADC-table stack buffer (M1), heap above PQ_SCORE_STACK_CAP"`.

---

## Task 2: `build_hnsw` + eager build the graph over the tier source

**Files:** Modify `atire/atire_segment_index_vector.cpp` (`build_hnsw`), `atire/atire_segment_index.cpp` (eager flush block + after eager `build_pq`); Test `tests/test_pq_hnsw_tiered.cpp` (extend).

- [ ] **Step 1: Write the failing test** — a graph builds over the tier source under each PQ tier (NONE builds over `pq_vectors` with `vectors` NULL). Add a helper + tests:

```cpp
/* Build a dim-16 COSINE index at the given tier, N docs, flush, build_pq, build_hnsw. Returns the index. */
static ATIRE_segment_index *build_tier_hnsw(long tier, long posture, long long *gen_out)
{
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);	/* PRE-open */
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_hnsw_config(16, 100) == 0);				/* POST-open */
	CHECK(idx->set_pq_config(4, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_pq_resident_tier(tier) == 0);
	float v[16];
	for (int d = 0; d < 40; d++)
		{ char name[64]; snprintf(name,sizeof(name),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f; CHECK(idx->add_document(name,"body words here",v)>=0); }
	CHECK(idx->flush() == 0);
	*gen_out = idx->disk_segment_generation(0);
	CHECK(idx->build_pq() == 0);
	CHECK(idx->build_hnsw() == 0);
	return idx;
}

static void test_build_hnsw_over_tier_source(void)
{
	long long gen;
	ATIRE_segment_index *n = build_tier_hnsw(ATIRE_segment_index::PQ_TIER_NONE, ATIRE_segment_index::PQ_POSTURE_REPLACE, &gen);
	CHECK(n->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_NONE);	/* no float resident */
	CHECK(n->disk_segment_has_pq(0) == 1);
	CHECK(n->disk_segment_has_hnsw(0) == 1);			/* graph built over pq_vectors */
	delete n;
	ATIRE_segment_index *i8 = build_tier_hnsw(ATIRE_segment_index::PQ_TIER_INT8, ATIRE_segment_index::PQ_POSTURE_RERANK, &gen);
	CHECK(i8->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_INT8);
	CHECK(i8->disk_segment_has_hnsw(0) == 1);			/* graph built over int8 .pqr */
	delete i8;
	ATIRE_segment_index *f = build_tier_hnsw(ATIRE_segment_index::PQ_TIER_FLOAT, ATIRE_segment_index::PQ_POSTURE_RERANK, &gen);
	CHECK(f->disk_segment_has_hnsw(0) == 1);
	delete f;
}
```
Add `test_build_hnsw_over_tier_source();` to `main()`.

- [ ] **Step 2: Run to verify it fails** — `make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered`. Expected: FAIL on the NONE case at `disk_segment_has_hnsw(0)==1` — today `build_hnsw` reloads the float `.vec`; under NONE the float `.vec` exists on disk so it may actually build over float (wrong source, but a graph is present). The real driver is that `build_hnsw` must build over `graph_source`, verified for consistency by the search recall test in Task 3. If `disk_segment_has_hnsw` is already 1 for all tiers (because build_hnsw reloads float from disk), that is the bug this task fixes: the graph must be built over the *tier* source, not the disk float. Make the assertion meaningful by ALSO deleting the float `.vec` before `build_hnsw` under NONE (the float stays on disk normally, but this proves the graph builds from `pq_vectors`, not the disk float):

```cpp
	/* In build_tier_hnsw, for the NONE tier only, delete the on-disk float .vec after build_pq
	   and before build_hnsw, proving the graph is built from resident pq_vectors, not the disk float: */
	if (tier == ATIRE_segment_index::PQ_TIER_NONE)
		{ char vecp[2048]; snprintf(vecp,sizeof(vecp),"%s/seg_%06lld.vec",DIR,(long long)*gen_out); /* keep a backup */ char bak[2100]; snprintf(bak,sizeof(bak),"%s.bak",vecp); rename(vecp,bak); }
	CHECK(idx->build_hnsw() == 0);
	if (tier == ATIRE_segment_index::PQ_TIER_NONE)
		{ char vecp[2048]; snprintf(vecp,sizeof(vecp),"%s/seg_%06lld.vec",DIR,(long long)*gen_out); char bak[2100]; snprintf(bak,sizeof(bak),"%s.bak",vecp); rename(bak,vecp); }	/* restore for later compaction */
```
With the float `.vec` gone, today's `build_hnsw` (which reloads `.vec`) builds nothing → `disk_segment_has_hnsw(0)==0` → the test FAILS as intended.

- [ ] **Step 3: Implement** — rewrite `build_hnsw` (`atire/atire_segment_index_vector.cpp`, ~line 967) to build over the resident tier source instead of reloading the float `.vec`:

```cpp
long ATIRE_segment_index::build_hnsw(void)
{
long long which;
char hnsw_name[4096];

if (hnsw_M_current == 0)
	return 1;

for (which = 0; which < segment_count; which++)
	{
	long long generation = segments[which].generation;
	long long docs = segments[which].engine->get_document_count();

	segment_filename(hnsw_name, sizeof(hnsw_name), generation, "hnsw");
	ANT_hnsw *existing = ANT_hnsw::load(hnsw_name, hnsw_M_current, hnsw_ef_construction_current, docs);
	long long already = existing->node_count() == docs && docs > 0 && !existing->empty();
	delete existing;
	if (already)
		continue;

	/* Build over the resident tier source (float / int8 .pqr / pq_vectors under NONE). */
	ANT_vector_source *src = segments[which].vectors != NULL
		? (ANT_vector_source *)segments[which].vectors
		: (ANT_vector_source *)segments[which].pq_vectors;
	if (src != NULL && src->document_count() == docs && docs > 0)
		{
		ANT_hnsw graph;
		if (graph.build(src, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0)
			graph.save(hnsw_name);
		}
	}
return 0;
}
```
(`vec_name` and the `ANT_vector_store::load(vec_name...)` reload are removed. `ANT_vector_source` is already visible via `pq_store.h`/`vector_store.h` includes; if not, add `#include "../source/vector_source.h"`.)

- [ ] **Step 4: Eager path** (`atire/atire_segment_index.cpp`): the inline flush HNSW block (~line 1242) reloads the float `.vec` — keep that ONLY for non-PQ, and build the PQ-tier graph after the eager `build_pq()` so the tier source is ready. (a) Guard the inline block: change `if (hnsw_M_current != 0)` to `if (hnsw_M_current != 0 && !pq_configured())`. (b) After the eager `build_pq()` call (~line 1450, `if (pq_eager) build_pq();`), add:

```cpp
if (pq_eager)
	build_pq();
if (pq_eager && hnsw_M_current != 0)
	build_hnsw();				/* PQ tiers: build the graph over the just-realized tier source */
```
(Non-PQ keeps the inline float build, byte-identical. PQ + not-`pq_eager`: no eager graph; the user calls `build_pq()` then `build_hnsw()`, as the test does. PQ + `pq_eager`: `build_pq()` realizes the tier stores, then `build_hnsw()` builds over `graph_source`.)

- [ ] **Step 5: Run** — `rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered` → PASS. Guards: `make test_pq_hnsw test_segment_index test_pq_resident_tier && ./bin/test_pq_hnsw && ./bin/test_segment_index && ./bin/test_pq_resident_tier` → all PASS (non-PQ HNSW build unchanged; #19 intact).

- [ ] **Step 6: Commit** — `git add atire/atire_segment_index_vector.cpp atire/atire_segment_index.cpp tests/test_pq_hnsw_tiered.cpp && git commit -m "feat(pq): build_hnsw + eager build the graph over the tier source (NONE->pq_vectors)"`.

---

## Task 3: Gatherer source rule — search the graph over the tier source

**Files:** Modify `atire/atire_segment_index_vector.cpp` (`vector_candidates_hnsw`); Test `tests/test_pq_hnsw_tiered.cpp` (extend).

- [ ] **Step 1: Write the failing test** — NONE-tier `search_vector_hnsw` returns correct top-k via the graph (recall vs an exact float reference), and FLOAT-tier PQ+HNSW is byte-identical to a non-PQ float HNSW. Add a recall helper + tests:

```cpp
static double recall10(ATIRE_segment_index *idx, const float *q, const long *planted, int np)
{
	long long n = idx->search_vector_hnsw(q, 10);
	int hit = 0;
	for (int i = 0; i < np; i++)
		{ char want[64]; snprintf(want,sizeof(want),"doc%ld",planted[i]);
		  for (long long h=0; h<n && h<10; h++) if (strcmp(idx->get_hit(h)->filename, want)==0){hit++;break;} }
	return (double)hit/np;
}

static void test_none_tier_hnsw_search(void)
{
	long long gen;
	ATIRE_segment_index *idx = build_tier_hnsw(ATIRE_segment_index::PQ_TIER_NONE, ATIRE_segment_index::PQ_POSTURE_REPLACE, &gen);
	CHECK(idx->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_NONE);
	CHECK(idx->disk_segment_has_hnsw(0) == 1);
	/* query near doc7 (same formula as build_tier_hnsw) */
	float q[16]; for (int j=0;j<16;j++) q[j]=(float)((7*7+j*3)%13-6)/6.0f;
	long planted[1] = {7};
	long long n = idx->search_vector_hnsw(q, 10);
	CHECK(n >= 1);						/* graph engaged under NONE (no NULL-skip) */
	CHECK(recall10(idx, q, planted, 1) >= 1.0 - 1e-9);	/* planted nearest recalled via ADC graph */
	delete idx;
}

static void test_float_tier_hnsw_byte_identical(void)
{
	/* Non-PQ float HNSW reference over identical data + a PQ+FLOAT+HNSW index -> identical top-k. */
	const char *DA = "/tmp/test_pqhnsw_float_a", *DB = "/tmp/test_pqhnsw_float_b";
	char cmd[4096];
	snprintf(cmd,sizeof(cmd),"rm -rf %s %s && mkdir -p %s %s",DA,DB,DA,DB); system(cmd);
	ATIRE_segment_index *a = new ATIRE_segment_index();		/* non-PQ float HNSW */
	CHECK(a->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE)==0);
	CHECK(a->open(DA)==0); CHECK(a->set_hnsw_config(16,100)==0);		/* set_hnsw_config POST-open */
	ATIRE_segment_index *b = new ATIRE_segment_index();		/* PQ + FLOAT tier + HNSW */
	CHECK(b->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE)==0);
	CHECK(b->open(DB)==0); CHECK(b->set_hnsw_config(16,100)==0);
	CHECK(b->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0);
	CHECK(b->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_FLOAT)==0);
	float v[16];
	for (int d=0; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f;
		CHECK(a->add_document(nm,"body words here",v)>=0); CHECK(b->add_document(nm,"body words here",v)>=0); }
	CHECK(a->flush()==0); CHECK(a->build_hnsw()==0);
	CHECK(b->flush()==0); CHECK(b->build_pq()==0); CHECK(b->build_hnsw()==0);
	float q[16]; for (int j=0;j<16;j++) q[j]=(float)((5*7+j*3)%13-6)/6.0f;
	long long na = a->search_vector_hnsw(q,10), nb = b->search_vector_hnsw(q,10);
	CHECK(na == nb);
	for (long long i=0;i<na;i++)
		{ ATIRE_segment_index::hit *ha=a->get_hit(i), *hb=b->get_hit(i);
		  CHECK(strcmp(ha->filename, hb->filename)==0); CHECK(ha->docid==hb->docid); CHECK(ha->score==hb->score); }
	delete a; delete b;
}
```
Add both to `main()`.

- [ ] **Step 2: Run to verify it fails** — `make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered`. Expected: FAIL in `test_none_tier_hnsw_search` at `n >= 1` — the gatherer's `if (segments[which].vectors == NULL) continue;` skips NONE segments, so `search_vector_hnsw` returns 0.

- [ ] **Step 3: Implement** — rewrite the per-segment loop body of `vector_candidates_hnsw` (`atire/atire_segment_index_vector.cpp`, ~line 1762-1785). Replace the `if (segments[which].vectors == NULL) continue;` guard + the graph/else branches:

```cpp
for (which = 0; which < segment_count; which++)
	{
	ANT_vector_source *src = segments[which].vectors != NULL
		? (ANT_vector_source *)segments[which].vectors
		: (ANT_vector_source *)segments[which].pq_vectors;
	if (src == NULL)
		continue;							/* NONE with no pq_vectors: nothing resident to score */
	unsigned char *fbits = evaluate_filter_for_segment(which, filter);
	if (segments[which].hnsw_graph != NULL && !segments[which].hnsw_graph->empty()
		&& segments[which].hnsw_graph->node_count() == segments[which].engine->get_document_count())
		{
		long long c = segments[which].hnsw_graph->search(query, vector_metric, ef, ef,
			src, segments[which].tombstones, cand_docids, cand_scores, fbits);
		for (long long p = 0; p < c; p++)
			{
			double sc = segments[which].exact_vectors != NULL
				? segments[which].exact_vectors->score(cand_docids[p], query, vector_metric)
				: cand_scores[p];			/* PQ tiers: exact_vectors is NULL -> graph nav score (ADC/int8/float) */
			ANT_vector_candidate_insert(best, &best_count, top_k, sc, segments[which].generation, cand_docids[p]);
			}
		}
	else if (segments[which].vectors != NULL)		/* no graph: exact/int8 linear scan */
		{
		ANT_vector_store *s = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
		s->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
		}
	else if (segments[which].pq_vectors != NULL)	/* NONE, no graph: linear ADC scan */
		segments[which].pq_vectors->scan_adc(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
	delete [] fbits;
	}
```
(The `src` upcast picks the tier source; the graph search runs over it. The no-graph fallbacks cover FLOAT/INT8 via `scan` and NONE via `scan_adc`. `ANT_vector_source` visibility: it is already included transitively; if the compile complains, add `#include "../source/vector_source.h"` at the top of the file.)

- [ ] **Step 4: Run** — `make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered` → PASS. Guards: `make test_pq_hnsw test_segment_index test_pq_resident_tier test_pq_search && ./bin/test_pq_hnsw && ./bin/test_segment_index && ./bin/test_pq_resident_tier && ./bin/test_pq_search` → all PASS (non-PQ HNSW gather byte-identical; #19 paths intact).

- [ ] **Step 5: Commit** — `git add atire/atire_segment_index_vector.cpp tests/test_pq_hnsw_tiered.cpp && git commit -m "feat(pq): HNSW gatherer searches over the tier source (NONE engages ADC graph)"`.

---

## Task 4: Compaction rebuilds the graph over the tier source (reordered after the #19 refresh)

**Files:** Modify `atire/atire_segment_index_compaction.cpp`; Test `tests/test_pq_hnsw_tiered.cpp` (extend).

- [ ] **Step 1: Write the failing test** — NONE-tier, two flushes, `build_pq`+`build_hnsw`, `compact`; the merged segment's graph is rebuilt over `pq_vectors` and search stays correct:

```cpp
static void test_compaction_hnsw_over_tier(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE)==0);
	CHECK(idx->open(DIR)==0); CHECK(idx->set_hnsw_config(16,100)==0);		/* set_hnsw_config POST-open */
	CHECK(idx->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_NONE)==0);
	float v[16];
	for (int d=0; d<20; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f; CHECK(idx->add_document(nm,"body words here",v)>=0);} 
	CHECK(idx->flush()==0);
	for (int d=20; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%13-6)/6.0f; CHECK(idx->add_document(nm,"body words here",v)>=0);} 
	CHECK(idx->flush()==0);
	CHECK(idx->build_pq()==0); CHECK(idx->build_hnsw()==0);
	long long gens[2] = { idx->disk_segment_generation(0), idx->disk_segment_generation(1) };
	CHECK(idx->compact(gens, 2)==0);
	CHECK(idx->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_NONE);
	CHECK(idx->disk_segment_has_hnsw(0) == 1);					/* merged graph rebuilt over pq_vectors */
	float q[16]; for (int j=0;j<16;j++) q[j]=(float)((7*7+j*3)%13-6)/6.0f;
	long planted[1] = {7};
	CHECK(recall10(idx, q, planted, 1) >= 1.0 - 1e-9);			/* planted doc recalled after merge */
	delete idx;
}
```
Add to `main()`.

- [ ] **Step 2: Run to verify it fails** — `make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered`. Expected: FAIL — the compaction HNSW block (line ~453) runs BEFORE the #19 `.pq`/`.pqr` refresh (line ~482-568), so at build time `output_segment->pq_vectors` is not yet the merged store; and it builds over the reloaded float `.vec`, not the tier source. Under NONE the merged graph is either stale or float-built, so recall/consistency breaks.

- [ ] **Step 3: Implement** — two edits in `atire/atire_segment_index_compaction.cpp`:

  (a) **Delete** the existing HNSW block at ~line 449-470 (`/* V3: rebuild the merged segment's HNSW graph ... */` through the closing `}` of `if (hnsw_M_current != 0) { ... }`).

  (b) **Re-insert** it AFTER the #19 resident-tier refresh block — i.e. after the `else if (pq_configured() && pq_resident_tier_current == PQ_TIER_NONE ...)` block ends (~line 568), and before the `/* V5: refresh the merged segment's ... .mvec ... */` block (~line 569). Build over the merged segment's tier source:

```cpp
/*
	V3 (tier-aware, #20): rebuild the merged segment's HNSW graph over the
	tier source -- output_segment->vectors (float/int8) or ->pq_vectors under
	NONE.  Placed AFTER the #19 .pq/.pqr refresh so the tier source is the
	merged store, keeping build-source == search-source.  Best-effort.
*/
if (hnsw_M_current != 0)
	{
	char out_hnsw[4096];
	segment_filename(out_hnsw, sizeof(out_hnsw), output_generation, "hnsw");
	long long out_docs = output_segment->engine->get_document_count();
	ANT_vector_source *gsrc = output_segment->vectors != NULL
		? (ANT_vector_source *)output_segment->vectors
		: (ANT_vector_source *)output_segment->pq_vectors;
	if (gsrc != NULL && gsrc->document_count() == out_docs && out_docs > 0)
		{
		ANT_hnsw graph;
		if (graph.build(gsrc, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0)
			graph.save(out_hnsw);
		}
	delete output_segment->hnsw_graph;
	output_segment->hnsw_graph = ANT_hnsw::load(out_hnsw, hnsw_M_current, hnsw_ef_construction_current, out_docs);
	}
```
(The old block reloaded the float `.vec` into `out_vectors` and freed it; the new block uses the resident merged tier store, so that local + its `delete` are gone. `ANT_vector_source` is already visible in this file via the segment header; if not, add the include.)

- [ ] **Step 4: Run** — `make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered` → PASS. Guards: `make test_pq_compaction test_segment_index test_pq_resident_tier && ./bin/test_pq_compaction && ./bin/test_segment_index && ./bin/test_pq_resident_tier` → all PASS (non-PQ compaction HNSW unchanged; #19 compaction intact). Note: for a non-PQ index, `output_segment->vectors` is the merged float store (loaded by append_segment), so the reordered block still builds the float graph correctly — verify `test_segment_index`'s HNSW/compaction cases stay green.

- [ ] **Step 5: Commit** — `git add atire/atire_segment_index_compaction.cpp tests/test_pq_hnsw_tiered.cpp && git commit -m "feat(pq): compaction rebuilds the HNSW graph over the tier source (after #19 refresh)"`.

---

## Task 5: Cross-tier recall sanity + ASan/UBSan + file the perf follow-up

**Files:** Test `tests/test_pq_hnsw_tiered.cpp` (extend).

- [ ] **Step 1: Recall sanity test** — 200 docs, dim 32, COSINE, 3 planted near-query docs; build FLOAT / INT8 / NONE indexes with HNSW at the default `m` (`set_pq_config(0, REPLACE_or_RERANK, FLOAT)`), `build_pq`+`build_hnsw`, and assert `search_vector_hnsw` recall@10 ≥ a floor for each tier (all 3 planted recalled by FLOAT; NONE ≥ 0.8; INT8 between). Print the three recall numbers. Reuse `build_tier_hnsw` generalized to dim 32 / 200 docs / 3 planted, or add a dedicated builder.

- [ ] **Step 2: Run** — `make test_pq_hnsw_tiered && ./bin/test_pq_hnsw_tiered` → PASS with the three recall numbers printed. If a tier misses its floor on this synthetic set, plant the near-query docs with a clearer margin (they must be genuine nearest) before loosening any floor; record the observed numbers in the commit.

- [ ] **Step 3: ASan/UBSan sweep** —
```bash
cd .worktrees/vector-pq-hnsw
rm -f obj/*.o lib/libantelope_engine.a
make all engine_lib test_pq_hnsw_tiered test_pq_hnsw test_pq_compaction CC='g++ -fsanitize=address,undefined -g'
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_hnsw_tiered
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_hnsw
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_compaction
```
Expected: no ASan ERROR and no in-scope leak/UB on the new paths (graph build/search over `pq_vectors`, the gatherer source switch, compaction rebuild, `score()` heap path). Only the known out-of-scope `ANT_file::setvbuff` leak + legacy-lexical misalignment. Then restore the normal build: `rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && ./bin/test_segment_index`.

- [ ] **Step 4: File the perf follow-up** — open a GitHub issue for the deferred per-node ADC rebuild: "PQ HNSW: prepare-per-query ADC table seam (prepare_query/score_prepared through ANT_vector_source + ANT_hnsw) so NONE-tier graph navigation builds the ADC table once per search instead of per visited node." Reference #20. (Use `gh issue create`; if `gh` is unavailable, note the follow-up text in the commit message instead.)

- [ ] **Step 5: Commit** — `git add tests/test_pq_hnsw_tiered.cpp && git commit -m "test(pq): cross-tier HNSW recall sanity + ASan/UBSan clean; file per-node-ADC perf follow-up"`.

---

## Final review + finish

After Task 5: dispatch a holistic review over `git diff master...HEAD` focusing on:
- **Consistency invariant:** build-source == search-source for every tier (build_hnsw, eager, compaction all use `graph_source`; the gatherer searches the same); no path builds over float while searching over ADC/int8.
- **FLOAT byte-identicalness:** PQ+FLOAT+HNSW == non-PQ float HNSW (the lock test), and non-PQ HNSW build/gather/compaction unchanged.
- **NONE correctness:** graph engages (no NULL-skip), no-graph fallback uses `scan_adc`, `src==NULL` guarded; zero float resident.
- **score() cap:** heap-above boundary correct, no leak, returned value unchanged.
- **Compaction reorder:** the moved HNSW block runs after the #19 refresh and frees no store twice (Step-6 shuffle still frees `hnsw_graph`); the old float-reload local is gone.
- **Leaks/UB:** ASan/UBSan clean on the new paths.

Fix Critical/Important with regression tests. Then finishing-a-development-branch — verify the full suite green on a clean build before merging locally to master.

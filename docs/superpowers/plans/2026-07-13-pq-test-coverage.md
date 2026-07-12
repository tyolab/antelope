# PQ Test Coverage Gaps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the M2 (cosine/L2 end-to-end PQ search) and M3 (store bad-magic / size-consistent m-mismatch, config posture+quant round-trip) test-coverage gaps from the Phase-1 review.

**Architecture:** A new `tests/test_pq_metrics.cpp` mirrors `test_pq_search.cpp` for `VECTOR_METRIC_COSINE`/`VECTOR_METRIC_L2` (replace recall, rerank exact, mixed PQ/float-fallback); extend `test_pq_store.cpp` `forgiving_load` and `test_pq_config.cpp`; add two one-line public getters (`pq_posture()`/`pq_rerank_quant()`) so config persistence is observable.

**Tech Stack:** C++ engine tests (`tests/*.cpp`, auto-discovered, `make <name>` → `bin/<name>`, `CHECK()`).

**Spec:** `docs/superpowers/specs/2026-07-13-pq-test-coverage-design.md`.

**Repo constraints:** `source/*.cpp`+`tests/*.cpp` auto-discovered; tests build to `bin/<name>` via `make <name>`; **after the Task-2 header change (`atire_segment_index.h`) run `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding**; after ASan a full clean rebuild before a normal link; config setters POST-open (`set_pq_config` after `open`), `set_vector_config` PRE-open; `add_document(key, body, vec)` 3-arg dense form; metric enums `VECTOR_METRIC_DOT/COSINE/L2`; `set_pq_config(m, PQ_POSTURE_REPLACE|RERANK, RERANK_QUANT_FLOAT|INT8)`; `disk_segment_has_pq(which)`; `.pq` header (sequential freads, no padding): `magic[8]` + `u32 version` + `i64 dimension`(off 12) + `i64 documents`(off 20) + `i64 m`(off 28) + `i64 k`(off 36).

---

## File Structure

- `tests/test_pq_metrics.cpp` (new) — M2: cosine + L2 × {replace recall, rerank exact} + mixed PQ/float-fallback.
- `atire/atire_segment_index.h` — two inline getters `pq_posture()`, `pq_rerank_quant()`.
- `tests/test_pq_store.cpp` — two new `forgiving_load` cases (bad-magic, size-consistent invalid-m).
- `tests/test_pq_config.cpp` — posture+quant round-trip case.

---

## Task 1: M2 — cosine/L2 end-to-end (`tests/test_pq_metrics.cpp`)

**Files:**
- Create: `tests/test_pq_metrics.cpp`

- [ ] **Step 1: Write the new test file**

Create `tests/test_pq_metrics.cpp` (mirrors `test_pq_search.cpp`'s helpers):

```cpp
/*
	TEST_PQ_METRICS.CPP
	-------------------
	#21 M2: cosine/L2 end-to-end PQ search (replace recall floor, rerank exact top-1)
	and the mixed PQ/float-fallback mid-query ordering. Existing test_pq_search/recall/
	rerank cover only VECTOR_METRIC_DOT.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define DIM 16

static char *make_index_dir(void)
{
char buffer[64]; strcpy(buffer, "/tmp/ant_pqmetrics_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1]; strcpy(result, dir); return result;
}

/* well-separated cluster: doc i dominant on axis (i % DIM) */
static void make_vec(long long i, float *v)
{
for (int d = 0; d < DIM; d++) v[d] = 0.02f * (float)(((i * 7 + d) % 5) - 2);
v[i % DIM] += 3.0f;
}

static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{
float v[DIM]; char key[32], body[64];
for (long long i = lo; i < hi; i++)
	{ make_vec(i, v); sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld z</DOC>", i); CHECK(ix->add_document(key, body, v) >= 0); }
}

static long in_topk(ATIRE_segment_index *ix, long long n, const char *want)
{
for (long long i = 0; i < n; i++) if (strcmp(ix->get_hit(i)->filename, want) == 0) return 1;
return 0;
}

/* replace posture: planted nearest is recalled in top-k (ADC is approximate). */
static void test_replace_recall(long metric, const char *label)
{
const long long N = 12;					/* N <= DIM so each dominant axis is unique */
char *dp = make_index_dir();
ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->set_vector_config(DIM, metric) == 0);
CHECK(pq->open(dp) == 0);
CHECK(pq->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
add_docs(pq, 0, N); CHECK(pq->flush() == 0);
CHECK(pq->build_pq() == 0); CHECK(pq->disk_segment_has_pq(0) == 1);
float q[DIM]; make_vec(5, q);
long long n = pq->search_vector(q, 5);
CHECK(n >= 1);
CHECK(in_topk(pq, n, "doc-5"));				/* planted nearest recalled under `metric` */
delete pq; delete [] dp;
printf("test_replace_recall[%s] OK\n", label);
}

/* rerank posture: top-1 equals the exact float index (rerank rescores through resident float). */
static void test_rerank_exact(long metric, const char *label)
{
const long long N = 12;
char *de = make_index_dir();
ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->set_vector_config(DIM, metric) == 0);
CHECK(ex->open(de) == 0);
add_docs(ex, 0, N); CHECK(ex->flush() == 0);

char *dp = make_index_dir();
ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->set_vector_config(DIM, metric) == 0);
CHECK(pq->open(dp) == 0);
CHECK(pq->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
add_docs(pq, 0, N); CHECK(pq->flush() == 0);
CHECK(pq->build_pq() == 0);

float q[DIM]; make_vec(7, q);
CHECK(ex->search_vector(q, 5) >= 1);
CHECK(pq->search_vector(q, 5) >= 1);
CHECK(strcmp(pq->get_hit(0)->filename, ex->get_hit(0)->filename) == 0);	/* rerank top-1 == exact top-1 */
delete ex; delete pq; delete [] de; delete [] dp;
printf("test_rerank_exact[%s] OK\n", label);
}

/* mixed: segment 0 has .pq (ADC), segment 1 has none (float scan fallback) in one query.
   The global nearest is planted in segment 1, so a sign/scale mismatch between the ADC and
   float-scan scoring paths would demote it out of top-k. */
static void test_mixed_pq_float_fallback(long metric, const char *label)
{
char *dp = make_index_dir();
ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->set_vector_config(DIM, metric) == 0);
CHECK(pq->open(dp) == 0);
CHECK(pq->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
add_docs(pq, 0, 6); CHECK(pq->flush() == 0);		/* segment 0 */
CHECK(pq->build_pq() == 0);				/* seg 0 -> .pq */
add_docs(pq, 6, 12); CHECK(pq->flush() == 0);		/* segment 1, build_pq NOT re-run */
CHECK(pq->disk_segment_has_pq(0) == 1);
CHECK(pq->disk_segment_has_pq(1) == 0);			/* mixed state: seg1 falls back to float scan */

float q[DIM]; make_vec(8, q);				/* nearest = doc-8, which lives in segment 1 (float-fallback) */
long long n = pq->search_vector(q, 5);
CHECK(n >= 1);
CHECK(in_topk(pq, n, "doc-8"));				/* float-fallback segment's nearest survives the merge */
delete pq; delete [] dp;
printf("test_mixed_pq_float_fallback[%s] OK\n", label);
}

int main(void)
{
test_replace_recall(ATIRE_segment_index::VECTOR_METRIC_COSINE, "cosine");
test_replace_recall(ATIRE_segment_index::VECTOR_METRIC_L2, "l2");
test_rerank_exact(ATIRE_segment_index::VECTOR_METRIC_COSINE, "cosine");
test_rerank_exact(ATIRE_segment_index::VECTOR_METRIC_L2, "l2");
test_mixed_pq_float_fallback(ATIRE_segment_index::VECTOR_METRIC_COSINE, "cosine");
test_mixed_pq_float_fallback(ATIRE_segment_index::VECTOR_METRIC_L2, "l2");
printf("ALL test_pq_metrics PASSED\n");
return 0;
}
```

- [ ] **Step 2: Build and run**

Run: `make test_pq_metrics && ./bin/test_pq_metrics`
Expected: `ALL test_pq_metrics PASSED` (all 6 cases OK). These lock already-correct behavior, so they should PASS against current master. **If a case FAILS**, do not weaken it blindly — first check whether it reveals a real defect (e.g. a wrong L2 sign in the mixed path). If it's a genuine bug, STOP and report it (a source fix would be a separate finding); if the test's own expectation is wrong for a correct contract (e.g. an over-strict assertion on an approximate replace result), adjust the assertion to the correct contract and note why.

- [ ] **Step 3: Commit**

```bash
git add tests/test_pq_metrics.cpp
git commit -m "test(#21): cosine/L2 end-to-end PQ search (replace recall, rerank exact, mixed PQ/float fallback)"
```

---

## Task 2: M3 — store bad-magic / size-consistent mismatch + config posture+quant round-trip

**Files:**
- Modify: `atire/atire_segment_index.h` (two getters)
- Modify: `tests/test_pq_store.cpp` (`forgiving_load` +2 cases)
- Modify: `tests/test_pq_config.cpp` (posture+quant round-trip)

- [ ] **Step 1: Add the two getters to `atire/atire_segment_index.h`**

Immediately after `long long pq_m(void) { return pq_m_current; }` (the existing accessor), add:
```cpp
	long pq_posture(void) { return pq_posture_current; }
	long pq_rerank_quant(void) { return pq_rerank_quant_current; }
```

- [ ] **Step 2: Add the config posture+quant round-trip test to `tests/test_pq_config.cpp`**

Add a new test function (model it on the existing persistence test in that file — reuse its scratch-dir + open pattern) and call it from `main`:
```cpp
static void test_posture_quant_persist(void)
{
	char dir[64]; strcpy(dir, "/tmp/ant_pqcfg2_XXXXXX"); { char *d = mkdtemp(dir); CHECK(d != NULL); }
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
	delete ix;						/* close; pq.config persisted */

	ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);				/* load_pq_config restores posture+quant */
	CHECK(ix->pq_configured());
	CHECK(ix->pq_m() == 4);
	CHECK(ix->pq_posture() == ATIRE_segment_index::PQ_POSTURE_RERANK);
	CHECK(ix->pq_rerank_quant() == ATIRE_segment_index::RERANK_QUANT_INT8);
	delete ix;
	printf("test_posture_quant_persist OK\n");
}
```
Add the include `#include <unistd.h>` if not present (for `mkdtemp`), and call `test_posture_quant_persist();` in `main` before the final "ALL ... PASSED" print. Confirm the exact enum spellings + that `set_pq_config` accepts `(4, RERANK, INT8)` here (RERANK_QUANT_INT8 is the PQ rerank-tier quant and is valid; it is NOT V4 int8 quantization, which is the mutually-exclusive one).

- [ ] **Step 3: Add bad-magic + size-consistent invalid-m cases to `tests/test_pq_store.cpp` `forgiving_load`**

Inside `forgiving_load()`, after the existing "wrong document count" block and before `unlink(path);`, add (the valid `path` store was written with dim=16, m=4):
```cpp
/* bad magic: a byte-identical-size copy with the 8 magic bytes clobbered -> degraded empty */
char badmagic_path[64]; strcpy(badmagic_path, "/tmp/ant_pqbm_XXXXXX"); { int fd = mkstemp(badmagic_path); if (fd >= 0) close(fd); }
{
	FILE *in = fopen(path, "rb"); FILE *out_f = fopen(badmagic_path, "wb");
	CHECK(in != NULL && out_f != NULL);
	fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, SEEK_SET);
	char *buf = new char[sz]; CHECK(fread(buf, 1, sz, in) == (size_t)sz);
	memcpy(buf, "BADMAGIC", 8);				/* clobber magic, keep size */
	CHECK(fwrite(buf, 1, sz, out_f) == (size_t)sz);
	delete [] buf; fclose(in); fclose(out_f);
}
ANT_pq_store *bad_magic = ANT_pq_store::load(badmagic_path, dim, 4, ANT_pq_codec::METRIC_DOT);
CHECK(bad_magic != NULL && bad_magic->document_count() == 0);
delete bad_magic; unlink(badmagic_path);

/* size-consistent invalid m: same file size, but the header's m field flipped to 6 (16 % 6 != 0).
   Rejection here comes from the m-divides-dimension field check, NOT the exact-size gate. */
char badm_path[64]; strcpy(badm_path, "/tmp/ant_pqim_XXXXXX"); { int fd = mkstemp(badm_path); if (fd >= 0) close(fd); }
{
	FILE *in = fopen(path, "rb"); FILE *out_f = fopen(badm_path, "wb");
	CHECK(in != NULL && out_f != NULL);
	fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, SEEK_SET);
	char *buf = new char[sz]; CHECK(fread(buf, 1, sz, in) == (size_t)sz);
	long long bad_m = 6; memcpy(buf + 28, &bad_m, 8);	/* m field at header offset 28; size unchanged */
	CHECK(fwrite(buf, 1, sz, out_f) == (size_t)sz);
	delete [] buf; fclose(in); fclose(out_f);
}
ANT_pq_store *bad_m = ANT_pq_store::load(badm_path, dim, 4, ANT_pq_codec::METRIC_DOT);
CHECK(bad_m != NULL && bad_m->document_count() == 0);
delete bad_m; unlink(badm_path);
```
(The `.pq` header field offsets — magic[0..8), version[8..12), dimension[12..20), documents[20..28), m[28..36), k[36..44) — follow from the sequential `fread`s in `ANT_pq_store::load`; confirm by reading that function before relying on offset 28.)

- [ ] **Step 4: Rebuild (header changed) and run all three touched suites**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_store && ./bin/test_pq_store
make test_pq_config && ./bin/test_pq_config
```
Expected: `forgiving_load OK` (with the two new cases) + `test_pq_store` PASSES; `test_pq_config` PASSES including `test_posture_quant_persist OK`.

- [ ] **Step 5: Regression + ASan/UBSan sweep**

Run the metrics test (Task 1) and a broad PQ sweep to confirm nothing regressed, then ASan:
```bash
make test_pq_metrics && ./bin/test_pq_metrics
for t in test_pq_search test_pq_recall test_pq_rerank test_pq_resident_tier; do make $t >/dev/null 2>&1 && ./bin/$t >/dev/null 2>&1 && echo "$t PASS" || echo "$t FAIL"; done
rm -f obj/*.o lib/libantelope_engine.a bin/test_pq_store bin/test_pq_config bin/test_pq_metrics
make CC='g++ -fsanitize=address,undefined -g' test_pq_store test_pq_config test_pq_metrics
for t in test_pq_store test_pq_config test_pq_metrics; do ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./bin/$t >/dev/null 2>&1 && echo "$t ASAN-clean" || echo "$t ASAN-FAIL"; done
rm -f obj/*.o lib/libantelope_engine.a
```
Expected: all PASS / ASAN-clean (the known out-of-scope `ANT_file::setvbuff` leak is why `detect_leaks=0`; no new UB in the touched paths).

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index.h tests/test_pq_store.cpp tests/test_pq_config.cpp
git commit -m "test(#21): pq_store bad-magic + size-consistent invalid-m; pq_config posture+quant round-trip (+getters)"
```

---

## Self-Review

**1. Spec coverage:** §1 M2 (cosine/L2 replace recall + rerank exact + mixed PQ/float-fallback, global-nearest planted in the fallback segment) → Task 1. §2 M3 (two getters; store bad-magic + size-consistent invalid-m; config posture+quant round-trip) → Task 2 Steps 1-3. §3 (build+pass on current master; ASan) → Task 1 Step 2 + Task 2 Step 5. ✓

**2. Placeholder scan:** no TBD/"handle edge cases". Every test is complete code. The only conditional guidance is Task 1 Step 2's "if a case fails, investigate" — which is a genuine TDD instruction (these tests lock existing behavior, so a failure is either a real bug to surface or an over-strict assertion), not a placeholder. ✓

**3. Type/signature consistency:** `pq_posture()`/`pq_rerank_quant()` defined in Task 2 Step 1 and used in Task 2 Step 2. `disk_segment_has_pq`, `search_vector`, `get_hit(i)->filename`, `set_pq_config(m, posture, quant)`, `VECTOR_METRIC_*`, `PQ_POSTURE_*`, `RERANK_QUANT_*` all match the shipped `atire_segment_index.h`. `.pq` m-field offset 28 stated consistently and cross-checked against the sequential-fread header layout. ✓

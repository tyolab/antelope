# Vector V5 — Late-Interaction (MaxSim) Reranking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an engine-native second-stage reranker — store ragged per-document multi-vectors (`.mvec`) and rescore a first-stage candidate set with ColBERT-style MaxSim late interaction.

**Architecture:** A new variable-length multi-vector store (`ANT_multivector_store`) reusing V4's per-dimension int8 quantization, a `maxsim()` kernel, and a two-stage `search_rerank` that reranks the existing first stage's top-N. Multi-vectors ride the same 7-site segment lifecycle as `.vec`/`.qvec`/`.vsig`/`.hnsw`.

**Tech Stack:** C++ (`source/`, `atire/`), Node-API addon (`nodejs/`), reusing `ANT_vector_quantize` (V4), `ATIRE_segment_index` lifecycle, existing `search_*` retrieval.

**Spec:** `docs/superpowers/specs/2026-07-07-vector-v5-late-interaction-design.md`.

---

## Conventions (read once)

- **Worktree:** all work in a dedicated worktree on branch `feature/vector-v5` (the executor sets this up via using-git-worktrees, mirroring V4's `.worktrees/vector-v4`). Build `make -C <wt>`, git `git -C <wt>`, never `cd`.
- **NO header dependency tracking:** after editing ANY `.h`, `rm -f <wt>/obj/*.o` before rebuilding, or stale objects link silently. Tasks that touch only `.cpp` can use a plain `make`.
- **Build discovery:** `make all` auto-discovers `source/*.cpp` and `atire/*.cpp` (GNUmakefile:316) — a new `source/multivector_store.cpp` needs no Makefile edit. `make tests` builds `tests/test_X.cpp` → `bin/test_X`; `tests/*.cpp` link all non-main `source/`+`atire/` objects.
- **Test-harness conventions:** each test file defines `CHECK(cond)`; read a neighbouring test before writing a new one to match the real API (constructors, `make_index_dir()`, `dir_has_glob()`, `get_hit()`). `ATIRE_segment_index` has NO `close()` method — use `delete index`. `flush()` is synchronous and returns 0. `search_*` return a `long long` hit count; read hits via `get_hit(i)->{generation,docid,score,filename}`.
- **Reused V4 API — `ANT_vector_quantize` (source/vector_quantize.h), all static:**
  - `compute_ranges(const float *vectors, long long dimension, long long n, float *mins, float *maxs)`
  - `quantize(const float *vector, long long dimension, const float *mins, const float *maxs, signed char *codes)`
  - `reconstruct(const signed char *codes, long long dimension, const float *mins, const float *maxs, float *out)`
- **Sidecar discipline (from V1–V4):** validate-before-allocate on load (compute exact file size from header, check before any `new[]`, degrade to empty on any mismatch); atomic writer (temp + rename); free every buffer on every error path and in the destructor; compaction refreshes the `output_segment` cache BEFORE the step-6 `segments[]` shuffle.
- **Milestone:** rerank is end-to-end functional after **Task 6** (flush + load + search_rerank). Task 7 completes compaction; Task 8 is Node; Task 9 hardens.

## File structure

- Create `source/multivector_store.h` / `source/multivector_store.cpp` — `ANT_multivector_store` (ragged load + `maxsim`) and `ANT_multivector_store_writer` (buffered, int8/float, atomic finish).
- Create `tests/test_multivector_store.cpp` — store round-trip, load validation, maxsim-vs-brute-force.
- Modify `atire/atire_segment_index.h` — `struct segment` gains `ANT_multivector_store *multivectors;`; rerank-config members + `set_rerank_config`/`rerank_configured` + `search_rerank` + `add_document`/`update_document` multi-vector overloads + writer-buffer members.
- Modify `atire/atire_segment_index.cpp` — ctor init, `open()` config load, flush `.mvec` write, `append_segment` load, destructor free, `delete_segment_files` unlink, NRT writer buffer.
- Modify `atire/atire_segment_index_vector.cpp` — `load/save/set_rerank_config`, `search_rerank`, `add_document_core` multi-vector threading, `writer_multivector_append`, live-buffer maxsim helper.
- Modify `atire/atire_segment_index_compaction.cpp` — variable-length `.mvec` merge + input-free.
- Modify `tests/test_segment_index.cpp` — integration tests.
- Modify `nodejs/addon/segment_index.cpp`, `nodejs/segment_index.d.ts`, `nodejs/README.md`, create `nodejs/test/rerank.test.js`.

**Known V5 limitation (document, don't fix):** multi-vectors are NOT written to the WAL. In durable mode, a doc added-but-not-yet-flushed is replayed from the WAL *without* its multi-vectors after a crash (they must be re-supplied). Flushed multi-vectors are durable (on disk in `.mvec`). This keeps V5 scoped; WAL persistence is a future follow-up.

---

## Task 1: `.mvec` multi-vector store — writer + validating load

**Files:**
- Create: `source/multivector_store.h`, `source/multivector_store.cpp`
- Test: `tests/test_multivector_store.cpp`

**Layout (`.mvec`):** header 36 bytes = `magic u64 "ANTMVEC1"` + `dimension i64` + `document_count i64` + `total_vectors i64` + `quant_flag i32`; then `counts[document_count]` i32; then `offsets[document_count+1]` i64 (prefix sums, `offsets[0]=0`, `offsets[n]=total_vectors`); then the pool `total_vectors × dimension` (int8 if `quant_flag==1` else float32); then, ONLY when int8, `qmin[dimension]` f32 + `qmax[dimension]` f32. Every appended vector is L2-normalized by the writer before quantization.

- [ ] **Step 1: Write the failing test** (`tests/test_multivector_store.cpp`)

```cpp
/*
	TEST_MULTIVECTOR_STORE.CPP
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "multivector_store.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* brute-force MaxSim reference over normalized float vectors (no store) */
static double ref_maxsim(const float *doc, long long m, const float *q, long long n, long long dim)
{
double total = 0.0;
for (long long i = 0; i < n; i++)
	{
	double best = -1e30;
	for (long long j = 0; j < m; j++)
		{
		double dot = 0.0;
		for (long long d = 0; d < dim; d++) dot += (double)q[i*dim+d] * (double)doc[j*dim+d];
		if (dot > best) best = dot;
		}
	total += best;
	}
return total;
}

static void l2norm(float *v, long long dim)
{
double s = 0.0; for (long long d = 0; d < dim; d++) s += (double)v[d]*v[d];
s = sqrt(s); if (s > 0) for (long long d = 0; d < dim; d++) v[d] = (float)(v[d]/s);
}

static void roundtrip_and_maxsim(int quantized)
{
long long dim = 16, ndocs = 5, i, d;
long long counts[5] = { 3, 0, 1, 4, 2 };		/* ragged incl. an absent doc */
long long total = 0; for (i = 0; i < ndocs; i++) total += counts[i];
float *all = new float[total * dim];			/* normalized doc vectors, laid out per doc */
srand(7);
for (i = 0; i < total * dim; i++) all[i] = (float)(rand()%2000-1000)/500.0f;
for (i = 0; i < total; i++) l2norm(all + i*dim, dim);

char path[64]; strcpy(path, "/tmp/ant_mvec_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_multivector_store_writer w;
CHECK(w.create(path, dim) == 0);
if (quantized) w.set_quantization(ANT_multivector_store_writer::QUANT_INT8);
long long off = 0;
for (i = 0; i < ndocs; i++)
	{ CHECK(w.append(counts[i] ? all + off*dim : NULL, counts[i]) == 0); off += counts[i]; }
CHECK(w.finish() == 0);

ANT_multivector_store *s = ANT_multivector_store::load(path, dim, ndocs);
CHECK(s != NULL);
CHECK(s->document_count() == ndocs);
CHECK(s->get_dimension() == dim);
CHECK(!s->has(1) && s->vector_count(1) == 0);	/* absent doc */
CHECK(s->has(0) && s->vector_count(0) == 3);
CHECK(s->has(3) && s->vector_count(3) == 4);

float q[3*16]; for (i = 0; i < 3*16; i++) q[i] = (float)(rand()%2000-1000)/500.0f;
for (i = 0; i < 3; i++) l2norm(q + i*dim, dim);
off = 0;
for (i = 0; i < ndocs; i++)
	{
	double got = s->maxsim(i, q, 3);
	double ref = counts[i] ? ref_maxsim(all + off*dim, counts[i], q, 3, dim) : 0.0;
	if (quantized) CHECK(fabs(got - ref) < 0.05 * 3);		/* int8: bounded error, 3 query terms */
	else CHECK(fabs(got - ref) < 1e-4);						/* float: exact */
	off += counts[i];
	}
CHECK(s->maxsim(1, q, 3) == 0.0);				/* absent doc scores 0 */
delete s; delete [] all; unlink(path);
printf("roundtrip_and_maxsim(quant=%d) OK\n", quantized);
}

static void load_validation(void)
{
/* corrupt magic -> empty; truncated -> empty; size-bomb header -> empty (no huge alloc) */
char p[64]; strcpy(p, "/tmp/ant_mvbad_XXXXXX"); { int fd=mkstemp(p); if(fd>=0) close(fd); }
FILE *f = fopen(p, "wb"); fputs("not a multivector store", f); fclose(f);
ANT_multivector_store *bad = ANT_multivector_store::load(p, 16, 5);
CHECK(bad != NULL && !bad->has(0) && bad->document_count() == 0);
delete bad;

/* size bomb: valid magic/dim but total_vectors = 2^40 with nothing behind it */
f = fopen(p, "wb");
unsigned long long magic = 0x31434556544E41ULL;			/* "ANTVEC1" placeholder; see NOTE */
long long dim = 16, docs = 4, total = 1LL << 40; int quant = 0;
/* NOTE: use the REAL magic constant your writer emits (read it back from the header you write). */
fwrite(&magic, 8, 1, f); fwrite(&dim, 8, 1, f); fwrite(&docs, 8, 1, f);
fwrite(&total, 8, 1, f); fwrite(&quant, 4, 1, f);
fclose(f);
ANT_multivector_store *bomb = ANT_multivector_store::load(p, 16, 4);
CHECK(bomb != NULL && !bomb->has(0));					/* size check rejects; degrades to empty */
delete bomb; unlink(p);
printf("load_validation OK\n");
}

int main(void)
{
roundtrip_and_maxsim(0);
roundtrip_and_maxsim(1);
load_validation();
printf("PASSED\n");
return 0;
}
```
(For the size-bomb magic: after Step 3 defines the real magic, set the bomb's `magic` to the exact constant the writer emits so the header passes the magic check and the *size* check is what rejects it — mirror `test_vector_store.cpp`'s bomb test.)

- [ ] **Step 2: Run to verify it fails**

Run: `make -C <wt> test_multivector_store`
Expected: FAIL to compile — `multivector_store.h` / the classes don't exist.

- [ ] **Step 3: Create `source/multivector_store.h`**

```cpp
/*
	MULTIVECTOR_STORE.H -- ragged per-document multi-vectors (V5 late interaction).
	Each document holds a variable count M_d >= 0 of `dimension`-D vectors.
	Vectors are L2-normalized on write; MaxSim over normalized vectors is a dot product.
	int8 pool reuses V4 ANT_vector_quantize (per-dimension ranges over the whole pool).
*/
#ifndef MULTIVECTOR_STORE_H_
#define MULTIVECTOR_STORE_H_

class ANT_multivector_store
{
private:
	long long dimension;
	long long documents;
	long long total_vectors;
	int quantized;					/* 1 = int8 pool, 0 = float pool */
	int *counts;					/* M_d per doc */
	long long *offsets;				/* documents+1 prefix sums */
	float *pool_f;					/* float pool (quantized==0) */
	signed char *pool_q;			/* int8 pool (quantized==1) */
	float *qmin;					/* dimension (quantized==1) */
	float *qmax;					/* dimension (quantized==1) */
	ANT_multivector_store();		/* built only by load() */

public:
	~ANT_multivector_store();
	static ANT_multivector_store *load(const char *filename, long long expected_dimension, long long expected_documents);
	long long get_dimension(void) { return dimension; }
	long long document_count(void) { return documents; }
	long has(long long docid) { return docid >= 0 && docid < documents && counts != NULL && counts[docid] > 0; }
	long long vector_count(long long docid) { return has(docid) ? counts[docid] : 0; }
	/* MaxSim of a normalized query matrix (num_query_vecs x dimension) vs this doc's vectors. 0 if absent. */
	double maxsim(long long docid, const float *query_vecs, long long num_query_vecs);
} ;

class ANT_multivector_store_writer
{
public:
	enum { QUANT_OFF = 0, QUANT_INT8 = 1 };

private:
	char *filename;
	long long dimension;
	int quant_mode;
	float *buffer;					/* growing normalized float pool */
	long long buffer_capacity;		/* in vectors */
	long long total_vectors;
	int *counts;					/* per doc */
	long long counts_capacity;		/* in docs */
	long long documents;

public:
	ANT_multivector_store_writer();
	~ANT_multivector_store_writer();
	long create(const char *path, long long dim);		/* 0 on success; resets state */
	void set_quantization(int mode) { quant_mode = mode; }
	long append(const float *vectors, long long num_vectors);	/* one doc's M x dim; NULL/0 = absent */
	long finish(void);								/* atomic temp+rename; 0 on success */
	void abandon(void);
} ;

#endif /* MULTIVECTOR_STORE_H_ */
```

- [ ] **Step 4: Create `source/multivector_store.cpp`** (mirror `source/vector_store.cpp`'s writer/loader discipline)

Implement:
- Define `static const unsigned long long ANT_MULTIVECTOR_STORE_MAGIC` from the 8 bytes of `"ANTMVEC1"` (mirror how `vector_store.cpp` builds `ANT_VECTOR_STORE_MAGIC`).
- **Writer `create`**: store `filename` (strdup), `dimension`, reset `quant_mode=QUANT_OFF`, allocate initial `buffer` (e.g. 1024 vectors) + `counts` (e.g. 256 docs), zero counters. A second `create` frees prior state first (idempotent reset — mirror the vector writer's "create twice" test contract).
- **Writer `append(vectors, num_vectors)`**: grow `counts` if `documents >= counts_capacity` (double). Set `counts[documents] = (int)num_vectors`. If `num_vectors > 0`: grow `buffer` to hold `total_vectors + num_vectors` (geometric); for each of the `num_vectors` rows, copy the row, L2-normalize it in place in the buffer, `total_vectors++`. `documents++`. Return 0 (1 on alloc failure).
- **Writer `finish`**: build `offsets[documents+1]` prefix sums from `counts`. Compute the pool: if `QUANT_INT8`, `ANT_vector_quantize::compute_ranges(buffer, dimension, total_vectors, mins, maxs)` then quantize every vector into a `signed char` codes array; else the pool is `buffer` (float). Write to `filename.tmp`: header (magic, dimension, documents, total_vectors, quant_flag) then `counts` (as int32) then `offsets` (int64) then pool (int8 or float) then, if int8, `mins`/`maxs`. `fclose`, `rename` to `filename`; on any write error `fclose`+`remove(tmp)`+return 1. Do NOT free the buffer here (allow `create`-reuse contract; the destructor frees).
- **Writer `abandon`/dtor**: free `buffer`, `counts`, `filename`.
- **`ANT_multivector_store::load`**: private-ctor object with all pointers NULL. `fopen("rb")`; read the 36-byte header; **validate BEFORE allocating**: magic == expected, `dimension == expected_dimension`, `documents == expected_documents`, `documents >= 0 && documents <= (1LL<<40)`, `total_vectors >= 0 && total_vectors <= (1LL<<42)`, `dimension >= 1 && dimension <= 65536`, `quant_flag` in {0,1}. Then compute the **exact** expected file size `36 + documents*4 + (documents+1)*8 + total_vectors*dimension*elem + (quant?dimension*4*2:0)` (elem = 1 int8 / 4 float) and compare to the real size via `fseek/ftell`; on ANY failure `fclose` and return the empty store (documents=0, all NULL — never crash, never over-allocate). Only then allocate `counts`/`offsets`/pool(+qmin/qmax) and read them; validate `offsets[0]==0 && offsets[documents]==total_vectors` and monotonic non-decreasing offsets with `offsets[k+1]-offsets[k]==counts[k]`; on mismatch free everything and degrade to empty. Free all buffers on every read-error path.
- **`maxsim(docid, q, n)`**: if `!has(docid)` return 0.0. For each query vector `i` in `[0,n)`, compute `best = max over j in [offsets[docid], offsets[docid+1])` of `dot(q_i, v_j)` where `v_j` is float (pool_f) or reconstructed from int8 (`ANT_vector_quantize::reconstruct(pool_q + j*dimension, dimension, qmin, qmax, tmp)` into a stack/scratch `float tmp[dimension]` — use a small stack buffer for `dimension<=512`, else `new`/`delete`, mirroring `vector_store.cpp::score`). Sum the per-query-vector maxima. Return the sum.
- **dtor**: free `counts`, `offsets`, `pool_f`, `pool_q`, `qmin`, `qmax` (NULL-safe).

- [ ] **Step 5: Run to verify it passes**

Run: `make -C <wt> all && make -C <wt> test_multivector_store && <wt>/bin/test_multivector_store`
Expected: `PASSED` (roundtrip float + int8, load validation).

- [ ] **Step 6: Commit**

```bash
git -C <wt> add source/multivector_store.h source/multivector_store.cpp tests/test_multivector_store.cpp
git -C <wt> commit -m "feat(v5): ragged .mvec multi-vector store + validating load + maxsim kernel"
```

---

## Task 2: MaxSim kernel edge cases (fold-in verification)

The `maxsim` kernel ships in Task 1 with its brute-force parity test. This task adds the empty/degenerate guards as explicit tests (no new production code unless a gap is found).

**Files:** Test: `tests/test_multivector_store.cpp`

- [ ] **Step 1: Add edge-case asserts** to `main()` (before `printf("PASSED")`)

```cpp
/* n=0 query -> score 0; single doc vector vs single query vector -> exact dot */
{
long long dim = 8;
char p[64]; strcpy(p, "/tmp/ant_mv1_XXXXXX"); { int fd=mkstemp(p); if(fd>=0) close(fd); }
float v[8] = {1,0,0,0,0,0,0,0};
ANT_multivector_store_writer w; CHECK(w.create(p, dim) == 0);
CHECK(w.append(v, 1) == 0); CHECK(w.finish() == 0);
ANT_multivector_store *s = ANT_multivector_store::load(p, dim, 1);
CHECK(s != NULL && s->has(0));
float q[8] = {1,0,0,0,0,0,0,0};
CHECK(fabs(s->maxsim(0, q, 1) - 1.0) < 1e-6);		/* normalized identical -> 1 */
CHECK(s->maxsim(0, q, 0) == 0.0);					/* no query vectors -> 0 */
delete s; unlink(p);
}
printf("maxsim_edges OK\n");
```

- [ ] **Step 2: Run** `make -C <wt> test_multivector_store && <wt>/bin/test_multivector_store` — Expected: `PASSED` with `maxsim_edges OK`. If either assert fails, fix the guard in `maxsim` (`num_query_vecs<=0 → return 0`; identical normalized vectors dot to 1).

- [ ] **Step 3: Commit**

```bash
git -C <wt> add tests/test_multivector_store.cpp source/multivector_store.cpp
git -C <wt> commit -m "test(v5): maxsim empty-query and identity edge cases"
```

---

## Task 3: `rerank.config` — persisted + immutable

**Files:**
- Modify: `atire/atire_segment_index.h`, `atire/atire_segment_index_vector.cpp`, `atire/atire_segment_index.cpp` (ctor + open)
- Test: `tests/test_segment_index.cpp`

Mirror the existing `load_hnsw_config`/`save_hnsw_config`/`set_hnsw_config` trio (binary magic+version, atomic temp+rename, defensive parse). Config file `<dir>/rerank.config`.

- [ ] **Step 1: Write the failing test** (add to `tests/test_segment_index.cpp`, register in `main()`)

```cpp
static void test_rerank_config_persist(void)
{
char *dir = make_index_dir();
{
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->rerank_configured() == 0);							/* default off */
	CHECK(ix->set_rerank_config(128, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
	CHECK(ix->rerank_configured() != 0);
	CHECK(ix->set_rerank_config(128, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);	/* same: idempotent */
	CHECK(ix->set_rerank_config(64, ATIRE_segment_index::RERANK_QUANT_INT8) != 0);	/* different dim: rejected */
	delete ix;
}
{
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->rerank_configured() != 0);							/* persisted across reopen */
	CHECK(ix->set_rerank_config(64, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);	/* different: rejected */
	delete ix;
}
delete [] dir;
printf("test_rerank_config_persist OK\n");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `rm -f <wt>/obj/*.o && make -C <wt> all >/dev/null 2>&1; make -C <wt> test_segment_index`
Expected: compile FAIL (`set_rerank_config`, `rerank_configured`, `RERANK_QUANT_*` undefined).

- [ ] **Step 3: Header** (`atire/atire_segment_index.h`)

Near the `QUANTIZE_*` enum add:
```cpp
	enum { RERANK_QUANT_FLOAT = 0, RERANK_QUANT_INT8 = 1 };
```
Add members near `quantization_current`:
```cpp
	long long rerank_dimension_current;		// 0 = rerank not configured
	long rerank_quant_current;				// RERANK_QUANT_FLOAT / RERANK_QUANT_INT8
```
Add public decls near the quantization-config decls:
```cpp
	long load_rerank_config(void);
	long save_rerank_config(void);
	long set_rerank_config(long long dimension, long quant_mode);	// immutable once set; 0 on success
	long rerank_configured(void) { return rerank_dimension_current != 0; }
	long long rerank_dimension(void) { return rerank_dimension_current; }
```

- [ ] **Step 4: Implement** in `atire/atire_segment_index_vector.cpp` (mirror the hnsw trio):
- `load_rerank_config`: read `<dir>/rerank.config` (magic `"ANTRR001"` as u64, version u32==1, `dimension` i64, `quant` i64). Absent → return 0 (stays off). Any read/magic/version failure, or `dimension < 1 || dimension > 65536`, or `quant` not in {0,1} → treat as absent (return 0, leave `rerank_dimension_current` unchanged). On success set `rerank_dimension_current = dimension; rerank_quant_current = (long)quant;`.
- `save_rerank_config`: atomic temp+rename write of magic/version/dimension/quant.
- `set_rerank_config(dimension, quant)`: `if (directory == NULL) return 1;` (must be open). Reject `dimension < 1 || dimension > 65536` or `quant` not in {FLOAT,INT8} → return 1. If `rerank_dimension_current != 0`: return `(rerank_dimension_current == dimension && rerank_quant_current == quant) ? 0 : 1` (idempotent same / reject different). Else set both members, `save_rerank_config()`; on save failure roll back to 0 and return 1.
- Constructor (`atire_segment_index.cpp`): init `rerank_dimension_current = 0; rerank_quant_current = 0;` next to `quantization_current = 0;`. In `open()`, after `load_quantization_config();`, add `load_rerank_config();`.

- [ ] **Step 5: Run to verify it passes**

Run: `rm -f <wt>/obj/*.o && make -C <wt> all && make -C <wt> test_segment_index && <wt>/bin/test_segment_index`
Expected: `PASSED` with `test_rerank_config_persist OK`.

- [ ] **Step 6: Commit**

```bash
git -C <wt> add atire/atire_segment_index.h atire/atire_segment_index_vector.cpp atire/atire_segment_index.cpp tests/test_segment_index.cpp
git -C <wt> commit -m "feat(v5): rerank.config (dimension + quant) persisted + immutable"
```

---

## Task 4: NRT writer multi-vector buffer + `add_document`/`update_document` overloads

**Files:**
- Modify: `atire/atire_segment_index.h`, `atire/atire_segment_index.cpp`, `atire/atire_segment_index_vector.cpp`
- Test: `tests/test_segment_index.cpp`

Add a per-writer buffer holding each pending doc's multi-vectors (parallel to `writer_vector_data`), plus public overloads that accept them. This task only *captures* multi-vectors; flush/search come later — so the test verifies capture via a new `writer_multivector_count_for_test(docid)` hook.

- [ ] **Step 1: Write the failing test** (add + register)

```cpp
static void test_writer_multivector_capture(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
float mv[3*8]; for (int i = 0; i < 3*8; i++) mv[i] = (float)(i+1)/10.0f;
long long h = ix->add_document("d0", "<DOC>hello</DOC>", /*docvec=*/NULL, mv, 3);
CHECK(h >= 0);
CHECK(ix->writer_multivector_count_for_test(0) == 3);
CHECK(ix->add_document("d1", "<DOC>world</DOC>", NULL, NULL, 0) >= 0);	/* no multivecs */
CHECK(ix->writer_multivector_count_for_test(1) == 0);
delete ix; delete [] dir;
printf("test_writer_multivector_capture OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — `rm -f <wt>/obj/*.o && make -C <wt> all >/dev/null 2>&1; make -C <wt> test_segment_index` → FAIL (overload + hook undefined).

- [ ] **Step 3: Header** — add writer-buffer members near `writer_vector_data`:
```cpp
	float *writer_multivector_data;			// growing pool of pending docs' multi-vectors
	long long writer_multivector_capacity;	// in vectors
	long long writer_multivector_total;		// vectors buffered so far
	int *writer_multivector_counts;			// M per writer docid
	long long writer_multivector_counts_capacity;	// in docs
```
Add public decls:
```cpp
	long long add_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors);
	long long update_document(const char *key, const char *document, const float *vector, const float *multivector, long long num_vectors);
	long writer_multivector_append(long long docid, const float *multivector, long long num_vectors);
	long long writer_multivector_count_for_test(long long docid);	// 0 if none; -1 if out of range
```

- [ ] **Step 4: Implement**
- `atire_segment_index.cpp` ctor: init the five new members to `NULL`/`0`. In the writer-reset path that frees `writer_vector_data` (find `reset_writer_vectors()` and the destructor), also free `writer_multivector_data` + `writer_multivector_counts` and zero the counters (add a `reset_writer_multivectors()` helper OR fold into the existing reset — mirror whatever `reset_writer_vectors` does).
- `atire_segment_index_vector.cpp`:
  - `writer_multivector_append(docid, mv, num)`: ensure `writer_multivector_counts` has room for `docid` (grow/zero-fill like `writer_vector_append` handles `writer_vector_presence`); set `writer_multivector_counts[docid] = (int)num`. If `num > 0 && mv != NULL`: grow `writer_multivector_data` to hold `writer_multivector_total + num` vectors (geometric); for each row, copy + L2-normalize into the buffer (reuse `ANT_vector_store::normalize` on a temp, or inline); `writer_multivector_total += num`. Return 0.
  - `writer_multivector_count_for_test(docid)`: bounds-check against the writer doc count; return `writer_multivector_counts` value or 0 / -1.
  - The new `add_document`/`update_document` 5-arg overloads: thread `multivector`/`num_vectors` into the shared core. Read how `add_document_core` currently records the single `vector` (it calls `writer_vector_append(docid, vector)`); add a sibling call `writer_multivector_append(docid, multivector, num_vectors)` on the same success path (docid known before `writer_documents++`, same as the vector append). Simplest: give `add_document_core` two extra params `(const float *multivector, long long num_vectors)` defaulting to `(NULL, 0)` so existing 2-/3-arg callers are unchanged, and the new overloads pass them through. Cosine-normalization of the doc-level `vector` stays as-is; multi-vectors are normalized inside `writer_multivector_append`.

- [ ] **Step 5: Run to verify it passes** — `rm -f <wt>/obj/*.o && make -C <wt> all && make -C <wt> test_segment_index && <wt>/bin/test_segment_index` → `PASSED` with `test_writer_multivector_capture OK`. All existing add/update tests still pass (defaulted params → byte-identical).

- [ ] **Step 6: Commit**

```bash
git -C <wt> add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git -C <wt> commit -m "feat(v5): NRT writer multi-vector buffer + add/update_document overloads"
```

---

## Task 5: Flush writes `.mvec` + segment-load + delete + dtor

**Files:**
- Modify: `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** (add + register). Uses the `dir_has_glob` helper already in the file.

```cpp
static void test_flush_writes_mvec(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
float mv[2*8]; for (int i = 0; i < 2*8; i++) mv[i] = (float)(i%5+1)/7.0f;
for (int i = 0; i < 20; i++)
	{ char k[16]; sprintf(k,"d%d",i); char d[48]; sprintf(d,"<DOC>doc %d</DOC>",i);
	  CHECK(ix->add_document(k, d, NULL, mv, 2) >= 0); }
CHECK(ix->flush() == 0);
CHECK(dir_has_glob(dir, "seg_*.mvec"));
delete ix;
/* reopen: the .mvec loads into the segment (proven indirectly here; search in Task 6) */
ATIRE_segment_index *re = new ATIRE_segment_index();
CHECK(re->open(dir) == 0);
CHECK(re->rerank_configured() != 0);
delete re; delete [] dir;
printf("test_flush_writes_mvec OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL at `dir_has_glob(dir, "seg_*.mvec")` (flush doesn't write it yet).

- [ ] **Step 3: Implement** in `atire/atire_segment_index.cpp`:
- `struct segment` (header, Task-3 file — but this is a `.cpp`-only task; the field was NOT added yet, so ADD it in the header here): add `ANT_multivector_store *multivectors;` to `struct segment` in `atire/atire_segment_index.h` (this makes Task 5 touch the header → `rm obj/*.o`).
- **flush**: right after the existing vector-sidecar block (the `if (vector_dimension_current != 0 && writer_vectors_present > 0)` region) add a rerank block guarded by `if (rerank_configured() && writer_multivector_total > 0)`: build `seg_G.mvec` via `ANT_multivector_store_writer` — `create(mvec_filename, rerank_dimension_current)`, `set_quantization(rerank_quant_current == RERANK_QUANT_INT8 ? ANT_multivector_store_writer::QUANT_INT8 : QUANT_OFF)`, then for `docid` in `[0, flushed_document_count)` append `writer_multivector_data + (offset for docid)` with `writer_multivector_counts[docid]` vectors (walk a running offset = sum of prior counts; append `NULL,0` when a doc has 0). `finish()`. Best-effort: a failure leaves the segment rerank-less (documented), non-fatal to flush. Name it with `segment_filename(mvec_filename, sizeof(...), flushed_vector_generation, "mvec")`.
- **append_segment (load)**: after the `.vectors`/`exact_vectors` load, add `segments[segment_count].multivectors = rerank_configured() ? ANT_multivector_store::load(<seg_G.mvec>, rerank_dimension_current, engine->get_document_count()) : NULL;` (build the filename with `segment_filename(..., "mvec")`). Set it to `NULL` in the `else` branch too.
- **destructor**: add `delete segments[which].multivectors;` in the teardown loop next to `delete segments[which].vectors;`.
- **delete_segment_files**: add `segment_filename(filename, ..., "mvec"); remove(filename);`.

- [ ] **Step 4: Run to verify it passes** — `rm -f <wt>/obj/*.o && make -C <wt> all && make -C <wt> test_segment_index && <wt>/bin/test_segment_index` → `PASSED` with `test_flush_writes_mvec OK`.

- [ ] **Step 5: Commit**

```bash
git -C <wt> add atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_segment_index.cpp
git -C <wt> commit -m "feat(v5): flush writes .mvec; segment-load/dtor/delete dispatch"
```

---

## Task 6: `search_rerank` two-stage flow (MILESTONE — rerank end-to-end)

**Files:**
- Modify: `atire/atire_segment_index.h`, `atire/atire_segment_index_vector.cpp`
- Test: `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** — a crafted case proving rerank *changes* the order: doc A wins the single-vector first stage but doc B wins MaxSim; assert B is top-1 after rerank.

```cpp
static void test_search_rerank_changes_order(void)
{
char *dir = make_index_dir();
long long dim = 4, mvdim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(mvdim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* float: exact */

/* stage-1 doc-vector: A closer to the query than B */
float qvec[4]  = {1.0f, 0.0f, 0.0f, 0.0f};
float Avec[4]  = {0.9f, 0.1f, 0.0f, 0.0f};		/* higher dot with qvec */
float Bvec[4]  = {0.5f, 0.5f, 0.0f, 0.0f};		/* lower dot with qvec */
/* multi-vectors: B has a token that matches the query token exactly; A does not */
float qmv[4]   = {0.0f, 0.0f, 1.0f, 0.0f};		/* query token points at dim 2 */
float Amv[2*4] = {1,0,0,0, 0,1,0,0};			/* no dim-2 token */
float Bmv[2*4] = {1,0,0,0, 0,0,1,0};			/* second token matches qmv exactly */
CHECK(ix->add_document("A", "<DOC>alpha</DOC>", Avec, Amv, 2) >= 0);
CHECK(ix->add_document("B", "<DOC>beta</DOC>",  Bvec, Bmv, 2) >= 0);

/* LIVE-BUFFER (pre-flush / NRT) rerank: exercises maxsim_live -> B first */
CHECK(ix->search_rerank(NULL, qvec, qmv, 1, 10, 2) == 2);
CHECK(strcmp(ix->get_hit(0)->filename, "B") == 0);

CHECK(ix->flush() == 0);

/* stage-1 alone (now from disk): A ranks first */
CHECK(ix->search_vector(qvec, 2) == 2);
CHECK(strcmp(ix->get_hit(0)->filename, "A") == 0);

/* rerank over the disk segment: B's exact-match token wins MaxSim -> B first */
long long n = ix->search_rerank(/*text=*/NULL, qvec, qmv, /*num_query_vecs=*/1, /*first_stage_n=*/10, /*top_k=*/2);
CHECK(n == 2);
CHECK(strcmp(ix->get_hit(0)->filename, "B") == 0);
CHECK(ix->get_hit(0)->score >= ix->get_hit(1)->score);
delete ix; delete [] dir;
printf("test_search_rerank_changes_order OK\n");
}
```
(Confirm the result key is exposed as `get_hit(i)->filename` by reading a neighbouring vector-search test; adjust the accessor if the field name differs.)

- [ ] **Step 2: Run to verify it fails** — compile FAIL (`search_rerank` undefined).

- [ ] **Step 3: Header** — add:
```cpp
	long long search_rerank(char *query_text, const float *query_vector, const float *query_multivector, long long num_query_vecs, long long first_stage_n, long long top_k);
```

- [ ] **Step 4: Implement** `search_rerank` in `atire/atire_segment_index_vector.cpp`:
1. Guard: `if (num_query_vecs < 1 || query_multivector == NULL || top_k < 1 || !rerank_configured()) ` → fall back to a plain first-stage search (`search_vector`/`search_hybrid`/`search`) so the call still returns sensible hits; or return 0 if nothing to search. Clamp `if (top_k > first_stage_n) top_k = first_stage_n;`.
2. **Normalize** the query multi-vectors into a local `float *qn = new float[num_query_vecs * rerank_dimension_current]` (copy then `ANT_vector_store::normalize` each row). The doc-level `query_vector` is passed to stage 1 unchanged (stage 1 handles its own cosine-normalize).
3. **Stage 1**: choose by args and run to `first_stage_n` — `if (query_text != NULL && query_vector != NULL) search_hybrid(query_text, query_vector, first_stage_n); else if (query_vector != NULL) search_vector(query_vector, first_stage_n); else search(query_text, first_stage_n);`. These populate the shared `results[]` / `results_count`.
4. **Rescore**: for each result `i` in `[0, results_count)`, resolve its store from `results[i].generation`: scan `segments[]` for a segment whose `generation == results[i].generation` and use `segment.multivectors`; if the hit is from the live buffer (generation matches the current writer generation — check how `search_vector_impl` tags live hits) use a new `maxsim_live(docid, qn, num_query_vecs)` helper over `writer_multivector_*`. Compute `ms` when the store `has(docid)`; record `(has_mv, ms)` per result.
5. **Reorder**: stable-partition `results[]` into `has_mv` (sorted by `ms` desc, ties by generation/docid asc — reuse the `vector_candidate_compare` tie rule) FOLLOWED BY the `!has_mv` results in their existing stage-1 order. Set the reranked hits' `.score = ms`; leave non-mv hits' stage-1 score. Truncate `results_count` to `top_k` (free the filenames of dropped hits via the same path `reset_results`/publish uses — mirror how `search_vector_impl` truncates).
6. `delete [] qn;` return `results_count`.
- Add the `maxsim_live` helper: mirror `scan_live_buffer_exact`'s access to `writer_multivector_data` using `writer_multivector_counts` + a running offset to find `docid`'s slice, reconstruct not needed (live buffer is float + normalized already), compute MaxSim directly.

- [ ] **Step 5: Run to verify it passes** — `rm -f <wt>/obj/*.o && make -C <wt> all && make -C <wt> test_segment_index && <wt>/bin/test_segment_index` → `PASSED` with `test_search_rerank_changes_order OK`.

- [ ] **Step 6: Commit**

```bash
git -C <wt> add atire/atire_segment_index.h atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git -C <wt> commit -m "feat(v5): search_rerank two-stage MaxSim flow (rerank end-to-end)"
```

---

## Task 7: Compaction rewrites `.mvec` (variable-length renumber) + input-free

**Files:**
- Modify: `atire/atire_segment_index_compaction.cpp`
- Test: `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** — build rerank over 2 flushed segments, compact, assert `.mvec` survives and rerank still returns the crafted winner.

```cpp
static void test_compaction_preserves_mvec(void)
{
char *dir = make_index_dir();
long long dim = 4, mvdim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(mvdim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
float qvec[4] = {1,0,0,0}, qmv[4] = {0,0,1,0};
float Avec[4] = {0.9f,0.1f,0,0}, Amv[2*4] = {1,0,0,0, 0,1,0,0};
float Bvec[4] = {0.5f,0.5f,0,0}, Bmv[2*4] = {1,0,0,0, 0,0,1,0};
CHECK(ix->add_document("A", "<DOC>a</DOC>", Avec, Amv, 2) >= 0);
CHECK(ix->flush() == 0);							/* generation 1 */
CHECK(ix->add_document("B", "<DOC>b</DOC>", Bvec, Bmv, 2) >= 0);
CHECK(ix->flush() == 0);							/* generation 2 */
long long gens[2] = {1, 2};
CHECK(ix->compact(gens, 2) == 0);
CHECK(dir_has_glob(dir, "seg_*.mvec"));
long long n = ix->search_rerank(NULL, qvec, qmv, 1, 10, 2);
CHECK(n == 2);
CHECK(strcmp(ix->get_hit(0)->filename, "B") == 0);	/* rerank order survives compaction */
delete ix; delete [] dir;
printf("test_compaction_preserves_mvec OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — after compaction `seg_*.mvec` is absent (compaction doesn't merge it), so `dir_has_glob` fails.

- [ ] **Step 3: Implement** in `atire/atire_segment_index_compaction.cpp`, inside `compact()`: mirror the existing `.vec` merge block (the one guarded by `if (vector_dimension_current != 0)` that uses `ANT_docid_renumberer`). Add a sibling block guarded by `if (rerank_configured())`:
- Determine if any input has multi-vectors (`inputs[i]->multivectors != NULL && ->document_count() > 0`).
- Build a fresh `ANT_docid_renumberer` (or reuse the one already built for `.vec` if in the same scope — but keep the block self-contained to avoid coupling) over the same `stone_list`/`doc_counts`.
- `ANT_multivector_store_writer mvw; mvw.create(<output_generation>.mvec, rerank_dimension_current); mvw.set_quantization(rerank_quant_current == RERANK_QUANT_INT8 ? QUANT_INT8 : QUANT_OFF);`
- Iterate output docs in the SAME order the `.vec` merge uses: for each `(input, docid)` whose `renumber(input, docid) >= 0` (not tombstoned), read that input doc's multi-vectors from `inputs[input]->multivectors`. Because the store returns `maxsim` not raw vectors, add a store accessor to fetch a doc's reconstructed vectors: `long copy_vectors(long long docid, float *out)` returning `M_d` and filling `out[M_d*dim]` (reconstruct int8 → float; for float pool, straight copy). Append those `M_d` vectors (or `NULL,0` when the doc has none) to `mvw`. Allocate a reusable `float *mvbuf = new float[MAX_M * dim]` sized to the largest `M_d` across inputs (compute it first) — free after.
- `mvw.finish()`. Best-effort (non-fatal), consistent with the `.vsig`/`.hnsw` rebuild blocks.
- **Refresh the output_segment cache BEFORE the step-6 shuffle**: after `append_segment(output_generation)` and alongside the `.vsig`/`.hnsw` refresh, reload `output_segment->multivectors` from the merged `.mvec` (so THIS session's `search_rerank` sees it).
- **Input-free**: in the step-6 input-drop loop, add `delete segments[which].multivectors;` next to the other frees.
- Add the store accessor to `ANT_multivector_store` (header + cpp): `long long copy_vectors(long long docid, float *out);` (returns `M_d`, fills reconstructed/copied normalized vectors; `copy_vectors` on an absent doc returns 0). Add a `long long max_vector_count(void)` or compute the max in the caller by iterating `vector_count`.

- [ ] **Step 4: Run to verify it passes** — `rm -f <wt>/obj/*.o && make -C <wt> all && make -C <wt> test_segment_index && <wt>/bin/test_segment_index` → `PASSED` with `test_compaction_preserves_mvec OK`. All pre-existing compaction tests (`test_compact_basic`, etc.) still pass (rerank off → block skipped).

- [ ] **Step 5: Commit**

```bash
git -C <wt> add source/multivector_store.h source/multivector_store.cpp atire/atire_segment_index_compaction.cpp tests/test_segment_index.cpp
git -C <wt> commit -m "feat(v5): compaction merges .mvec (variable-length renumber) + input-free"
```

---

## Task 8: Node binding — `rerank` option + `addDocument` multi-vectors + `searchRerank`

**Files:**
- Modify: `nodejs/addon/segment_index.cpp`, `nodejs/segment_index.d.ts`, `nodejs/README.md`
- Create: `nodejs/test/rerank.test.js`

Mirror how the `quantize`/`hnsw` options + `addDocument` vector arg + `searchVector` are wired (constructor option parsed into a member, applied in `open()` non-fatally; vectors extracted via the existing `extract_vector` helper; searches return `[{key,score,generation,docid}]`).

- [ ] **Step 1: Write the failing test** (`nodejs/test/rerank.test.js`)

```js
const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

test('rerank: searchRerank reorders vs searchVector', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_rr_'));
  const idx = new SegmentIndex({ dimension: 4, metric: 'dot', rerank: { dimension: 4, quantize: 'float' } });
  idx.open(dir);
  const qvec = new Float32Array([1,0,0,0]);
  idx.addDocument('A', '<DOC>a</DOC>', new Float32Array([0.9,0.1,0,0]),
                  [new Float32Array([1,0,0,0]), new Float32Array([0,1,0,0])]);
  idx.addDocument('B', '<DOC>b</DOC>', new Float32Array([0.5,0.5,0,0]),
                  [new Float32Array([1,0,0,0]), new Float32Array([0,0,1,0])]);
  await idx.flush();
  const stage1 = idx.searchVector(qvec, 2);
  assert.strictEqual(stage1[0].key, 'A');
  const qmv = [new Float32Array([0,0,1,0])];
  const reranked = idx.searchRerank(qmv, { vector: qvec, firstStageN: 10, topK: 2 });
  assert.strictEqual(reranked[0].key, 'B');		// MaxSim lifts B
  idx.close();
});
```

- [ ] **Step 2: Run to verify it fails** — `rm -f <wt>/obj/*.o && (cd <wt>/nodejs && npm run build:segment) && node <wt>/nodejs/test/rerank.test.js` → FAIL (`rerank` option ignored / `searchRerank` not a function).

- [ ] **Step 3: Implement** (`nodejs/addon/segment_index.cpp`)
- Parse constructor option `rerank` (object `{ dimension: number, quantize?: 'int8'|'float' }`): store `option_rerank_dim` (0 = off) + `option_rerank_quant` (default int8). In `Open()`, after the quantize application and before `state = OPEN;`, `if (option_rerank_dim > 0) engine->set_rerank_config(option_rerank_dim, option_rerank_quant);` (non-fatal).
- `AddDocument`/`UpdateDocument`: accept an optional 4th arg `multiVectors` = `Array<Float32Array>`. When present, validate each row length == `option_rerank_dim`, flatten into a `float *` scratch of `M*dim`, and call the 5-arg `add_document`/`update_document` overload with `(flat, M)`; free the scratch. When absent, call with `(NULL, 0)`.
- `SearchRerank(queryMultiVectors, options)`: `queryMultiVectors` = `Array<Float32Array>` (each length == rerank dim); `options` = `{ text?, vector?, firstStageN=100, topK=10 }`. Flatten the query multi-vectors; extract the optional `vector` (via `extract_vector`) and `text`; call `engine->search_rerank(text, vector, flatMV, N, firstStageN, topK)`; publish results the same way `SearchVector` does (`[{key,score,generation,docid}]`).
- Register `InstanceMethod("searchRerank", &SegmentIndexWrap::SearchRerank)`.

- [ ] **Step 3b: Types + docs**
- `nodejs/segment_index.d.ts`: add `rerank?: { dimension: number; quantize?: 'int8' | 'float' }` to the options interface; overload `addDocument`/`updateDocument` to accept an optional `multiVectors?: Float32Array[]`; add `searchRerank(queryMultiVectors: Float32Array[], options: { text?: string; vector?: Float32Array | number[]; firstStageN?: number; topK?: number }): Array<{ key: string; score: number; generation: number; docid: number }>;`.
- `nodejs/README.md`: add a "Late-interaction reranking (V5)" section modeled on the V2/V3/V4 sections — what MaxSim does, the `rerank: { dimension }` option, `addDocument(..., multiVectors)` as `Float32Array[]` (ragged), `searchRerank`, and the supply-at-index-time / no-backfill note.

- [ ] **Step 4: Run to verify it passes** — `rm -f <wt>/obj/*.o && (cd <wt>/nodejs && npm run build:segment) && node <wt>/nodejs/test/rerank.test.js` → passes; also `node <wt>/nodejs/test/hnsw.test.js` still passes.

- [ ] **Step 5: Commit**

```bash
git -C <wt> add nodejs/addon/segment_index.cpp nodejs/segment_index.d.ts nodejs/README.md nodejs/test/rerank.test.js
git -C <wt> commit -m "feat(v5): Node rerank option + addDocument multi-vectors + searchRerank"
```

---

## Task 9: Coexistence + float/int8 parity + ASan lifecycle sweep

**Files:** Test: `tests/test_segment_index.cpp`

- [ ] **Step 1: Coexistence + parity test** (add + register)

```cpp
static void test_rerank_coexists_and_parity(void)
{
/* rerank + V4 quantize(replace) + approximate + hnsw on ONE index, across add/flush/delete/maintain */
char *dir = make_index_dir();
long long dim = 16, mvdim = 8, n = 60, i, d;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_approximate_config(256) == 0);
CHECK(ix->set_hnsw_config(16, 200) == 0);
CHECK(ix->set_quantization(ATIRE_segment_index::QUANTIZE_REPLACE) == 0);
CHECK(ix->set_rerank_config(mvdim, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
srand(71);
float dv[16], mv[3*8];
for (i = 0; i < n; i++)
	{
	for (d = 0; d < dim; d++) dv[d] = (float)(rand()%2000-1000)/500.0f;
	if (dv[0] == 0.0f) dv[0] = 0.3f;					/* avoid zero doc-vector under cosine */
	for (d = 0; d < 3*8; d++) mv[d] = (float)(rand()%2000-1000)/500.0f;
	char k[16]; sprintf(k,"d%lld",i); char doc[48]; sprintf(doc,"<DOC>doc %lld tok</DOC>",i);
	CHECK(ix->add_document(k, doc, dv, mv, 3) >= 0);
	if (i % 20 == 19) CHECK(ix->flush() == 0);
	}
CHECK(ix->flush() == 0);
CHECK(ix->delete_document("d5") == 0);
CHECK(ix->maintain() == 0);
CHECK(dir_has_glob(dir, "seg_*.mvec"));
float qv[16]; for (d = 0; d < dim; d++) qv[d] = (float)(rand()%2000-1000)/500.0f;
float qmv[3*8]; for (d = 0; d < 3*8; d++) qmv[d] = (float)(rand()%2000-1000)/500.0f;
char qtext[] = "tok";
CHECK(ix->search_vector(qv, 5) >= 1);
CHECK(ix->search_rerank(qtext, qv, qmv, 3, 50, 5) >= 1);		/* rerank over hybrid stage 1, post-compaction */
delete ix; delete [] dir;
printf("test_rerank_coexists_and_parity OK\n");
}
```

- [ ] **Step 2: Run** — `rm -f <wt>/obj/*.o && make -C <wt> all && make -C <wt> test_segment_index && <wt>/bin/test_segment_index` → `PASSED` with `test_rerank_coexists_and_parity OK` and all pre-existing tests green (rerank/quantize off paths byte-identical).

- [ ] **Step 3: ASan lifecycle sweep** (best-effort; the normal-build test in Step 2 is the hard gate):
```bash
rm -f <wt>/obj/*.o
make -C <wt> all CXXFLAGS="-fsanitize=address -g -fPIC" CFLAGS="-fsanitize=address -g -fPIC" LDFLAGS="-fsanitize=address" 2>&1 | tail -5
make -C <wt> test_segment_index CXXFLAGS="-fsanitize=address -g -fPIC" CFLAGS="-fsanitize=address -g -fPIC" LDFLAGS="-fsanitize=address" 2>&1 | tail -5
ASAN_OPTIONS=detect_leaks=0 <wt>/bin/test_segment_index 2>&1 | tail -20
```
Expected: `PASSED`, no ASan `ERROR` on the `.mvec` add→flush→delete→maintain lifecycle. Note: a `detect_leaks=1` run may flag the pre-existing engine-wide `ANT_file::setvbuff` leak — that is NOT a V5 defect; only a leak/overflow rooted in `multivector_store`/`.mvec`/`maxsim`/`writer_multivector_*`/compaction-mvec counts as a V5 finding. Restore a clean normal build afterward: `rm -f <wt>/obj/*.o && make -C <wt> all`.

- [ ] **Step 4: Commit**

```bash
git -C <wt> add tests/test_segment_index.cpp
git -C <wt> commit -m "test(v5): rerank coexistence (+quantize/approx/hnsw) + ASan lifecycle sweep"
```

---

## Final review

After all tasks: dispatch a holistic code review over the whole V5 diff (`git diff master...HEAD`) focusing on: `.mvec` load validation (validate-before-allocate, integer-overflow safety on `total_vectors*dimension`, all buffers freed on error), MaxSim correctness (parity with the brute-force reference, normalized-dot semantics), the seven `.mvec` lifecycle sites for leaks/double-free (esp. compaction variable-length renumber and the `output_segment` refresh ordering), rerank ordering/graceful-degradation correctness, the WAL limitation being honestly documented, and that rerank-off / all pre-V5 paths are byte-identical. Fix any Critical/Important finding (with a regression test) before finishing. Then use superpowers:finishing-a-development-branch.

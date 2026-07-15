# Token `.mvpq` Global Codebook Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Train ONE frozen collection-wide token codebook for the whole `.mvpq` collection (instead of per-segment), so token codes are comparable across segments and compaction needn't retrain. Approach B: each `.mvpq` still embeds a copy of the shared frozen codebook.

**Architecture:** The engine owns `global_mvpq_codebook` (+`global_mvpq_rotation` under T1 OPQ), trained ONCE from the first build's flattened token pool, persisted to `<dir>/multivector_pq.codebook` and loaded on open; every `.mvpq` writer is handed the frozen codebook+R via a new `set_external_codebook` seam and skips its own `train`/`train_rotation`, embedding the shared copy — so `.mvpq` format/load/codec/query are UNCHANGED. This is a direct mirror of the shipped dense global codebook (#22.2); crib the dense engine functions named below.

**Tech Stack:** C++ (C++03-style) ATIRE/antelope engine. Tests: standalone `tests/*.cpp` via `make <name>` → `bin/<name>`, `CHECK()` convention.

**Reference (dense #22.2 — mirror these exactly, adapting dense→token):**
- `source/pq_store.cpp` `ANT_pq_store_writer::finish` (owned-vs-borrowed refactor) + `set_external_codebook`.
- `atire/atire_segment_index_vector.cpp`: `save_pq_codebook` (~760), `load_pq_codebook` (~799), `ensure_global_pq_codebook` (~838), `rebuild_pq_global_codebook` (~932), `set_pq_global_codebook`, and the `pq.config` v4 `global` field in `load/save_pq_config`.

**Repo gotchas:** after ANY header edit (`source/multivector_pq_store.h`, `atire/atire_segment_index.h`): `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding (no header dep tracking → stale-object SEGV). Build a test: `make <name>` → `bin/<name>`. Config setters POST-open. Confirm line numbers by grep. `D = rerank_dimension_current`; token k is always 256 (variable-k is the later T2). Do NOT modify `source/pq_codec.{h,cpp}`.

---

### Task 1: Writer external-codebook seam (`ANT_multivector_pq_store_writer`)

Let the token writer run `finish()` against a caller-supplied codebook+R instead of training its own. Self-contained in `multivector_pq_store.{h,cpp}` + a unit test.

**Files:**
- Modify: `source/multivector_pq_store.h` (writer `ext_codebook`/`ext_rotation` members, `set_external_codebook` decl)
- Modify: `source/multivector_pq_store.cpp` (ctor init, `create` reset, `set_external_codebook`, `finish` refactor)
- Test: `tests/test_mvpq_external_codebook.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_mvpq_external_codebook.cpp`:

```cpp
/*
	TEST_MVPQ_EXTERNAL_CODEBOOK.CPP -- token global-codebook groundwork
	(token epic 2/4): a writer handed an external codebook (+optional R)
	skips training, encodes/embeds against it, and loads back identically;
	the non-external path stays byte-identical (deterministic).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void fill(float *v, long long n, unsigned seed)
{ srand(seed); for (long long i = 0; i < n; i++) v[i] = (float)(((rand()%2000)-1000)/500.0); }

/* train a standalone codebook over a flat token pool (no OPQ) to use as "external" */
static float *train_ext(const float *pool, long long D, long long m, long long ntok)
{
	float *cb = new float[m * (long long)ANT_pq_codec::K * (D/m)];
	CHECK(ANT_pq_codec::train(pool, D, m, ANT_pq_codec::K, ntok, cb) == 0);
	return cb;
}

static void test_external_encodes_and_embeds(void)
{
	const long long D = 8, m = 4;
	int counts[3] = { 10, 6, 8 }; long long ntok = 24;
	float *pool = new float[ntok * D]; fill(pool, ntok*D, 5);
	float *ext = train_ext(pool, D, m, ntok);

	char path[] = "/tmp/mvpq_ext_XXXXXX"; CHECK(mkstemp(path) >= 0);
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, D, m, ANT_pq_codec::METRIC_DOT, /*opq*/0) == 0);
	w.set_external_codebook(ext, NULL);					// supply codebook, no OPQ rotation
	long long off = 0;
	for (int d = 0; d < 3; d++) { CHECK(w.append(pool + off*D, counts[d]) == 0); off += counts[d]; }
	CHECK(w.finish() == 0);

	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, D, 3, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == ntok);
	// embedded codebook must equal the supplied one (finish did not retrain)
	long long cb_floats = m * (long long)ANT_pq_codec::K * (D/m);
	CHECK(memcmp(s->get_codebook(), ext, (size_t)cb_floats * sizeof(float)) == 0);
	// each token's code must equal a direct encode against ext
	for (long long t = 0; t < ntok; t++)
		{
		unsigned char expect[4];
		ANT_pq_codec::encode(pool + t*D, D, m, ANT_pq_codec::K, ext, expect);
		CHECK(memcmp(s->token_codes(t), expect, (size_t)m) == 0);
		}
	delete s; remove(path); delete [] ext; delete [] pool;
	printf("test_external_encodes_and_embeds OK\n");
}

static void test_non_external_unchanged(void)
{
	// no external codebook -> trains its own -> two runs byte-identical (deterministic).
	const long long D = 6, m = 3;
	int counts[2] = { 8, 8 }; long long ntok = 16;
	float *pool = new float[ntok * D]; fill(pool, ntok*D, 9);
	char a[] = "/tmp/mvpq_na_XXXXXX", b[] = "/tmp/mvpq_nb_XXXXXX";
	CHECK(mkstemp(a) >= 0); CHECK(mkstemp(b) >= 0);
	for (int pass = 0; pass < 2; pass++)
		{
		ANT_multivector_pq_store_writer w;
		CHECK(w.create(pass ? b : a, D, m, ANT_pq_codec::METRIC_DOT, 0) == 0);
		long long off = 0;
		for (int d = 0; d < 2; d++) { CHECK(w.append(pool + off*D, counts[d]) == 0); off += counts[d]; }
		CHECK(w.finish() == 0);
		}
	FILE *fa = fopen(a,"rb"), *fb = fopen(b,"rb"); CHECK(fa && fb);
	fseek(fa,0,SEEK_END); long la = ftell(fa); fseek(fb,0,SEEK_END); long lb = ftell(fb);
	CHECK(la == lb && la > 0); rewind(fa); rewind(fb);
	unsigned char *ba = new unsigned char[la], *bb = new unsigned char[lb];
	CHECK(fread(ba,1,la,fa)==(size_t)la && fread(bb,1,lb,fb)==(size_t)lb);
	CHECK(memcmp(ba, bb, la) == 0);
	fclose(fa); fclose(fb); delete[] ba; delete[] bb; remove(a); remove(b); delete [] pool;
	printf("test_non_external_unchanged OK\n");
}

int main(void)
{
	test_external_encodes_and_embeds();
	test_non_external_unchanged();
	printf("ALL test_mvpq_external_codebook PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
make test_mvpq_external_codebook 2>&1 | tail -15
```
Expected: FAIL to compile — `w.set_external_codebook(...)` undeclared.

- [ ] **Step 3: Header — writer external members + setter**

In `source/multivector_pq_store.h`, in the writer's private section add (after `long opq;`):
```cpp
	const float *ext_codebook;		// borrowed external codebook (global mode); NULL = train own
	const float *ext_rotation;		// borrowed external R (global+OPQ); NULL = non-OPQ or train own
```
In the public section add:
```cpp
	void set_external_codebook(const float *codebook, const float *rotation);	// finish() encodes/embeds these instead of training (borrowed; never freed here)
```

- [ ] **Step 4: `.cpp` — ctor init, `create` reset, `set_external_codebook`**

In `source/multivector_pq_store.cpp`:

Ctor init list — add `ext_codebook(0), ext_rotation(0)`:
```cpp
ANT_multivector_pq_store_writer::ANT_multivector_pq_store_writer() :
	filename(0), dimension(0), m(0), metric(0), opq(0), ext_codebook(0), ext_rotation(0),
	buffer(0), capacity(0), total_tokens(0), counts(0), counts_capacity(0), documents(0) {}
```
(Match the actual field order in the header — put the ext_* inits where they sit in declaration order to avoid `-Wreorder`.)

`create` — reset externals (a reused writer must not carry stale ones). At the top of `create`, right after `abandon();`:
```cpp
ext_codebook = NULL;
ext_rotation = NULL;
```

Add the setter (near `create`):
```cpp
void ANT_multivector_pq_store_writer::set_external_codebook(const float *codebook, const float *rotation)
{
ext_codebook = codebook;
ext_rotation = rotation;
}
```

- [ ] **Step 5: `finish()` — owned-vs-borrowed refactor (mirror dense `ANT_pq_store_writer::finish`)**

Replace the current `finish()` body's rotation+codebook+encode section so it uses external buffers when supplied. The rewritten `finish()`:

```cpp
long ANT_multivector_pq_store_writer::finish(void)
{
if (filename == NULL) return 1;
long long cb_floats = 256*dimension;

/* --- rotation: external (borrowed) OR trained (owned) OR none --- */
float *owned_rotation = NULL;
const float *rotation = NULL;
if (ext_codebook != NULL)
	rotation = ext_rotation;						/* borrowed (may be NULL for non-OPQ global) */
else if (opq && total_tokens > 0)
	{
	owned_rotation = new float[dimension * dimension];
	if (ANT_pq_codec::train_rotation(buffer, dimension, m, total_tokens, owned_rotation) != 0)
		{ delete [] owned_rotation; owned_rotation = NULL; }
	else
		rotation = owned_rotation;
	}
if (rotation != NULL)								/* rotate the whole pool in place */
	{
	float *tmp = new float[dimension];
	for (long long t = 0; t < total_tokens; t++)
		{
		ANT_pq_codec::apply_rotation(buffer + t*dimension, dimension, rotation, tmp);
		memcpy(buffer + t*dimension, tmp, (size_t)(dimension*sizeof(float)));
		}
	delete [] tmp;
	}

/* --- codebook: external (borrowed) OR trained (owned) --- */
float *owned_codebook = NULL;
const float *codebook = NULL;
if (ext_codebook != NULL)
	codebook = ext_codebook;						/* borrowed; do NOT free */
else
	{
	owned_codebook = new float[cb_floats];
	if (ANT_pq_codec::train(buffer, dimension, m, ANT_pq_codec::K, total_tokens, owned_codebook) != 0)
		{ delete [] owned_codebook; delete [] owned_rotation; return 1; }
	codebook = owned_codebook;
	}

unsigned char *codes = new unsigned char[total_tokens*m > 0 ? total_tokens*m : 1];
for (long long t = 0; t < total_tokens; t++)
	ANT_pq_codec::encode(buffer + t*dimension, dimension, m, ANT_pq_codec::K, codebook, codes + t*m);

char *tmp = new char[strlen(filename)+5]; strcpy(tmp, filename); strcat(tmp, ".tmp");
FILE *out = fopen(tmp, "wb");
long ok = out != NULL;
if (ok)
	{
	long long opq_flag = (rotation != NULL) ? 1 : 0;			/* derive from the R actually used */
	unsigned int version = opq_flag ? 2u : 1u;
	long long k = 256;
	ok = fwrite("ANTMVPQ1", 1, 8, out) == 8
		&& fwrite(&version, 4, 1, out) == 1
		&& fwrite(&dimension, 8, 1, out) == 1
		&& fwrite(&documents, 8, 1, out) == 1
		&& fwrite(&total_tokens, 8, 1, out) == 1
		&& fwrite(&m, 8, 1, out) == 1
		&& fwrite(&k, 8, 1, out) == 1
		&& (opq_flag == 0 || fwrite(&opq_flag, 8, 1, out) == 1)
		&& (documents == 0 || fwrite(counts, 4, (size_t)documents, out) == (size_t)documents)
		&& (total_tokens == 0 || fwrite(codes, 1, (size_t)(total_tokens*m), out) == (size_t)(total_tokens*m))
		&& fwrite(codebook, sizeof(float), (size_t)cb_floats, out) == (size_t)cb_floats
		&& (opq_flag == 0 || fwrite(rotation, sizeof(float), (size_t)(dimension*dimension), out) == (size_t)(dimension*dimension));
	if (fclose(out) != 0) ok = 0;
	}
if (ok && rename(tmp, filename) != 0) ok = 0;
if (!ok) remove(tmp);
delete [] tmp; delete [] codes; delete [] owned_codebook; delete [] owned_rotation;	/* NEVER free ext_* */
if (ok) abandon();
return ok ? 0 : 1;
}
```
KEY: only `owned_*` are freed; the borrowed `ext_codebook`/`ext_rotation` are never freed here. `opq_flag`/`version` derive from the `rotation` actually used (external or owned). Non-external path is byte-identical to today (owned train, same writes).

- [ ] **Step 6: Run the test + existing token suites**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_external_codebook 2>&1 | tail -5
./bin/test_mvpq_external_codebook
for t in test_mvpq_opq test_mvpq_store test_pq_token_resident_tier test_v6_search_multivector test_v6_compaction; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: `ALL test_mvpq_external_codebook PASSED` and every existing token suite PASSED (non-external path byte-identical).

- [ ] **Step 7: Commit**

```bash
git add source/multivector_pq_store.h source/multivector_pq_store.cpp tests/test_mvpq_external_codebook.cpp
git commit -m "feat(mvpq): writer external-codebook seam (global codebook groundwork, token epic 2/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Engine global codebook — train-once, sidecar, config v4, build wiring

Mirror dense #22.2 Task 2 for tokens.

**Files:**
- Modify: `atire/atire_segment_index.h` (members `mvpq_global_current`/`global_mvpq_codebook`/`global_mvpq_rotation`; getter; decls `set_multivector_pq_global_codebook`, `ensure_global_mvpq_codebook`, `load_mvpq_codebook`, `save_mvpq_codebook`, `rebuild_mvpq_global_codebook`)
- Modify: `atire/atire_segment_index.cpp` (ctor init 0/NULL, dtor free the 2 buffers, `open()` calls `load_mvpq_codebook()` after `load_multivector_pq_config()` when global mode)
- Modify: `atire/atire_segment_index_vector.cpp` (`multivector_pq.config` v3→v4; `save/load_mvpq_codebook`; `set_multivector_pq_global_codebook`; `ensure_global_mvpq_codebook`; wire `build_multivector_pq` writer)
- Test: `tests/test_mvpq_global.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_mvpq_global.cpp` covering (mirror `tests/test_pq_global.cpp`'s engine harness — read it for the token setup; use the `set_rerank_config`/`set_multivector_pq_config`/`add_document(...)`/`flush`/`build_multivector_pq` sequence from `tests/test_mvpq_opq_config.cpp`):
- `test_train_once_and_persistence`: open, `set_multivector_pq_config(...)`, `set_multivector_pq_global_codebook(1)`==0 and `multivector_pq_global_codebook()==1`; add docs + flush + `build_multivector_pq()` (segment 0); add more + flush + `build_multivector_pq()` (segment 1); assert `<dir>/multivector_pq.codebook` exists; close; reopen; `multivector_pq_global_codebook()==1` (persisted v4); token search sane.
- `test_cross_segment_comparability`: the SAME token vector present in two segments encodes to identical `token_codes` bytes under global mode (load each `seg_*.mvpq` and compare the probe token's codes + embedded codebooks byte-equal).
- `test_default_off_no_codebook_file`: without `set_multivector_pq_global_codebook`, no `multivector_pq.codebook` sidecar is written; existing behavior unchanged.

Write the full test source following those references (helpers `make_dir`, add-docs, `seg_%06lld.mvpq` path building).

- [ ] **Step 2: Run test to verify it fails** — `make test_mvpq_global` → FAIL (`set_multivector_pq_global_codebook`/`multivector_pq_global_codebook` undeclared).

- [ ] **Step 3: Header + ctor/dtor + open()** — mirror dense (`atire_segment_index.h` members near `mvpq_opq_current`; getter `multivector_pq_global_codebook()`; the 5 decls). Ctor init `mvpq_global_current=0; global_mvpq_codebook=NULL; global_mvpq_rotation=NULL;`. Dtor `delete [] global_mvpq_codebook; delete [] global_mvpq_rotation;`. In `open()`, after `load_multivector_pq_config()`, add `if (mvpq_global_current) load_mvpq_codebook();`.

- [ ] **Step 4: `multivector_pq.config` v4** — in `load/save_multivector_pq_config` bump v3→v4, append a `global` i64 (6th val); back-compat v1(3)/v2(4)/v3(5) ⇒ global=0; validate `global ∈ {0,1}`. Mirror the T1 opq v3 addition exactly (extend the `vals[]` array to 6, add a `version==4` read branch, assign `mvpq_global_current`).

- [ ] **Step 5: `save_mvpq_codebook` / `load_mvpq_codebook`** — mirror dense `save_pq_codebook`(~760)/`load_pq_codebook`(~799) with: magic `ANTMVGCB`; `dimension=rerank_dimension_current`, `m=mvpq_m_current`, `k=256`, `opq=mvpq_opq_current`; sidecar path `<dir>/multivector_pq.codebook`; validate-before-alloc (D≤65536), exact remaining-size check, forgiving-degrade to NULL on any mismatch.

- [ ] **Step 6: `set_multivector_pq_global_codebook` + `ensure_global_mvpq_codebook`** —
  `set_multivector_pq_global_codebook` mirrors `set_pq_global_codebook` (open + `multivector_pq_configured()`, idempotent, immutable-once, persist, revert on save fail).
  `ensure_global_mvpq_codebook(long which)` mirrors `ensure_global_pq_codebook` but gathers the ragged token pool:
```cpp
long ATIRE_segment_index::ensure_global_mvpq_codebook(long which)
{
char mvec_name[4096];
long long docs, ntok, t;
float *rows;
ANT_multivector_store *src;

if (global_mvpq_codebook != NULL)
	return 0;					// already trained (this session or loaded at open())
if (which < 0 || which >= segment_count || mvpq_m_current == 0)
	return 1;

segment_filename(mvec_name, sizeof(mvec_name), segments[which].generation, "mvec");
docs = segments[which].engine->get_document_count();
src = ANT_multivector_store::load(mvec_name, rerank_dimension_current, docs);
ntok = src->token_count();
if (ntok <= 0)
	{ delete src; return 1; }

rows = new float[ntok * rerank_dimension_current];
for (t = 0; t < ntok; t++)
	src->token_reconstruct(t, rows + t * rerank_dimension_current);
delete src;

if (mvpq_opq_current)
	{
	global_mvpq_rotation = new float[rerank_dimension_current * rerank_dimension_current];
	if (ANT_pq_codec::train_rotation(rows, rerank_dimension_current, mvpq_m_current, ntok, global_mvpq_rotation) != 0)
		{ delete [] global_mvpq_rotation; global_mvpq_rotation = NULL; delete [] rows; return 1; }
	float *tmp = new float[rerank_dimension_current];
	for (t = 0; t < ntok; t++)
		{
		ANT_pq_codec::apply_rotation(rows + t*rerank_dimension_current, rerank_dimension_current, global_mvpq_rotation, tmp);
		memcpy(rows + t*rerank_dimension_current, tmp, (size_t)rerank_dimension_current*sizeof(float));
		}
	delete [] tmp;
	}

{
long long sub = rerank_dimension_current / mvpq_m_current;
long long floats = mvpq_m_current * (long long)ANT_pq_codec::K * sub;
global_mvpq_codebook = new float[floats > 0 ? floats : 1];
if (ANT_pq_codec::train(rows, rerank_dimension_current, mvpq_m_current, ANT_pq_codec::K, ntok, global_mvpq_codebook) != 0)
	{ delete [] global_mvpq_codebook; global_mvpq_codebook = NULL; delete [] global_mvpq_rotation; global_mvpq_rotation = NULL; delete [] rows; return 1; }
}
delete [] rows;

if (save_mvpq_codebook() != 0)
	{ delete [] global_mvpq_codebook; global_mvpq_codebook = NULL; delete [] global_mvpq_rotation; global_mvpq_rotation = NULL; return 1; }
return 0;
}
```
(Confirm `ANT_multivector_store::load`, `token_count`, `token_reconstruct`, `segment_filename(...,"mvec")`, and `segments[which].generation`/`engine` by grep — the `.mvec` extension and the resident-float path names must match how `build_multivector_pq` reads the pool.)

- [ ] **Step 7: Wire `build_multivector_pq`** — at the `.mvpq` writer create/finish site (`atire_segment_index_vector.cpp:~1937`), under `mvpq_global_current`: `ensure_global_mvpq_codebook((long)which)` then `if (global_mvpq_codebook != NULL) w.set_external_codebook(global_mvpq_codebook, global_mvpq_rotation);` before `finish()`. (Grep the exact `which`/segment variable at that site.)

- [ ] **Step 8: Run the test + full token regression + commit**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_global 2>&1 | tail -5 && ./bin/test_mvpq_global
for t in test_mvpq_external_codebook test_mvpq_opq test_mvpq_opq_config test_mvpq_store test_pq_token_resident_tier test_v6_search_multivector test_v6_compaction test_segment_index; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_mvpq_global.cpp
git commit -m "feat(mvpq): engine global codebook — train-once, sidecar, config v4, build wiring (token epic 2/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```
Expected: all PASSED; default-off writes no sidecar (byte-identical).

---

### Task 3: Compaction no-retrain + `rebuild_mvpq_global_codebook`

**Files:**
- Modify: `atire/atire_segment_index_compaction.cpp` (compaction `.mvpq` writer reuses the frozen codebook)
- Modify: `atire/atire_segment_index_vector.cpp` (`rebuild_mvpq_global_codebook`)
- Test: `tests/test_mvpq_global_compaction.cpp` (new)

- [ ] **Step 1: Write the failing test** — mirror `tests/test_pq_global.cpp`'s compaction/rebuild cases for tokens:
- `test_compaction_reuses_global_codebook`: ≥2 global-mode segments; capture `multivector_pq.codebook` bytes; `compact(gens, 2)`; assert sidecar bytes byte-identical (no retrain) + merged `.mvpq` embeds the same codebook; token search sane.
- `test_rebuild_global_codebook`: after adding differently-distributed tokens, `rebuild_mvpq_global_codebook()` → sidecar bytes DIFFER; every segment re-embeds the new codebook; search sane.
- `test_opq_global_composition`: global + `set_multivector_pq_opq(1)` — one R+codebook; sidecar carries opq=1 + R; cross-segment codes comparable.

- [ ] **Step 2: Run** → FAIL (`rebuild_mvpq_global_codebook` undeclared; compaction still retrains).

- [ ] **Step 3: Compaction no-retrain** — at the compaction `.mvpq` writer site (`atire_segment_index_compaction.cpp:667`), under `mvpq_global_current`: `ensure_global_mvpq_codebook((long)(segment_count-1))` (the merged output is the newly appended segment; guard fail-soft if NULL) + `w.set_external_codebook(global_mvpq_codebook, global_mvpq_rotation)` before `finish()`. (Confirm the merged-output segment index — mirror how dense compaction picks `segments[segment_count-1]`; grep the surrounding code.)

- [ ] **Step 4: `rebuild_mvpq_global_codebook`** — mirror dense `rebuild_pq_global_codebook`(~932): gather ALL segments' tokens (load each `.mvec`, `token_reconstruct` into a combined `rows` buffer) BEFORE freeing the old codebook (fail-soft ordering); retrain R+codebook; `save_mvpq_codebook`; then re-encode every segment's `.mvpq` (load its `.mvec`, write via a writer with `set_external_codebook(global_mvpq_codebook, global_mvpq_rotation)`); refresh `segments[].multivector_pq`. Per-segment re-encode failure skips (stale `.mvpq`, overall nonzero), never aborts mid-way. Returns nonzero if global/token-PQ unconfigured or no tokens.

- [ ] **Step 5: Run + full regression + commit**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_global_compaction 2>&1 | tail -5 && ./bin/test_mvpq_global_compaction
for t in test_mvpq_external_codebook test_mvpq_global test_mvpq_opq test_mvpq_store test_pq_token_resident_tier test_v6_search_multivector test_v6_compaction test_v6_recall test_segment_index; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
git add atire/atire_segment_index_compaction.cpp atire/atire_segment_index_vector.cpp tests/test_mvpq_global_compaction.cpp
git commit -m "feat(mvpq): compaction reuses global codebook (no retrain) + rebuild_mvpq_global_codebook (token epic 2/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Notes for the final holistic review (after all 3 tasks)

- **`finish()` owned-vs-borrowed:** every exit frees only `owned_*` (+`codes`/`tmp`), NEVER `ext_*`; external path skips both `train` and `train_rotation`; `opq_flag` follows the used `rotation`; non-external byte-identical.
- **`multivector_pq.codebook` forgiving-load:** validate-before-alloc (dim/m/k/opq/exact-size), D≤65536 bounds D²; mismatch → untrained NULL.
- **`multivector_pq.config` v4 back-compat:** v1/v2/v3 load (global=0); v4 reads+validates `global`.
- **`ensure`/`rebuild` fail-soft:** leave `global_mvpq_codebook` NULL on any failure (never half-trained); rebuild gathers before freeing.
- **Compaction:** `segment_count-1` is the merged output; fail-soft if NULL; per-segment default-off path unchanged.
- **Default off byte-identity + compose:** no `set_multivector_pq_global_codebook` ⇒ per-segment training as today; composes with T1 OPQ (shared R) + #24 tiers.
- **No cross-segment borrow:** `ANT_multivector_pq_store::load` copies the embedded codebook → no `.mvpq` aliases the engine buffer (single-resident borrow is the later T4).
- ASan/UBSan environment-blocked — report, don't attempt.
```

# Token `.mvpq` Single-Resident Global Codebook Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Under global mode, make every per-segment `ANT_multivector_pq_store` BORROW the engine's single resident `global_mvpq_codebook` (+`global_mvpq_rotation` under OPQ) instead of loading its own embedded copy into RAM — cutting resident token-codebook memory from N+1 copies to 1, with byte-identical search results.

**Architecture:** Direct mirror of the shipped dense single-resident feature (Approach A, 0cbc25f, `source/pq_store.{h,cpp}` + `rebuild_pq_global_codebook`). The `.mvpq` on-disk format is UNCHANGED (each file still embeds its copy → self-describing/back-compatible); only the in-RAM load path skips reading it. `owns_codebook`/`owns_rotation` flags guard the dtor so a borrowing store never frees/derefs the shared buffer (teardown-order-independent). Unconditional under global mode — no config toggle.

**Tech Stack:** C++ engine. Files: `source/multivector_pq_store.{h,cpp}` (borrow seam), `atire/atire_segment_index.cpp` + `atire/atire_segment_index_vector.cpp` (load-site wiring + rebuild drop-all/had_pq), tests `tests/*.cpp`.

**Key reference (crib exact shapes):**
- Dense store `source/pq_store.{h,cpp}`: `owns_codebook`/`owns_rotation` (h:25-26), borrow predicate (cpp:194), borrow-skip-read load path (cpp:196-251), `codebook_is_borrowed()` (h:50).
- Dense engine `atire/atire_segment_index_vector.cpp`: `rebuild_pq_global_codebook` Approach-A ordering (drop-all-borrowing-after-free + `had_pq[]` snapshot) at ~1009-1109; load-site wiring passing `global_pq_codebook`/`global_pq_rotation`.
- Current token store `source/multivector_pq_store.{h,cpp}`: `load` is 4-arg, dtor frees `codebook`/`rotation` unconditionally, engine already owns `global_mvpq_codebook`/`global_mvpq_rotation` (T3) sized `k*dim` (T2).

**Global constraints (every task):**
- Commit ONLY the files named in that task's `git add`. NEVER `git add -A`. NEVER stage build artifacts (`.o`/`.a`/`.so`/`.node`) or data sidecars (`.mvpq`/`.mvec`/`multivector_pq.*`/`.tann`) or the untracked `docs/business-strategy-2026-07-07.md`. After committing, confirm `git status --short` shows that doc still `??` (or absent).
- After editing any `.h`: `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding.
- Config setters are POST-open. Tests must `open(dir)` before any `set_*`.
- Tests auto-discovered: `make <name>` → `bin/<name>`; `CHECK(cond)` aborts on failure.
- Do NOT push or touch remotes. ASan/UBSan environment-blocked — report, don't attempt.
- Do NOT modify `source/pq_codec.*`. The `.mvpq` on-disk format does NOT change.

---

## Task 1: Store borrow seam (`source/multivector_pq_store.{h,cpp}`)

Add `owns_codebook`/`owns_rotation`, give `load()` defaulted `borrowed_codebook`/`borrowed_rotation` params, borrow when supplied + opq-consistent (skip reading the embedded codebook/rotation into RAM, point at the borrowed buffers), guard the dtor, and add `codebook_is_borrowed()`.

**Files:**
- Modify: `source/multivector_pq_store.h`
- Modify: `source/multivector_pq_store.cpp`
- Test: `tests/test_mvpq_borrow.cpp` (create)

- [ ] **Step 1: Write the failing test** — `tests/test_mvpq_borrow.cpp`

```cpp
/*
	TEST_MVPQ_BORROW.CPP -- token epic 4/4 Task 1: single-resident borrow seam.
	A store loaded with a hand-supplied borrowed codebook points AT it (skips the
	embedded copy), reports is_borrowed, reconstructs identically to the owned
	load, and its dtor frees nothing shared. Declines (owns embedded) on opq
	mismatch.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 8
#define MM 4

static void fill(long long seed, float *v)
{ double n=0; for (int j=0;j<DIM;j++){v[j]=(float)(((seed*7+j*3)%13)-6)/6.0f;n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<DIM;j++)v[j]/=(float)n; }

/* build a non-OPQ k=256 .mvpq over ndoc docs (2 tokens each) */
static void build(const char *path, long long ndoc)
{
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, DIM, MM, 256, ANT_pq_codec::METRIC_DOT, 0) == 0);
	for (long long d = 0; d < ndoc; d++)
		{ float rows[2*DIM]; fill(d*3, rows); fill(d*3+1, rows+DIM); CHECK(w.append(rows, 2) == 0); }
	CHECK(w.finish() == 0);
}

static void test_borrow_points_at_supplied_codebook(void)
{
	char path[] = "/tmp/ant_mvb_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	const long long ndoc = 16;
	build(path, ndoc);

	/* owned load: get the canonical codebook + a reconstruction */
	ANT_multivector_pq_store *owned = ANT_multivector_pq_store::load(path, DIM, ndoc, ANT_pq_codec::METRIC_DOT);
	CHECK(owned != NULL && owned->token_count() == 2*ndoc);
	CHECK(owned->codebook_is_borrowed() == 0);						/* default = owns */
	long long cb_floats = 256 * DIM;
	float *resident = new float[cb_floats];
	memcpy(resident, owned->get_codebook(), (size_t)cb_floats * sizeof(float));
	float rec_owned[DIM]; owned->token_reconstruct(0, rec_owned);
	delete owned;

	/* borrowed load: hand it the resident copy, non-OPQ (rotation NULL) */
	ANT_multivector_pq_store *bor = ANT_multivector_pq_store::load(path, DIM, ndoc, ANT_pq_codec::METRIC_DOT, resident, NULL);
	CHECK(bor != NULL && bor->token_count() == 2*ndoc);
	CHECK(bor->codebook_is_borrowed() == 1);						/* borrowed */
	CHECK(bor->get_codebook() == resident);							/* points AT the supplied buffer */
	float rec_bor[DIM]; bor->token_reconstruct(0, rec_bor);
	for (int j = 0; j < DIM; j++) CHECK(fabs(rec_bor[j] - rec_owned[j]) < 1e-6);	/* identical to owned */
	delete bor;														/* must NOT free `resident` */
	/* resident still valid after the borrowing store's dtor: */
	double s = 0; for (long long i = 0; i < cb_floats; i++) s += resident[i];
	CHECK(s == s);													/* not a UAF (no crash / not NaN-from-freed) */
	delete [] resident;
	remove(path);
	printf("test_borrow_points_at_supplied_codebook OK\n");
}

/* opq mismatch: file is non-OPQ (opq==0) but caller supplies a rotation -> decline, own embedded. */
static void test_borrow_declines_on_opq_mismatch(void)
{
	char path[] = "/tmp/ant_mvbm_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	const long long ndoc = 10;
	build(path, ndoc);							/* opq off */
	float dummy_cb[256*DIM] = {0};
	float dummy_rot[DIM*DIM] = {0};
	/* supplying a rotation for a non-OPQ file -> (stored_opq==1)==(rot!=NULL) is (0)==(1) = false -> no borrow */
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, DIM, ndoc, ANT_pq_codec::METRIC_DOT, dummy_cb, dummy_rot);
	CHECK(s != NULL && s->token_count() == 2*ndoc);
	CHECK(s->codebook_is_borrowed() == 0);		/* declined -> owns its embedded copy */
	CHECK(s->get_codebook() != dummy_cb);		/* did NOT point at the supplied buffer */
	delete s;
	remove(path);
	printf("test_borrow_declines_on_opq_mismatch OK\n");
}

int main(void)
{
	test_borrow_points_at_supplied_codebook();
	test_borrow_declines_on_opq_mismatch();
	printf("ALL test_mvpq_borrow PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_mvpq_borrow && ./bin/test_mvpq_borrow`
Expected: FAIL to compile — `load(...)` has no 6-arg (borrowed) overload and `codebook_is_borrowed()` does not exist.

- [ ] **Step 3: Header — `owns_*` members, `load` signature, accessor**

In `source/multivector_pq_store.h`, in the `private:` block of `ANT_multivector_pq_store` (near `float *codebook;` / `float *rotation;`), add:

```cpp
	long owns_codebook;		// 1 = this store allocated codebook (free in dtor); 0 = borrowed (engine-owned, never freed/deref'd here)
	long owns_rotation;		// 1 = owned; 0 = borrowed
```

Change the `load` declaration to add two defaulted trailing params:

```cpp
	static ANT_multivector_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric, const float *borrowed_codebook = NULL, const float *borrowed_rotation = NULL);
```

Add the accessor next to `get_codebook`:

```cpp
	long codebook_is_borrowed(void) { return owns_codebook == 0; }
```

- [ ] **Step 4: ctor init + dtor guard**

In `source/multivector_pq_store.cpp`, the private ctor initializer list (currently `counts(0), offsets(0), codebook(0), codes(0), rotation(0), adc_table_builds(0)`) — add `owns_codebook(1), owns_rotation(1)`:

```cpp
	counts(0), offsets(0), codebook(0), codes(0), rotation(0), owns_codebook(1), owns_rotation(1), adc_table_builds(0) {}
```

Change the dtor (currently `delete [] counts; delete [] offsets; delete [] codebook; delete [] codes; delete [] rotation;`) to guard the two borrowable buffers:

```cpp
ANT_multivector_pq_store::~ANT_multivector_pq_store()
{
delete [] counts; delete [] offsets; delete [] codes;
if (owns_codebook) delete [] codebook;
if (owns_rotation) delete [] rotation;
}
```

- [ ] **Step 5: `load` — accept borrowed params, borrow-skip-read, point-at-borrowed**

In `source/multivector_pq_store.cpp`, update the `load` definition signature to match the header (add `const float *borrowed_codebook, const float *borrowed_rotation` — NO defaults in the definition). Then, AFTER the `expected_size` validation and `fseek(in, header_size, SEEK_SET)` (i.e. where the buffers are currently allocated and read), replace the allocate+read+assign region with the borrow-aware version. The current region is:

```cpp
int *counts = new int[docs > 0 ? docs : 1];
long long *offsets = new long long[docs + 1];
unsigned char *codes = new unsigned char[toks*row_bytes > 0 ? toks*row_bytes : 1];
float *codebook = new float[cb_floats > 0 ? cb_floats : 1];
float *rotation = rot_floats > 0 ? new float[rot_floats] : NULL;

long ok = 1;
if (docs > 0 && fread(counts, 4, (size_t)docs, in) != (size_t)docs) ok = 0;
if (ok && toks > 0 && fread(codes, 1, (size_t)(toks*row_bytes), in) != (size_t)(toks*row_bytes)) ok = 0;
if (ok && fread(codebook, sizeof(float), (size_t)cb_floats, in) != (size_t)cb_floats) ok = 0;
if (ok && rot_floats > 0 && fread(rotation, sizeof(float), (size_t)rot_floats, in) != (size_t)rot_floats) ok = 0;
fclose(in);
```

Replace it with (mirror dense pq_store.cpp:194-251 — disk order is counts, codes, codebook, rotation; borrow skips codebook + rotation reads via `fseek`):

```cpp
/*
	Approach A (single-resident): if a borrowed codebook is supplied AND the
	file's opq flag is consistent with the supplied rotation (rotation present
	iff opq==1), point at the borrowed buffers and SKIP reading the embedded
	codebook/rotation into RAM (fseek past them). Otherwise own an embedded copy
	exactly as before. Header dims/m/k were already validated against expected_*;
	global mode guarantees the resident codebook is the one every .mvpq embeds,
	so no byte-compare is needed.
*/
int borrow = (borrowed_codebook != NULL) && ((opq == 1) == (borrowed_rotation != NULL));

int *counts = new int[docs > 0 ? docs : 1];
long long *offsets = new long long[docs + 1];
unsigned char *codes = new unsigned char[toks*row_bytes > 0 ? toks*row_bytes : 1];
float *codebook = borrow ? NULL : new float[cb_floats > 0 ? cb_floats : 1];
float *rotation = (borrow || rot_floats == 0) ? NULL : new float[rot_floats];

long ok = 1;
if (docs > 0 && fread(counts, 4, (size_t)docs, in) != (size_t)docs) ok = 0;
if (ok && toks > 0 && fread(codes, 1, (size_t)(toks*row_bytes), in) != (size_t)(toks*row_bytes)) ok = 0;
if (ok && cb_floats > 0)					/* codebook block: read OR skip */
	{
	if (borrow)
		{ if (fseek(in, (long)(cb_floats * (long long)sizeof(float)), SEEK_CUR) != 0) ok = 0; }
	else if (fread(codebook, sizeof(float), (size_t)cb_floats, in) != (size_t)cb_floats) ok = 0;
	}
if (ok && rot_floats > 0)					/* rotation block: read OR skip */
	{
	if (borrow)
		{ if (fseek(in, (long)(rot_floats * (long long)sizeof(float)), SEEK_CUR) != 0) ok = 0; }
	else if (fread(rotation, sizeof(float), (size_t)rot_floats, in) != (size_t)rot_floats) ok = 0;
	}
fclose(in);
```

Then the existing `if (ok) { offsets prefix-sum ... }` block is UNCHANGED. The cleanup line `if (!ok) { delete [] counts; delete [] offsets; delete [] codes; delete [] codebook; delete [] rotation; return s; }` stays as-is (both `codebook`/`rotation` are NULL under borrow, so `delete []` is a safe no-op). Finally, change the success assignment block from:

```cpp
s->counts = counts; s->offsets = offsets; s->codes = codes; s->codebook = codebook; s->rotation = rotation;
```

to the borrow-aware assignment (mirror dense cpp:242-251):

```cpp
s->counts = counts; s->offsets = offsets; s->codes = codes;
if (borrow)
	{
	s->codebook = (float *)borrowed_codebook; s->owns_codebook = 0;
	s->rotation = (opq == 1) ? (float *)borrowed_rotation : NULL; s->owns_rotation = 0;
	}
else
	{
	s->codebook = codebook; s->owns_codebook = 1;
	s->rotation = rotation; s->owns_rotation = 1;	/* rotation NULL when opq==0 */
	}
```

(Confirm by grep that `opq` is the local variable name holding the file's opq flag in this function — the header read block sets `long long opq = 0; ... opq = read_i64(ohdr);`. If it's named differently, use the actual name.)

- [ ] **Step 6: Rebuild and run the test + store regressions**

Run:
```
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_borrow && ./bin/test_mvpq_borrow
make test_mvpq_store && ./bin/test_mvpq_store
make test_mvpq_opq && ./bin/test_mvpq_opq
make test_mvpq_variable_k && ./bin/test_mvpq_variable_k
```
Expected: `ALL test_mvpq_borrow PASSED`; the three regressions still PASS (defaulted params ⇒ every existing 4-arg `load` caller is the owned path, byte-identical).

- [ ] **Step 7: Commit**

```bash
git add source/multivector_pq_store.h source/multivector_pq_store.cpp tests/test_mvpq_borrow.cpp
git commit -m "feat(mvpq): store borrow seam for single-resident global codebook (token epic 4/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git status --short | grep -q business-strategy && echo "strategy doc still untracked OK" || echo "(strategy doc absent in worktree)"
```

---

## Task 2: Engine load-site wiring (`atire/atire_segment_index*`)

Pass `global_mvpq_codebook`/`global_mvpq_rotation` to `ANT_multivector_pq_store::load` at all three sites under global mode, so segments borrow the resident codebook. NULL when global mode off ⇒ owned path unchanged.

**Files:**
- Modify: `atire/atire_segment_index.cpp` (`open()` load site ~1640)
- Modify: `atire/atire_segment_index_vector.cpp` (rebuild Pass-2 reload ~1732, ensure/build reload ~2420)
- Test: `tests/test_mvpq_single_resident.cpp` (create)

- [ ] **Step 1: Write the failing test** — `tests/test_mvpq_single_resident.cpp`

```cpp
/*
	TEST_MVPQ_SINGLE_RESIDENT.CPP -- token epic 4/4 Task 2: engine load-site borrow.
	Under global mode every segment's loaded .mvpq store BORROWS the engine's
	resident global_mvpq_codebook; search is byte-identical to the owned baseline;
	teardown is order-independent. Global off -> owned (no borrow).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define RD 8
#define MM 4

static char *mkdir_tmp(const char *tmpl)
{ char b[64]; strcpy(b, tmpl); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r, d); return r; }
static void fill(long long seed, float *v)
{ double n=0; for (int j=0;j<RD;j++){v[j]=(float)(((seed*7+j*3)%13)-6)/6.0f;n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<RD;j++)v[j]/=(float)n; }
static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{ for (long long i=lo;i<hi;i++){ float rows[3*RD]; for(int r=0;r<3;r++) fill(i*5+r, rows+r*RD);
  char key[32]; snprintf(key,sizeof(key),"d-%lld",i); CHECK(ix->add_document(key,"body",NULL,rows,3)>=0); } }

/* build 2 global-mode segments, reopen, and assert every segment store borrows the
   resident codebook (pointer identity) and search works. Then delete the engine
   (resident freed, then store dtors run) -> no crash/double-free. */
static void test_borrow_active_and_teardown(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msr1_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 12); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 12, 24); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	delete ix;

	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);					/* open() load sites now borrow */
	CHECK(re->disk_segment_count() == 2);
	CHECK(re->multivector_pq_global_codebook() == 1);
	const float *resident = re->debug_global_mvpq_codebook();		/* accessor added in Step 5 */
	CHECK(resident != NULL);
	for (long long w = 0; w < re->disk_segment_count(); w++)
		{
		const ANT_multivector_pq_store *st = re->debug_segment_multivector_pq(w);	/* accessor added in Step 5 */
		CHECK(st != NULL);
		CHECK(((ANT_multivector_pq_store *)st)->codebook_is_borrowed() == 1);
		CHECK(((ANT_multivector_pq_store *)st)->get_codebook() == resident);			/* points at resident */
		}
	CHECK(re->build_token_index() == 0);
	float q[2*RD]; fill(1, q); fill(2, q+RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);
	delete re;									/* engine dtor frees resident then segment stores -> must not double-free */
	delete [] dir;
	printf("test_borrow_active_and_teardown OK\n");
}

/* global OFF -> stores own their embedded copy (no borrow). */
static void test_global_off_owns(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msr2_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* NO global codebook */
	add_docs(ix, 0, 12); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	delete ix;
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	const ANT_multivector_pq_store *st = re->debug_segment_multivector_pq(0);
	CHECK(st != NULL && ((ANT_multivector_pq_store *)st)->codebook_is_borrowed() == 0);	/* owns embedded */
	delete re; delete [] dir;
	printf("test_global_off_owns OK\n");
}

int main(void)
{
	test_borrow_active_and_teardown();
	test_global_off_owns();
	printf("ALL test_mvpq_single_resident PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_mvpq_single_resident && ./bin/test_mvpq_single_resident`
Expected: FAIL to compile — the `debug_global_mvpq_codebook()` / `debug_segment_multivector_pq()` test accessors don't exist yet (Step 5 adds them), AND (once they exist) `codebook_is_borrowed()` would be 0 under global mode because the load sites don't pass the resident codebook yet.

- [ ] **Step 3: Wire the `open()` load site** — `atire/atire_segment_index.cpp` (~1640)

Find the `.mvpq` load in `open()`:

```cpp
ANT_multivector_pq_store *p = ANT_multivector_pq_store::load(mvpq_filename, rerank_dimension_current, engine->get_document_count(), ANT_pq_codec::METRIC_DOT);
```

Replace with the borrow-aware call (add the two resident pointers, gated on global mode):

```cpp
const float *bcb = (mvpq_global_current && global_mvpq_codebook != NULL) ? global_mvpq_codebook : NULL;
const float *brot = (bcb != NULL) ? global_mvpq_rotation : NULL;
ANT_multivector_pq_store *p = ANT_multivector_pq_store::load(mvpq_filename, rerank_dimension_current, engine->get_document_count(), ANT_pq_codec::METRIC_DOT, bcb, brot);
```

NOTE: `open()` calls `load_mvpq_codebook()` (which populates `global_mvpq_codebook`) BEFORE iterating segments — confirm by grep that the codebook load precedes the segment-store load loop. If it does not, the borrow silently declines (bcb NULL) and every segment owns its copy (correct but no RAM win). Verify the order; if the segment loop runs first, no reorder is needed for correctness but note it. (In practice `load_mvpq_codebook` is called in `open()` under global mode before the per-segment `append_segment`/load — confirm.)

- [ ] **Step 4: Wire the two reload sites** — `atire/atire_segment_index_vector.cpp` (~1732 and ~2420)

At BOTH reload sites, currently:

```cpp
segments[which].multivector_pq = ANT_multivector_pq_store::load(mvpq_name, rerank_dimension_current, docs, ANT_pq_codec::METRIC_DOT);
```

Replace EACH with:

```cpp
const float *bcb = (mvpq_global_current && global_mvpq_codebook != NULL) ? global_mvpq_codebook : NULL;
const float *brot = (bcb != NULL) ? global_mvpq_rotation : NULL;
segments[which].multivector_pq = ANT_multivector_pq_store::load(mvpq_name, rerank_dimension_current, docs, ANT_pq_codec::METRIC_DOT, bcb, brot);
```

(Grep `ANT_multivector_pq_store::load(` across the file to confirm exactly these two call sites in `atire_segment_index_vector.cpp` — the rebuild Pass-2 reload and the ensure/build reload after `finish()`. The rebuild one in Task 3 gets the additional ordering fix, but the borrow arg is added here.)

- [ ] **Step 5: Add the two test-only debug accessors** — `atire/atire_segment_index.h`

To let the test assert pointer identity without exposing internals broadly, add two accessors near the other `multivector_pq_*` getters:

```cpp
	const float *debug_global_mvpq_codebook(void) { return global_mvpq_codebook; }
	ANT_multivector_pq_store *debug_segment_multivector_pq(long long which) { return (which >= 0 && which < segment_count) ? segments[which].multivector_pq : NULL; }
```

(If `ANT_multivector_pq_store` is not already a visible type in this header, it is forward-declared / included — grep; `segment` already holds a `multivector_pq` member so the type is available. If only a forward declaration exists, returning a pointer is fine.)

- [ ] **Step 6: Rebuild and run**

Run:
```
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_single_resident && ./bin/test_mvpq_single_resident
make test_mvpq_borrow && ./bin/test_mvpq_borrow
make test_mvpq_global && ./bin/test_mvpq_global
make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier
```
Expected: `ALL test_mvpq_single_resident PASSED` (borrow active + pointer identity + teardown clean; global-off owns); the regressions PASS.

- [ ] **Step 7: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_mvpq_single_resident.cpp
git commit -m "feat(mvpq): engine borrows resident global codebook at .mvpq load sites (token epic 4/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git status --short | grep -q business-strategy && echo "strategy doc still untracked OK" || echo "(strategy doc absent in worktree)"
```

---

## Task 3: Rebuild drop-all + `had_pq` snapshot + composition

Make `rebuild_mvpq_global_codebook` safe under borrowing: freeing/replacing the resident codebook invalidates every borrowing store's pointer, so capture a `had_pq[]` eligibility snapshot BEFORE the drop, free the old resident + drop every borrowing store to NULL, train+save the new resident, then Pass-2 reload re-borrows the new resident. Add composition tests (k≠256, OPQ, NONE-tier).

**Files:**
- Modify: `atire/atire_segment_index_vector.cpp` (`rebuild_mvpq_global_codebook`)
- Test: `tests/test_mvpq_single_resident_rebuild.cpp` (create)

- [ ] **Step 1: Write the failing test** — `tests/test_mvpq_single_resident_rebuild.cpp`

```cpp
/*
	TEST_MVPQ_SINGLE_RESIDENT_REBUILD.CPP -- token epic 4/4 Task 3: rebuild under
	borrowing. rebuild_mvpq_global_codebook frees+retrains the resident; borrowing
	stores must be dropped then reloaded re-borrowing the NEW codebook (no UAF, no
	skipped re-encode); composes with k!=256 / OPQ / NONE-tier.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define RD 8
#define MM 4

static char *mkdir_tmp(const char *tmpl)
{ char b[64]; strcpy(b, tmpl); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r, d); return r; }
static void fill(long long seed, float *v)
{ double n=0; for (int j=0;j<RD;j++){v[j]=(float)(((seed*7+j*3)%13)-6)/6.0f;n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<RD;j++)v[j]/=(float)n; }
static void fill_shift(long long seed, float *v)		/* different distribution for rebuild */
{ double n=0; for (int j=0;j<RD;j++){v[j]=(float)(((seed*11+j*5)%17)-8)/8.0f * (j==0?3.0f:1.0f);n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<RD;j++)v[j]/=(float)n; }
static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi, int shift)
{ for (long long i=lo;i<hi;i++){ float rows[3*RD]; for(int r=0;r<3;r++){ if(shift) fill_shift(i*5+r,rows+r*RD); else fill(i*5+r,rows+r*RD);} 
  char key[32]; snprintf(key,sizeof(key),"d-%lld",i); CHECK(ix->add_document(key,"body",NULL,rows,3)>=0); } }

/* rebuild under borrowing (global mode, default FLOAT tier), k=16, then assert every
   segment re-borrows the NEW resident codebook and search is sane (no UAF). */
static void test_rebuild_reborrow(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msrr_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 12, 0); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 12, 24, 0); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	/* a third, differently-distributed segment so rebuild actually changes the codebook */
	add_docs(ix, 100, 112, 1); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);

	CHECK(ix->rebuild_mvpq_global_codebook() == 0);
	const float *resident = ix->debug_global_mvpq_codebook();
	CHECK(resident != NULL);
	for (long long w = 0; w < ix->disk_segment_count(); w++)
		{
		const ANT_multivector_pq_store *st = ix->debug_segment_multivector_pq(w);
		CHECK(st != NULL);
		CHECK(((ANT_multivector_pq_store *)st)->codebook_is_borrowed() == 1);		/* re-borrowed */
		CHECK(((ANT_multivector_pq_store *)st)->get_codebook() == resident);			/* the NEW resident */
		}
	CHECK(ix->build_token_index() == 0);
	float q[2*RD]; fill(3, q); fill(7, q+RD);
	CHECK(ix->search_multivector(q, 2, 10) > 0);					/* no UAF, sane */
	delete ix; delete [] dir;
	printf("test_rebuild_reborrow OK\n");
}

/* compose: borrow + OPQ + NONE-tier + rebuild (the memory-heaviest corner). */
static void test_borrow_opq_none_tier_rebuild(void)
{
	char *dir = mkdir_tmp("/tmp/ant_msron_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_opq(1) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 14, 0); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 100, 114, 1); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);

	/* switch to NONE tier and reopen so token_source wraps borrowing .mvpq stores */
	CHECK(ix->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete ix;
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	/* under OPQ+global the stores borrow BOTH codebook and rotation */
	const ANT_multivector_pq_store *st0 = re->debug_segment_multivector_pq(0);
	CHECK(st0 != NULL && ((ANT_multivector_pq_store *)st0)->codebook_is_borrowed() == 1);
	CHECK(re->build_token_index() == 0);
	float q[2*RD]; fill(1, q); fill(2, q+RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);					/* graph path, borrowed rotation used */
	CHECK(re->rebuild_mvpq_global_codebook() == 0);					/* rebuild under NONE-tier borrowing */
	for (long long w = 0; w < re->disk_segment_count(); w++)
		CHECK(re->disk_segment_has_token_index(w) == 0);			/* T3 UAF guard: token_index invalidated */
	CHECK(re->build_token_index() == 0);
	CHECK(re->search_multivector(q, 2, 10) > 0);					/* sane after rebuild */
	delete re; delete [] dir;
	printf("test_borrow_opq_none_tier_rebuild OK\n");
}

int main(void)
{
	test_rebuild_reborrow();
	test_borrow_opq_none_tier_rebuild();
	printf("ALL test_mvpq_single_resident_rebuild PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_mvpq_single_resident_rebuild && ./bin/test_mvpq_single_resident_rebuild`

**Honest note on the red:** because Task 2 already added the borrow arg to the Pass-2 reload, the CURRENT (T3) rebuild ordering happens to reload+re-borrow the new resident on the HAPPY path, so these test assertions may already PASS pre-implementation. That is expected and fine — do NOT weaken the test to force a red. The genuine unsafety this task fixes is the *failure* path: under the T3 ordering, a borrowing store whose Pass-2 re-encode FAILS (or any borrowing store between the old-codebook free and its reload) is left pointing at the freed old codebook → a later search is a UAF. That requires fault injection to trigger deterministically (ASan is environment-blocked), so the unit test cannot reliably force it. Treat Task 3 as **structural hardening**: implement the drop-all-borrowing-to-NULL + `had_pq[]` snapshot per Step 3 regardless of whether the test was red — it makes every failure path fail-closed (a dropped store is NULL, never dangling-borrowed) and is the faithful dense Approach-A contract. The test locks the happy-path re-borrow (`get_codebook() == new resident` for every segment) and the NONE-tier UAF guard (`disk_segment_has_token_index==0` post-rebuild); the code-quality review verifies the drop-all/had_pq structure. If the test is green both before and after, record that in your report (as prior tasks in this epic did when a predicted red didn't materialize).

- [ ] **Step 3: Restructure `rebuild_mvpq_global_codebook` to the Approach-A ordering**

Open `atire/atire_segment_index_vector.cpp` and read the CURRENT `rebuild_mvpq_global_codebook` (T3: gather → train into `new_codebook`/`new_rotation` → swap old→new → `save_mvpq_codebook` → rollback-on-save-fail → Pass-2 re-encode+reload with the T3 NONE-tier `token_source`/`token_index` refresh + `.tann` invalidation). Also read the dense `rebuild_pq_global_codebook` at ~1009-1109 for the exact Approach-A shape. Then restructure so the resident swap is borrow-safe. The required ordering:

1. **Gather** the full token pool across all segments into `rows` (UNCHANGED — happens before any free).
2. **Capture the eligibility snapshot BEFORE any drop** (mirror dense:1023-1028):

```cpp
char *had_pq = new char[segment_count > 0 ? segment_count : 1];
for (long long s = 0; s < segment_count; s++)
	{
	long long sdocs = segments[s].engine ? segments[s].engine->get_document_count() : 0;
	had_pq[s] = (segments[s].multivector_pq != NULL && segments[s].multivector_pq->document_count() == sdocs && sdocs > 0 && segments[s].multivector_pq->token_count() > 0) ? 1 : 0;
	}
```

3. **Train the new codebook + rotation into LOCALS** first (`new_codebook`, `new_rotation`) exactly as T3 does — this does NOT touch the resident yet, so borrowing stores stay valid during training. On a train/train_rotation failure: `delete [] new_codebook; delete [] new_rotation; delete [] rows; delete [] had_pq; return 1;` (resident untouched).
4. **Persist FIRST, then swap** (avoids a half-broken resident on save failure): call a save of the NEW codebook. Because `save_mvpq_codebook()` writes the resident members, either (a) temporarily point at locals to save then keep, or (b) mirror dense which frees old + assigns new + saves + fail-closes. Use the dense pattern for exactness: after training locals, do the borrow-safe swap:

```cpp
/* Approach A: freeing the old resident invalidates every borrowing store's codebook
   pointer. Swap in the new resident, then DROP every borrowing store to NULL so nothing
   dereferences the freed buffer; Pass-2 reload (below) re-borrows the NEW resident. */
delete [] global_mvpq_codebook; global_mvpq_codebook = new_codebook;
delete [] global_mvpq_rotation; global_mvpq_rotation = new_rotation;		/* new_rotation NULL when OPQ off */
for (long long s = 0; s < segment_count; s++)
	if (segments[s].multivector_pq != NULL && segments[s].multivector_pq->codebook_is_borrowed())
		{
		delete segments[s].token_index; segments[s].token_index = NULL;		/* graph borrows token_source borrows the store */
		delete segments[s].token_source; segments[s].token_source = NULL;
		delete segments[s].multivector_pq; segments[s].multivector_pq = NULL;
		}
if (save_mvpq_codebook() != 0)
	{ delete [] had_pq; return 1; }		/* persist failed: borrowing stores already NULL -> fail closed to resident float tier; resident holds new unpersisted codebook -> caller must honor nonzero */
```

(Note: a NON-borrowing store — global mode was off when it loaded, e.g. a mixed history — keeps its embedded copy and is left alone here; it will still be re-encoded in Pass-2 if `had_pq[s]`.)

5. **Pass-2: re-encode + reload re-borrowing the NEW resident** — mirror the T3 Pass-2 but gate eligibility on the `had_pq[]` snapshot (the store pointer is now NULL for borrowing segments, so the live check can't be used):

```cpp
long any_failed = 0;
for (long long which = 0; which < segment_count; which++)
	{
	if (!had_pq[which]) continue;					/* captured before the drop */
	/* ... existing T3 Pass-2 body: open a writer with set_external_codebook(global_mvpq_codebook, global_mvpq_rotation),
	   append each doc's tokens from the on-disk float .mvec, finish(); on failure any_failed=1 + continue;
	   then reload the store re-borrowing the NEW resident: */
	const float *bcb = (mvpq_global_current && global_mvpq_codebook != NULL) ? global_mvpq_codebook : NULL;
	const float *brot = (bcb != NULL) ? global_mvpq_rotation : NULL;
	delete segments[which].multivector_pq;
	segments[which].multivector_pq = ANT_multivector_pq_store::load(mvpq_name, rerank_dimension_current, docs, ANT_pq_codec::METRIC_DOT, bcb, brot);
	/* ... existing T3 NONE-tier token_source/token_index refresh + .tann/.tann.g invalidation (UNCHANGED) ... */
	}
delete [] had_pq;
return any_failed ? 1 : 0;
```

Preserve the T3 Pass-2 body verbatim (writer create at `mvpq_k_current`, `set_external_codebook`, append-from-float-`.mvec`, `finish`, the tier-aware `token_source` rebuild, `.tann`/`.tann.g` `remove`) — the ONLY changes are: (a) the `had_pq[]` gate replaces the live `multivector_pq != NULL` eligibility check, (b) the reload passes `bcb`/`brot`, and (c) the pre-Pass-2 free+drop block above replaces the T3 train-locals→swap→save→rollback block. Read the current function carefully and splice these in without disturbing the gather (Pass 1a/1b) or the Pass-2 body.

- [ ] **Step 4: Run the rebuild test + the full token/engine regression**

Run:
```
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_single_resident_rebuild && ./bin/test_mvpq_single_resident_rebuild
```
Expected: `ALL test_mvpq_single_resident_rebuild PASSED` (re-borrow after rebuild; OPQ+NONE-tier rebuild UAF-safe with `disk_segment_has_token_index==0` post-rebuild).

Then the full suite — every one must PASS:
```
for t in test_mvpq_borrow test_mvpq_single_resident test_mvpq_single_resident_rebuild \
         test_mvpq_variable_k test_mvpq_variable_k_config test_mvpq_variable_k_compose \
         test_mvpq_global test_mvpq_external_codebook test_mvpq_opq test_mvpq_opq_config \
         test_mvpq_store test_pq_token_resident_tier test_v6_search_multivector \
         test_v6_compaction test_segment_index; do
  make $t >/dev/null 2>&1 && ./bin/$t >/tmp/sr_$t.log 2>&1 && echo "PASS $t" || echo "FAIL $t"; done
```
Expected: every line `PASS`.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_mvpq_single_resident_rebuild.cpp
git commit -m "feat(mvpq): rebuild drop-all-borrowing + had_pq snapshot + composition (token epic 4/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git status --short | grep -q business-strategy && echo "strategy doc still untracked OK" || echo "(strategy doc absent in worktree)"
```

---

## Notes for the implementer

- **Teardown-order independence is the whole point.** The dtor guard (`if (owns_codebook) delete []`) means a borrowing store frees nothing shared, so the engine may free `global_mvpq_codebook` before OR after the segment stores are destroyed. Do NOT add any dtor logic that dereferences `codebook`/`rotation` — only the conditional free.
- **`.mvpq` on-disk format is UNCHANGED.** The borrow only skips the RAM read; every `.mvpq` still embeds its codebook copy on disk (fseek-past, not truncate). A file written by this build loads identically in an old build.
- **Global mode off ⇒ `bcb == NULL` everywhere ⇒ owned path ⇒ byte-identical to today.** The `test_global_off_owns` case and the untouched `test_mvpq_store`/`test_mvpq_opq`/`test_v6_*` suites are the guard.
- **The rebuild `had_pq[]` snapshot MUST be captured before dropping stores to NULL** — otherwise Pass-2's eligibility check reads a NULL store pointer and silently skips re-encoding every borrowing segment (a silent no-op rebuild). This is the exact bug the dense Approach-A review caught; do not reintroduce it.
- **Grep before every edit** — T1/T2/T3 evolved these functions; the line numbers here are approximate. Confirm the current body, then splice.
```

# Single-Resident Global Codebook (Approach A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Under global-codebook mode, each segment's `ANT_pq_store` borrows the engine's single resident `global_pq_codebook` (+`global_pq_rotation` under OPQ) instead of owning a private copy — cutting resident codebook RAM from N+1 copies to 1.

**Architecture:** The `.pq` on-disk format is UNCHANGED (each file still embeds its codebook, staying self-describing and back-compatible). `ANT_pq_store::load` gains an optional borrowed codebook+rotation; when supplied and header-consistent it points `store->codebook`/`rotation` at the engine's resident buffers, skips reading the embedded copy into RAM, and records `owns_codebook = owns_rotation = 0` so the destructor never frees (or dereferences) them. A borrow is only ever taken from the engine-owned `global_pq_codebook`, whose lifetime spans all segment stores.

**Tech Stack:** C++ (C++03-style) ATIRE/antelope engine. Tests are standalone `tests/*.cpp` built via `make <name>` → `bin/<name>`, using the repo `CHECK()` convention.

**Repo gotchas (read before starting):**
- After ANY header edit (`source/pq_store.h`, `atire/atire_segment_index.h`): `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding (no header dep tracking → stale-object SEGV / `undefined reference`).
- Build a test: `make <name>` → `bin/<name>`; run `./bin/<name>`.
- Config setters POST-open. Confirm line numbers by grep (they drift).
- The 4-arg `ANT_pq_store::load(filename, dim, docs, metric)` has ~30 callers (all tests + 4 engine sites). Keep it working as a thin wrapper — do NOT change those callers.

---

### Task 1: Store borrow seam (`ANT_pq_store`)

Make a store able to borrow a caller-supplied codebook (+rotation) instead of loading the embedded copy — self-contained in `pq_store.{h,cpp}` plus a unit test.

**Files:**
- Modify: `source/pq_store.h` (owns flags, `codebook_is_borrowed()`, 5-arg `load` decl)
- Modify: `source/pq_store.cpp` (ctor init, dtor guard, load overload + 4-arg wrapper)
- Test: `tests/test_pq_borrow.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_pq_borrow.cpp`:

```cpp
/*
	TEST_PQ_BORROW.CPP -- Approach A store borrow seam (#22.2 RAM follow-up):
	a store loaded with a borrowed codebook points at the supplied buffer,
	does NOT own it, and reconstruct/scan match an owned-copy store byte for
	byte; a header-inconsistent borrow falls back to owning; the 4-arg load
	is unchanged.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void fill(float *v, long long n, unsigned seed)
{
	for (long long i = 0; i < n; i++) v[i] = (float)(((i * 131 + seed * 17) % 197) - 98) / 40.0f;
}

/* Build a non-OPQ .pq at (D,m,k=256) and return its path (caller removes). */
static void build_store(char *path, long long D, long long m, long long n, const float *vecs)
{
	strcpy(path, "/tmp/ant_borrow_XXXXXX");
	CHECK(mkstemp(path) >= 0);
	ANT_pq_store_writer w;
	CHECK(w.create(path, D, m, 256, ANT_pq_codec::METRIC_L2, 0) == 0);
	for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i * D) == 0);
	CHECK(w.finish() == 0);
}

static void test_borrow_points_at_supplied_and_matches(void)
{
	const long long D = 8, m = 4, n = 40;
	float *vecs = new float[n * D]; fill(vecs, n * D, 3);
	char path[64]; build_store(path, D, m, n, vecs);

	// Owned load (Approach B) gives us the reference codebook + reference scores.
	ANT_pq_store *owned = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2);
	CHECK(owned != NULL && owned->document_count() == n);
	CHECK(owned->codebook_is_borrowed() == 0);
	const float *ref_cb = owned->get_codebook();
	long long cb_floats = m * 256 * (D / m);
	float *shared_cb = new float[cb_floats];
	memcpy(shared_cb, ref_cb, (size_t)cb_floats * sizeof(float));

	// Borrowed load: same file, but hand it shared_cb (no OPQ -> rotation NULL).
	ANT_pq_store *borrowed = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2, shared_cb, NULL);
	CHECK(borrowed != NULL && borrowed->document_count() == n);
	CHECK(borrowed->codebook_is_borrowed() == 1);
	CHECK(borrowed->get_codebook() == shared_cb);			// points AT the supplied buffer

	// reconstruct must be byte-identical between owned and borrowed.
	float ro[8], rb[8];
	for (long long doc = 0; doc < n; doc++)
		{
		owned->reconstruct(doc, ro);
		borrowed->reconstruct(doc, rb);
		CHECK(memcmp(ro, rb, (size_t)D * sizeof(float)) == 0);
		}
	// score must match too.
	float q[8]; fill(q, D, 9);
	for (long long doc = 0; doc < n; doc++)
		CHECK(owned->score(doc, q, ANT_pq_codec::METRIC_L2) == borrowed->score(doc, q, ANT_pq_codec::METRIC_L2));

	delete borrowed;			// must NOT free shared_cb
	// shared_cb still valid here -> use it again to prove the dtor didn't free it.
	ANT_pq_store *again = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2, shared_cb, NULL);
	CHECK(again->codebook_is_borrowed() == 1);
	delete again;
	delete owned;
	delete [] shared_cb;		// we own it; freeing here is the ONLY free
	remove(path); delete [] vecs;
	printf("test_borrow_points_at_supplied_and_matches OK\n");
}

static void test_header_mismatch_falls_back_to_owning(void)
{
	const long long D = 8, m = 4, n = 20;
	float *vecs = new float[n * D]; fill(vecs, n * D, 5);
	char path[64]; build_store(path, D, m, n, vecs);
	// Supply a borrowed codebook but load with a WRONG expected dimension:
	// header validation fails the whole load (returns empty store, no borrow).
	float dummy[1] = { 0 };
	ANT_pq_store *s = ANT_pq_store::load(path, /*wrong D*/ 16, n, ANT_pq_codec::METRIC_L2, dummy, NULL);
	CHECK(s != NULL && s->document_count() == 0);		// degraded empty store (dim mismatch)
	delete s;
	// Correct load WITHOUT a borrow still owns its embedded copy.
	ANT_pq_store *own = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2);
	CHECK(own->codebook_is_borrowed() == 0);
	delete own;
	remove(path); delete [] vecs;
	printf("test_header_mismatch_falls_back_to_owning OK\n");
}

int main(void)
{
	test_borrow_points_at_supplied_and_matches();
	test_header_mismatch_falls_back_to_owning();
	printf("ALL test_pq_borrow PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
make test_pq_borrow 2>&1 | tail -15
```
Expected: FAIL — compile error (`codebook_is_borrowed()` undeclared; 6-arg `load` not declared).

- [ ] **Step 3: Header — owns flags, accessor, 5-arg load decl**

In `source/pq_store.h`:
1. Add private members after `float *rotation;`:
```cpp
	long owns_codebook;		// 1 = this store allocated codebook (free in dtor); 0 = borrowed (engine-owned, never freed/deref'd here)
	long owns_rotation;		// 1 = owned; 0 = borrowed
```
2. Add a public accessor near `get_codebook`:
```cpp
	long codebook_is_borrowed(void) { return owns_codebook == 0; }
```
3. Replace the `load` declaration with the 5-arg form plus keep a 4-arg convenience wrapper:
```cpp
	static ANT_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric,
		const float *borrowed_codebook, const float *borrowed_rotation);
	static ANT_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric)
		{ return load(filename, expected_dimension, expected_documents, metric, 0, 0); }
```

- [ ] **Step 4: Ctor init + dtor guard**

In `source/pq_store.cpp` constructor, after `rotation = NULL;`:
```cpp
owns_codebook = 1;
owns_rotation = 1;
```
Destructor — free codebook/rotation only when owned:
```cpp
ANT_pq_store::~ANT_pq_store()
{
delete [] presence;
if (owns_codebook)
	delete [] codebook;
delete [] codes;
if (owns_rotation)
	delete [] rotation;
}
```

- [ ] **Step 5: `load` overload — borrow decision, skip embedded read, fallback**

In `source/pq_store.cpp`, change the `load` definition signature to the 5-arg form:
```cpp
ANT_pq_store *ANT_pq_store::load(const char *filename, long long expected_dimension, long long expected_documents, long metric,
	const float *borrowed_codebook, const float *borrowed_rotation)
```
All header parsing and the full validation block (magic/version/dims/`stored_bits`/opq/exact-file-size) stay EXACTLY as-is — the borrow changes only how the codebook/rotation regions are consumed, never the validation. After the `fseek(fp, header_size, SEEK_SET)` that repositions to the start of the payload (i.e. where the current code allocates `presence_buffer` etc.), decide whether to borrow:

```cpp
/*
	Approach A (single-resident global codebook): if the caller supplied a
	borrowed codebook AND the file's opq flag is consistent with the supplied
	rotation (rotation present iff opq==1), point at the borrowed buffers and
	SKIP reading the embedded codebook/rotation into RAM (fseek past them).
	Otherwise own an embedded copy exactly as before.  Header dims/m/k were
	already validated above against expected_*; global mode guarantees the
	resident codebook is the one every .pq embeds, so no byte-compare is needed.
*/
int borrow = (borrowed_codebook != NULL) && ((stored_opq == 1) == (borrowed_rotation != NULL));

unsigned char *presence_buffer = new unsigned char[presence_bytes > 0 ? presence_bytes : 1];
float *codebook_buffer = borrow ? NULL : new float[codebook_floats > 0 ? codebook_floats : 1];
unsigned char *codes_buffer = new unsigned char[codes_bytes > 0 ? codes_bytes : 1];
float *rotation_buffer = (borrow || rotation_floats == 0) ? NULL : new float[rotation_floats];

/* presence, then codebook (read OR skip), then codes, then rotation (read OR skip) -- disk order */
int failed = 0;
if (presence_bytes > 0 && fread(presence_buffer, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
	failed = 1;
if (!failed && codebook_floats > 0)
	{
	if (borrow)
		{ if (fseek(fp, (long)(codebook_floats * (long long)sizeof(float)), SEEK_CUR) != 0) failed = 1; }
	else if (fread(codebook_buffer, sizeof(float), (size_t)codebook_floats, fp) != (size_t)codebook_floats)
		failed = 1;
	}
if (!failed && codes_bytes > 0 && fread(codes_buffer, 1, (size_t)codes_bytes, fp) != (size_t)codes_bytes)
	failed = 1;
if (!failed && rotation_floats > 0)
	{
	if (borrow)
		{ if (fseek(fp, (long)(rotation_floats * (long long)sizeof(float)), SEEK_CUR) != 0) failed = 1; }
	else if (fread(rotation_buffer, sizeof(float), (size_t)rotation_floats, fp) != (size_t)rotation_floats)
		failed = 1;
	}
if (failed)
	{
	delete [] presence_buffer;
	delete [] codebook_buffer;
	delete [] codes_buffer;
	delete [] rotation_buffer;
	fclose(fp);
	return result;
	}

fclose(fp);
```

Replace the existing "allocate four buffers + one combined `fread` `if`" block with the block above. Then set the result fields; the codebook/rotation come from the borrow decision:

```cpp
result->dimension = stored_dimension;
result->documents = stored_documents;
result->m = stored_m;
result->k = stored_k;
result->bits = stored_bits;
result->row_bytes = stored_row_bytes;
result->metric = metric;
result->presence = presence_buffer;
result->codes = codes_buffer;
if (borrow)
	{
	result->codebook = (float *)borrowed_codebook;   result->owns_codebook = 0;
	result->rotation = (stored_opq == 1) ? (float *)borrowed_rotation : NULL;
	result->owns_rotation = 0;
	}
else
	{
	result->codebook = codebook_buffer;   result->owns_codebook = 1;
	result->rotation = rotation_buffer;   result->owns_rotation = 1;   // NULL when opq==0
	}
return result;
```
(Keep the exact variable names already present — `presence_bytes`, `codebook_floats`, `codes_bytes`, `rotation_floats`, `stored_*`. `stored_row_bytes` is the Task-2-of-#22.3 local; confirm its name by reading the current function.)

- [ ] **Step 6: Run the test to verify it passes**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_borrow 2>&1 | tail -5
./bin/test_pq_borrow
```
Expected: `ALL test_pq_borrow PASSED`.

- [ ] **Step 7: Verify the whole PQ + engine suite is unchanged (4-arg wrapper + default owning)**

```bash
for t in test_pq_borrow test_pq_store test_pq_config test_pq_compaction test_pq_opq test_pq_global test_pq_metrics test_pq_resident_tier test_pq_hnsw test_pq_hnsw_tiered test_pq_hnsw_prepared test_pq_kwidth test_pq_kwidth_compose test_pq_codec_kwidth test_pq_load_hardening test_segment_index; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: every suite PASSED (the 4-arg wrapper preserves all existing behavior; every store still owns its codebook because no caller passes a borrow yet).

- [ ] **Step 8: Commit**

```bash
git add source/pq_store.h source/pq_store.cpp tests/test_pq_borrow.cpp
git commit -m "feat(pq): store borrow seam for single-resident global codebook (Approach A)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Engine wiring — borrow at every segment load site under global mode

Route the resident `global_pq_codebook` into the segment stores when global mode is on, via one shared helper, and prove the RAM dedup + result-equivalence.

**Files:**
- Modify: `atire/atire_segment_index.h` (helper decl + two introspection getters)
- Modify: `atire/atire_segment_index.cpp` (helper def; open-path load site ~1570)
- Modify: `atire/atire_segment_index_vector.cpp` (load sites ~1083, ~1745)
- Modify: `atire/atire_segment_index_compaction.cpp` (load site ~534)
- Test: `tests/test_pq_single_resident.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_pq_single_resident.cpp` (engine harness mirrors `tests/test_pq_global.cpp` — `set_vector_config`/`open`/`set_pq_config`/`set_pq_global_codebook`/`add_document`/`flush`/`build_pq`; GDIM=16, m=4). It asserts the RAM dedup via the new introspection getters and result-equivalence to Approach B:

```cpp
/*
	TEST_PQ_SINGLE_RESIDENT.CPP -- Approach A engine wiring: under global mode
	every resident segment store BORROWS the one engine codebook (same pointer,
	not N copies), search results are unchanged, and default (non-global) mode
	still owns a per-segment copy.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define GDIM 16
#define GM 4

static char *make_dir(const char *tmpl)
{ char b[64]; strcpy(b, tmpl); char *d = mkdtemp(b); if (!d) exit(printf("mkdtemp\n"));
  char *r = new char[strlen(d)+1]; strcpy(r, d); return r; }
static void gvec(long long i, float *v)
{ for (int d=0; d<GDIM; d++) v[d]=0.02f*(float)(((i*7+d)%5)-2); v[i%GDIM]+=3.0f; }
static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{ float v[GDIM]; char k[32], b[64];
  for (long long i=lo;i<hi;i++){ gvec(i,v); sprintf(k,"d-%lld",i); sprintf(b,"<DOC>t%lld z</DOC>",i);
    CHECK(ix->add_document(k,b,v)>=0);} }

/* global mode: all resident segment stores share ONE codebook pointer (the engine's). */
static void test_all_segments_borrow_one_codebook(void)
{
	char *dir = make_dir("/tmp/ant_sr_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);
	for (int s = 0; s < 3; s++) { add_docs(ix, s*10, s*10+10); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0); }
	CHECK(ix->disk_segment_count() == 3);

	const float *resident = ix->resident_pq_codebook();
	CHECK(resident != NULL);
	for (long i = 0; i < ix->disk_segment_count(); i++)
		{
		CHECK(ix->disk_segment_pq_codebook(i) == resident);		// borrows the SAME buffer
		CHECK(ix->disk_segment_pq_borrowed(i) == 1);
		}
	float q[GDIM]; gvec(5, q);
	CHECK(ix->search_vector(q, 5) >= 1);						// borrowed codebook search works
	delete ix; delete [] dir;
	printf("test_all_segments_borrow_one_codebook OK\n");
}

/* default (non-global) mode: each segment owns its own codebook (distinct pointers). */
static void test_non_global_owns_per_segment(void)
{
	char *dir = make_dir("/tmp/ant_sr2_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	// NO set_pq_global_codebook
	for (int s = 0; s < 2; s++) { add_docs(ix, s*10, s*10+10); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0); }
	CHECK(ix->resident_pq_codebook() == NULL);					// no global codebook resident
	CHECK(ix->disk_segment_pq_borrowed(0) == 0);				// owns its embedded copy
	CHECK(ix->disk_segment_pq_borrowed(1) == 0);
	CHECK(ix->disk_segment_pq_codebook(0) != ix->disk_segment_pq_codebook(1));	// distinct buffers
	delete ix; delete [] dir;
	printf("test_non_global_owns_per_segment OK\n");
}

int main(void)
{
	test_all_segments_borrow_one_codebook();
	test_non_global_owns_per_segment();
	printf("ALL test_pq_single_resident PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
make test_pq_single_resident 2>&1 | tail -15
```
Expected: FAIL to compile (`resident_pq_codebook`/`disk_segment_pq_codebook`/`disk_segment_pq_borrowed` undeclared), and — once those are added but before wiring — the pointer-equality assertion fails (stores still own private copies).

- [ ] **Step 3: Header — the shared load helper + introspection getters**

In `atire/atire_segment_index.h`, in the private section (near the other PQ helpers) declare:
```cpp
	ANT_pq_store *load_segment_pq_vectors(const char *filename, long long docs);	// borrow global_pq_codebook under global mode, else own
```
And in the public section (near the `disk_segment_*` introspection getters used by tests):
```cpp
	const float *resident_pq_codebook(void) { return global_pq_codebook; }			// engine's single resident codebook (NULL if none)
	const float *disk_segment_pq_codebook(long idx);								// idx-th resident segment store's codebook ptr (NULL if none)
	long disk_segment_pq_borrowed(long idx);										// 1 iff that store borrowed the resident codebook
```

- [ ] **Step 4: Implement the helper + introspection getters**

In `atire/atire_segment_index.cpp` (place near the open path). The helper centralizes "borrow under global mode, else own":
```cpp
/*
	Load a segment's dense .pq.  Under global-codebook mode with a resident
	global_pq_codebook, the store BORROWS it (single resident copy, Approach A);
	otherwise the store owns its embedded copy (Approach B / per-segment).
*/
ANT_pq_store *ATIRE_segment_index::load_segment_pq_vectors(const char *filename, long long docs)
{
if (pq_global_current && global_pq_codebook != NULL)
	return ANT_pq_store::load(filename, vector_dimension_current, docs, vector_metric, global_pq_codebook, global_pq_rotation);
return ANT_pq_store::load(filename, vector_dimension_current, docs, vector_metric);
}

const float *ATIRE_segment_index::disk_segment_pq_codebook(long idx)
{
if (idx < 0 || idx >= segment_count || segments[idx].pq_vectors == NULL)
	return NULL;
return segments[idx].pq_vectors->get_codebook();
}

long ATIRE_segment_index::disk_segment_pq_borrowed(long idx)
{
if (idx < 0 || idx >= segment_count || segments[idx].pq_vectors == NULL)
	return 0;
return segments[idx].pq_vectors->codebook_is_borrowed();
}
```
(Confirm the segment-array field names `segment_count` / `segments[idx].pq_vectors` and the introspection getters' indexing convention by reading the existing `disk_segment_has_pq`/`disk_segment_generation` — match whatever they use, e.g. a disk-only index mapping.)

- [ ] **Step 5: Route the four load sites through the helper**

Replace each 4-arg `ANT_pq_store::load(pq_..., vector_dimension_current, docs, vector_metric)` with the helper:
- `atire/atire_segment_index.cpp:1570` (open path):
```cpp
ANT_pq_store *pq = load_segment_pq_vectors(pq_filename, docs);
```
- `atire/atire_segment_index_vector.cpp:1083` and `:1745`:
```cpp
segments[which].pq_vectors = load_segment_pq_vectors(pq_name, docs);
```
- `atire/atire_segment_index_compaction.cpp:534`:
```cpp
output_segment->pq_vectors = load_segment_pq_vectors(out_pq, output_segment->engine->get_document_count());
```
(Grep-confirm each call's exact `docs` expression; keep it. These are the ONLY dense-`.pq` load sites — the `.mvpq` token loads use a different class and are out of scope.)

- [ ] **Step 6: Run the test + full regression**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_single_resident 2>&1 | tail -5
./bin/test_pq_single_resident
for t in test_pq_borrow test_pq_store test_pq_compaction test_pq_opq test_pq_global test_pq_resident_tier test_pq_hnsw test_pq_kwidth test_pq_kwidth_compose test_segment_index test_v6_compaction; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: `ALL test_pq_single_resident PASSED` and every existing suite PASSED (global tests still pass because the borrow yields identical codebook values; non-global unchanged).

- [ ] **Step 7: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp atire/atire_segment_index_compaction.cpp tests/test_pq_single_resident.cpp
git commit -m "feat(pq): borrow resident global codebook at all segment load sites (Approach A)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `rebuild_pq_global_codebook` re-borrow + teardown safety

`rebuild` reallocates the resident codebook; ensure every segment store re-borrows the NEW buffer, and prove teardown is safe.

**Files:**
- Modify: `atire/atire_segment_index_vector.cpp` (`rebuild_pq_global_codebook` ordering)
- Test: `tests/test_pq_single_resident_rebuild.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_pq_single_resident_rebuild.cpp` (same GDIM=16/m=4 harness as Task 2 — replicate the `make_dir`/`gvec`/`add_docs` helpers):

```cpp
/*
	TEST_PQ_SINGLE_RESIDENT_REBUILD.CPP -- Approach A: after
	rebuild_pq_global_codebook() reallocates the resident codebook, every
	segment store re-borrows the NEW pointer (no dangling old one), search
	stays correct, and engine teardown frees exactly once (no double-free).
*/
// ... includes + CHECK + make_dir/gvec/add_docs identical to test_pq_single_resident.cpp ...

static void test_reborrow_after_rebuild(void)
{
	char *dir = make_dir("/tmp/ant_srr_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);
	for (int s = 0; s < 3; s++) { add_docs(ix, s*10, s*10+10); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0); }

	const float *before = ix->resident_pq_codebook();
	CHECK(ix->rebuild_pq_global_codebook() == 0);
	const float *after = ix->resident_pq_codebook();
	CHECK(after != NULL);
	// every segment now borrows the CURRENT resident pointer (whatever it is post-rebuild).
	for (long i = 0; i < ix->disk_segment_count(); i++)
		{
		CHECK(ix->disk_segment_pq_borrowed(i) == 1);
		CHECK(ix->disk_segment_pq_codebook(i) == after);	// re-borrowed the new buffer, not a dangling `before`
		}
	float q[GDIM]; gvec(7, q);
	CHECK(ix->search_vector(q, 5) >= 1);					// search correct through the new borrowed codebook
	(void)before;
	delete ix;												// teardown: resident freed once; borrowing stores don't free/deref it
	delete [] dir;
	printf("test_reborrow_after_rebuild OK\n");
}

int main(void)
{
	test_reborrow_after_rebuild();
	printf("ALL test_pq_single_resident_rebuild PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails or passes**

```bash
make test_pq_single_resident_rebuild 2>&1 | tail -8
./bin/test_pq_single_resident_rebuild 2>&1 | tail -3
```
Expected: it FAILS (the `disk_segment_pq_borrowed(i)==1` / `== after` assertion) IF `rebuild`'s per-segment refresh reloads the store BEFORE the new resident codebook pointers are installed, or if it reloads via the 4-arg (owning) path. If it already passes, the ordering is coincidentally correct — still make the ordering explicit in Step 3 and add the assertion comment so it cannot silently regress.

- [ ] **Step 3: Make rebuild install the new resident codebook before re-borrowing refreshes**

Read `rebuild_pq_global_codebook` in `atire/atire_segment_index_vector.cpp`. It (per #22.2/#22.3) gathers rows → frees old `global_pq_codebook`/`global_pq_rotation` → trains new ones into the resident members → `save_pq_codebook()` → re-encodes each segment's `.pq` and refreshes `segments[which].pq_vectors`. Two requirements:
1. The new `global_pq_codebook`/`global_pq_rotation` must be assigned to the engine members **before** the per-segment refresh loop (so the refresh borrows the new buffers). Confirm the existing order already does this (train writes directly into `global_pq_codebook`); if the refresh loop runs after, no change beyond wiring is needed.
2. The per-segment refresh must load through `load_segment_pq_vectors(...)` (Task 2 helper) — NOT a raw 4-arg `ANT_pq_store::load` — so it borrows. Grep the refresh site inside `rebuild`; if it still calls `ANT_pq_store::load(...)` directly, change it to `load_segment_pq_vectors(pq_name, docs)`.

Add a brief comment at the refresh site: `/* Approach A: reload borrows the freshly-installed resident global_pq_codebook (must run AFTER it is assigned). */`

- [ ] **Step 4: Run the test to verify it passes**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_single_resident_rebuild 2>&1 | tail -5
./bin/test_pq_single_resident_rebuild
```
Expected: `ALL test_pq_single_resident_rebuild PASSED`.

- [ ] **Step 5: Full regression**

```bash
for t in test_pq_borrow test_pq_single_resident test_pq_single_resident_rebuild test_pq_store test_pq_compaction test_pq_opq test_pq_global test_pq_resident_tier test_pq_hnsw test_pq_hnsw_tiered test_pq_hnsw_prepared test_pq_kwidth test_pq_kwidth_compose test_pq_codec_kwidth test_pq_load_hardening test_segment_index test_v6_compaction; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: every suite PASSED.

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_pq_single_resident_rebuild.cpp
git commit -m "feat(pq): rebuild re-borrows the reallocated resident codebook; teardown safe (Approach A)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Notes for the final holistic review (after all 3 tasks)

- **Borrow correctness:** a borrowed store's `codebook`/`rotation` point at the engine buffers; `owns_*==0`; dtor neither frees nor dereferences them. `reconstruct`/`score`/`score_prepared`/`scan_adc` produce identical values to an owned-copy store.
- **Skip-embedded read:** the `fseek` past the codebook (and rotation) region lands exactly at the codes (and EOF) — the exact-file-size validation still runs against the on-disk bytes; no over/under-read; borrow disabled when `borrowed_codebook==NULL` or opq/rotation-nullness disagree.
- **Lifetime:** teardown-order-independent (dtors never deref borrowed buffers); the ONLY resident-codebook reallocation is `rebuild`, which re-borrows via the refreshed stores; no dangling pointer survives a rebuild.
- **Default byte-identity:** non-global mode takes the owning 4-arg path everywhere; all existing PQ/engine suites unchanged.
- **Compose:** OPQ (borrow rotation), variable-k (`m·k·sub` borrowed codebook) both correct.
- ASan/UBSan environment-blocked (no makefile hook) — report, don't attempt.
```

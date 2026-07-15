# Token `.mvpq` OPQ Rotation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Opt-in OPQ rotation for the token `.mvpq` pool — a learned orthogonal `D×D` R applied before the subspace split, improving MaxSim recall at the same m/k, metric-exactly. Default off ⇒ byte-identical.

**Architecture:** Reuse the shipped, k-independent `ANT_pq_codec::train_rotation`/`apply_rotation`/`apply_rotation_transpose` UNCHANGED (R orthogonal ⇒ dot preserved ⇒ ADC scores metric-exact in rotated space, no un-rotation for scoring; only `token_reconstruct` un-rotates via Rᵀ). Add an R block to the `.mvpq` sidecar (v2), rotate the flattened pool + train in rotated space at write time, rotate the query before every ADC table build, and expose `set_multivector_pq_opq`. This mirrors the dense `.pq` OPQ (#22.1) for the ragged token store.

**Tech Stack:** C++ (C++03-style) ATIRE/antelope engine. Tests are standalone `tests/*.cpp` built via `make <name>` → `bin/<name>`, using the repo `CHECK()` convention.

**Repo gotchas (read before starting):**
- After ANY header edit (`source/multivector_pq_store.h`, `atire/atire_segment_index.h`): `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding (no header dep tracking → stale-object SEGV / `undefined reference`).
- Build a test: `make <name>` → `bin/<name>`; run `./bin/<name>`.
- Config setters POST-open. Confirm line numbers by grep (they drift).
- The codec is SHARED and already has OPQ helpers — do NOT modify `source/pq_codec.{h,cpp}`.

**`.mvpq` on-disk layout (current v1, 52-byte header):** magic `ANTMVPQ1`(8) · version u32(4) · dim i64 · docs i64 · total_tokens i64 · m i64 · k i64 · then `counts`(docs×int) · `codes`(total_tokens×m bytes) · `codebook`(256×dim floats). **v2 adds:** an `opq` i64 at header offset 52 (header → 60 bytes) and, when opq==1, a trailing `dim×dim` float R block AFTER the codebook.

---

### Task 1: Store `.mvpq` v2 + query rotation + writer `train_rotation`

Self-contained in `multivector_pq_store.{h,cpp}` plus a unit test.

**Files:**
- Modify: `source/multivector_pq_store.h` (rotation member, `create(...,opq)` decl)
- Modify: `source/multivector_pq_store.cpp` (dtor, load v2, 3 ADC sites, reconstruct, writer)
- Test: `tests/test_mvpq_opq.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_mvpq_opq.cpp`:

```cpp
/*
	TEST_MVPQ_OPQ.CPP -- token .mvpq OPQ rotation (token epic 1/4): a store
	written with OPQ persists v2 + R, reloads with rotation restored, its
	maxsim/token_score match a direct rotated-space ADC, token_reconstruct
	un-rotates, a v1 (no-opq) file still loads, and the non-OPQ path is
	byte-identical.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

/* anisotropic tokens: variance concentrated on a few axes so the rotation matters */
static void fill_tokens(float *v, long long n, long long D, unsigned seed)
{
	srand(seed);
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			{
			double scale = (d < D/2) ? 4.0 : 0.25;			// anisotropy
			v[i*D + d] = (float)(((rand() % 2000) - 1000) / 1000.0 * scale);
			}
}

/* write a store: 3 docs with [n0,n1,n2] tokens drawn from `pool` (total = n0+n1+n2). */
static void write_store(const char *path, long long D, long long m, long opq,
	const float *pool, const int *counts, long ndocs)
{
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, D, m, ANT_pq_codec::METRIC_DOT, opq) == 0);
	long long off = 0;
	for (long d = 0; d < ndocs; d++) { CHECK(w.append(pool + off*D, counts[d]) == 0); off += counts[d]; }
	CHECK(w.finish() == 0);
}

static unsigned int read_version(const char *path)
{
	FILE *f = fopen(path, "rb"); unsigned char h[12]; unsigned int v = 0;
	if (f) { if (fread(h, 1, 12, f) == 12) memcpy(&v, h+8, 4); fclose(f); }
	return v;
}

static void test_opq_v2_roundtrip_and_score(void)
{
	const long long D = 8, m = 4;
	int counts[3] = { 10, 6, 8 };
	long long total = 24;
	float *pool = new float[total * D];
	fill_tokens(pool, total, D, 123);

	write_store("/tmp/mvpq_opq.mvpq", D, m, /*opq*/ 1, pool, counts, 3);
	CHECK(read_version("/tmp/mvpq_opq.mvpq") == 2u);					// OPQ => v2

	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load("/tmp/mvpq_opq.mvpq", D, 3, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->document_count() == 3 && s->token_count() == total);

	// token_reconstruct returns finite original-space vectors (un-rotated via R^T).
	float recon[8];
	s->token_reconstruct(0, recon);
	for (long long d = 0; d < D; d++) CHECK(recon[d] == recon[d]);		// not NaN

	// maxsim with a query equal to doc 0's first token should score high (self-similar).
	double sim = s->maxsim(0, pool + 0*D, 1);
	CHECK(sim > 0.0);
	delete s;
	delete [] pool;
	printf("test_opq_v2_roundtrip_and_score OK\n");
}

static void test_non_opq_is_v1_and_loads(void)
{
	const long long D = 8, m = 4;
	int counts[3] = { 5, 5, 5 };
	float *pool = new float[15 * D];
	fill_tokens(pool, 15, D, 7);
	write_store("/tmp/mvpq_noopq.mvpq", D, m, /*opq*/ 0, pool, counts, 3);
	CHECK(read_version("/tmp/mvpq_noopq.mvpq") == 1u);					// non-OPQ stays v1 (byte-identical default)
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load("/tmp/mvpq_noopq.mvpq", D, 3, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == 15);
	delete s;
	delete [] pool;
	printf("test_non_opq_is_v1_and_loads OK\n");
}

static void test_empty_pool_graceful(void)
{
	const long long D = 8, m = 4;
	int counts[2] = { 0, 0 };
	write_store("/tmp/mvpq_empty.mvpq", D, m, /*opq*/ 1, NULL, counts, 2);
	CHECK(read_version("/tmp/mvpq_empty.mvpq") == 1u);					// empty pool -> opq=0 -> v1 (no R)
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load("/tmp/mvpq_empty.mvpq", D, 2, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == 0);
	delete s;
	printf("test_empty_pool_graceful OK\n");
}

int main(void)
{
	test_opq_v2_roundtrip_and_score();
	test_non_opq_is_v1_and_loads();
	test_empty_pool_graceful();
	printf("ALL test_mvpq_opq PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
make test_mvpq_opq 2>&1 | tail -15
```
Expected: FAIL to compile — `w.create(...)` called with a 5th `opq` arg the current 4-arg `create` rejects.

- [ ] **Step 3: Header — rotation member + `create` arity**

In `source/multivector_pq_store.h`:
1. Add a private member after `unsigned char *codes;`:
```cpp
	float *rotation;		// D*D OPQ rotation R (row-major), NULL when OPQ off
```
2. Change the writer's `create` decl:
```cpp
	long create(const char *path, long long dim, long long m, long metric, long opq);
```
3. Add `long opq;` to the writer's private members (alongside `long metric;`).

- [ ] **Step 4: Store `.cpp` — ctor/dtor, load v2, query rotation, reconstruct**

In `source/multivector_pq_store.cpp`:

1. Ctor init list — add `rotation(0)`:
```cpp
ANT_multivector_pq_store::ANT_multivector_pq_store() :
	dimension(0), documents(0), total_tokens(0), m(0), metric(0),
	counts(0), offsets(0), codebook(0), codes(0), rotation(0), adc_table_builds(0) {}
```
2. Dtor — free rotation:
```cpp
delete [] counts; delete [] offsets; delete [] codebook; delete [] codes; delete [] rotation;
```
3. `token_reconstruct` — un-rotate when OPQ:
```cpp
void ANT_multivector_pq_store::token_reconstruct(long long t, float *out)
{
if (!token_has(t)) { memset(out, 0, (size_t)(dimension*sizeof(float))); return; }
if (rotation != NULL)
	{
	float *tmp = new float[dimension];
	ANT_pq_codec::reconstruct(codes + t*m, dimension, m, ANT_pq_codec::K, codebook, tmp);
	ANT_pq_codec::apply_rotation_transpose(tmp, dimension, rotation, out);
	delete [] tmp;
	}
else
	ANT_pq_codec::reconstruct(codes + t*m, dimension, m, ANT_pq_codec::K, codebook, out);
}
```
4. **Query rotation helper** — add a small file-static helper to DRY the three sites:
```cpp
/* rotate `query` into R-space if OPQ is on; returns a buffer the caller must delete[],
   or NULL when no rotation (caller then uses the original query). */
static float *rotate_query_or_null(const float *query, long long dimension, const float *rotation)
{
if (rotation == NULL) return NULL;
float *rq = new float[dimension];
ANT_pq_codec::apply_rotation(query, dimension, rotation, rq);
return rq;
}
```
5. `token_score` — rotate before the table build:
```cpp
double ANT_multivector_pq_store::token_score(long long t, const float *query, long)
{
if (!token_has(t)) return 0.0;
double *table = new double[(size_t)(m * ANT_pq_codec::K)];
float *rq = rotate_query_or_null(query, dimension, rotation);
ANT_pq_codec::adc_table(rq ? rq : query, dimension, m, ANT_pq_codec::K, codebook, metric, table);
delete [] rq;
adc_table_builds++;
double s = ANT_pq_codec::adc_score(codes + t*m, m, ANT_pq_codec::K, table);
delete [] table;
return s;
}
```
6. `token_prepare_query` — same rotate-before-build:
```cpp
void *ANT_multivector_pq_store::token_prepare_query(const float *query)
{
if (total_tokens == 0 || codebook == 0)
	return 0;
double *table = new double[(size_t)(m * ANT_pq_codec::K)];
float *rq = rotate_query_or_null(query, dimension, rotation);
ANT_pq_codec::adc_table(rq ? rq : query, dimension, m, ANT_pq_codec::K, codebook, metric, table);
delete [] rq;
adc_table_builds++;
return table;
}
```
7. `maxsim` — rotate EACH query token before its table:
```cpp
double ANT_multivector_pq_store::maxsim(long long docid, const float *query_vecs, long long num_query_vecs)
{
if (!has(docid) || num_query_vecs < 1) return 0.0;
double *tables = new double[(size_t)(num_query_vecs * m * ANT_pq_codec::K)];
for (long long q = 0; q < num_query_vecs; q++)
	{
	float *rq = rotate_query_or_null(query_vecs + q*dimension, dimension, rotation);
	ANT_pq_codec::adc_table(rq ? rq : (query_vecs + q*dimension), dimension, m, ANT_pq_codec::K, codebook, metric, tables + q*(m*ANT_pq_codec::K));
	delete [] rq;
	}
long long begin = offsets[docid], end = offsets[docid] + counts[docid];
double total = 0.0;
for (long long q = 0; q < num_query_vecs; q++)
	{
	const double *table = tables + q*(m*ANT_pq_codec::K);
	double best = 0.0; int seen = 0;
	for (long long t = begin; t < end; t++)
		{ double s = ANT_pq_codec::adc_score(codes + t*m, m, ANT_pq_codec::K, table); if (!seen || s > best) { best = s; seen = 1; } }
	total += best;
	}
delete [] tables;
return total;
}
```
(`token_score_prepared` is UNCHANGED — the table it receives was already built on the rotated query by `token_prepare_query`.)

- [ ] **Step 5: Store `load()` — read v2 opq + R block**

Rewrite the header-parse + sizing portion of `load` to accept v1 and v2. Replace from the `unsigned char hdr[52];` block through the `expected_size` check with:

```cpp
unsigned char hdr[52];
if (fread(hdr, 1, 52, in) != 52) { fclose(in); return s; }
if (memcmp(hdr, "ANTMVPQ1", 8) != 0) { fclose(in); return s; }
unsigned int version; memcpy(&version, hdr+8, 4);
if (version != 1 && version != 2) { fclose(in); return s; }
long long dim = read_i64(hdr+12), docs = read_i64(hdr+20), toks = read_i64(hdr+28), mm = read_i64(hdr+36), kk = read_i64(hdr+44);
if (dim != expected_dimension || docs != expected_documents || kk != ANT_pq_codec::K) { fclose(in); return s; }
if (mm < 1 || dim < 1 || mm > dim || dim % mm != 0 || docs < 0 || toks < 0) { fclose(in); return s; }

long long opq = 0, header_size = 52;
if (version == 2)
	{
	unsigned char ohdr[8];
	if (fread(ohdr, 1, 8, in) != 8) { fclose(in); return s; }
	opq = read_i64(ohdr);
	if (opq != 0 && opq != 1) { fclose(in); return s; }
	header_size = 60;
	}

if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return s; }
long long actual = ftell(in);
/* toks is the only header count without an upper bound; cap it against the real file size so
   toks*mm below cannot signed-overflow (docs/dim are pinned to expected_* above). */
if (actual < header_size || (mm > 0 && toks > (actual - header_size) / mm)) { fclose(in); return s; }
long long rot_floats = opq ? dim*dim : 0;					/* dim<=... : dim*dim*4 <= 2^34, no overflow (dim pinned to expected) */
long long expected_size = header_size + docs*4 + toks*mm + 256*dim*4 + rot_floats*4;
if (actual != expected_size) { fclose(in); return s; }
if (fseek(in, header_size, SEEK_SET) != 0) { fclose(in); return s; }
```

Then the allocation/read block: add a rotation buffer read AFTER the codebook (disk order counts → codes → codebook → rotation):
```cpp
int *counts = new int[docs > 0 ? docs : 1];
long long *offsets = new long long[docs + 1];
unsigned char *codes = new unsigned char[toks*mm > 0 ? toks*mm : 1];
long long cb_floats = 256*dim;
float *codebook = new float[cb_floats > 0 ? cb_floats : 1];
float *rotation = rot_floats > 0 ? new float[rot_floats] : NULL;

long ok = 1;
if (docs > 0 && fread(counts, 4, (size_t)docs, in) != (size_t)docs) ok = 0;
if (ok && toks > 0 && fread(codes, 1, (size_t)(toks*mm), in) != (size_t)(toks*mm)) ok = 0;
if (ok && fread(codebook, sizeof(float), (size_t)cb_floats, in) != (size_t)cb_floats) ok = 0;
if (ok && rot_floats > 0 && fread(rotation, sizeof(float), (size_t)rot_floats, in) != (size_t)rot_floats) ok = 0;
fclose(in);
```
Update the offsets-build failure cleanup and the success assignment to include `rotation`:
```cpp
if (!ok) { delete [] counts; delete [] offsets; delete [] codes; delete [] codebook; delete [] rotation; return s; }

s->dimension = dim; s->documents = docs; s->total_tokens = toks; s->m = mm;
s->counts = counts; s->offsets = offsets; s->codes = codes; s->codebook = codebook; s->rotation = rotation;
return s;
```

- [ ] **Step 6: Writer `create` + `finish` — train R, rotate pool, write v2**

`create` — store the opq flag:
```cpp
long ANT_multivector_pq_store_writer::create(const char *path, long long dim, long long mm, long met, long op)
{
if (dim < 1 || mm < 1 || mm > dim || dim % mm != 0) return 1;
abandon();
filename = new char[strlen(path)+1]; strcpy(filename, path);
dimension = dim; m = mm; metric = met; opq = op ? 1 : 0;
capacity = 1024; buffer = new float[capacity * dimension];
counts_capacity = 256; counts = new int[counts_capacity];
total_tokens = 0; documents = 0;
return 0;
}
```
Add `opq = 0;` to the writer ctor init list.

`finish` — train R (when opq && tokens), rotate the pool in place, train + encode in rotated space, write v2 + R:
```cpp
long ANT_multivector_pq_store_writer::finish(void)
{
if (filename == NULL) return 1;
long long cb_floats = 256*dimension;

/* OPQ: learn R over the whole flattened token pool, then rotate every token in place so the
   codebook trains on -- and every code encodes -- R*x.  Empty pool -> no R (opq_flag=0). */
float *rotation = NULL;
if (opq && total_tokens > 0)
	{
	rotation = new float[dimension * dimension];
	if (ANT_pq_codec::train_rotation(buffer, dimension, m, total_tokens, rotation) != 0)
		{ delete [] rotation; rotation = NULL; }
	else
		{
		float *tmp = new float[dimension];
		for (long long t = 0; t < total_tokens; t++)
			{
			ANT_pq_codec::apply_rotation(buffer + t*dimension, dimension, rotation, tmp);
			memcpy(buffer + t*dimension, tmp, (size_t)(dimension*sizeof(float)));
			}
		delete [] tmp;
		}
	}

float *codebook = new float[cb_floats];
if (ANT_pq_codec::train(buffer, dimension, m, ANT_pq_codec::K, total_tokens, codebook) != 0)
	{ delete [] codebook; delete [] rotation; return 1; }

unsigned char *codes = new unsigned char[total_tokens*m > 0 ? total_tokens*m : 1];
for (long long t = 0; t < total_tokens; t++)
	ANT_pq_codec::encode(buffer + t*dimension, dimension, m, ANT_pq_codec::K, codebook, codes + t*m);

char *tmp = new char[strlen(filename)+5]; strcpy(tmp, filename); strcat(tmp, ".tmp");
FILE *out = fopen(tmp, "wb");
long ok = out != NULL;
if (ok)
	{
	long long opq_flag = (rotation != NULL) ? 1 : 0;			/* derive from trained R, not the request */
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
delete [] tmp; delete [] codes; delete [] codebook; delete [] rotation;
if (ok) abandon();
return ok ? 0 : 1;
}
```

- [ ] **Step 7: Run the test + existing token suites**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_opq 2>&1 | tail -5
./bin/test_mvpq_opq
for t in test_mvpq_store test_pq_token_resident_tier test_v6_token_index test_v6_search_multivector test_v6_compaction; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: `ALL test_mvpq_opq PASSED` and every existing token suite PASSED (non-OPQ path writes v1, byte-identical).

- [ ] **Step 8: Commit**

```bash
git add source/multivector_pq_store.h source/multivector_pq_store.cpp tests/test_mvpq_opq.cpp
git commit -m "feat(mvpq): token .mvpq OPQ rotation — v2 store + query rotation + train_rotation (token epic 1/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Config `set_multivector_pq_opq` + `multivector_pq.config` v3 + writer wiring

**Files:**
- Modify: `atire/atire_segment_index.h` (`mvpq_opq_current` member, getter, `set_multivector_pq_opq` decl)
- Modify: `atire/atire_segment_index.cpp` (ctor init `mvpq_opq_current = 0`)
- Modify: `atire/atire_segment_index_vector.cpp` (config v3 load/save, `set_multivector_pq_opq`, writer create site ~1904)
- Modify: `atire/atire_segment_index_compaction.cpp` (writer create site ~667)
- Test: `tests/test_mvpq_opq_config.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_mvpq_opq_config.cpp` (engine harness mirrors `tests/test_pq_token_resident_tier.cpp` / `test_v6_*` — read one for the exact `set_vector_config`/`set_rerank_config`/`open`/`set_multivector_pq_config`/`add_document`/`flush` sequence; the token pool needs rerank/multivector configured). It asserts:

```cpp
/*
	TEST_MVPQ_OPQ_CONFIG.CPP -- set_multivector_pq_opq persists to
	multivector_pq.config v3, reopen restores it, immutability + idempotence
	hold, a v2 config (no opq) loads as 0, and build/compaction under OPQ work.
*/
```

Implement, following the token-engine harness of `tests/test_pq_token_resident_tier.cpp`:
- `test_opq_config_roundtrip`: open index with rerank + `set_multivector_pq_config(...)`, `set_multivector_pq_opq(1)` returns 0 and `multivector_pq_opq()==1`; add docs + flush + `build_multivector_pq()`; close; reopen; `multivector_pq_opq()==1` (persisted v3); a token search is sane.
- `test_opq_immutable_idempotent`: `set_multivector_pq_opq(1)` then `set_multivector_pq_opq(1)` again → 0 (idempotent); `set_multivector_pq_opq(0)` after enabled → nonzero (immutable). `set_multivector_pq_opq` before `set_multivector_pq_config` → nonzero (requires token-PQ configured).

Use the exact rerank/multivector setup calls from `test_pq_token_resident_tier.cpp` (e.g. `set_rerank_config(...)`, `set_multivector_pq_config(m, PQ_POSTURE_REPLACE, RERANK_QUANT_FLOAT)`); replicate its `make_dir`/add/flush helpers rather than inventing new ones.

- [ ] **Step 2: Run test to verify it fails**

```bash
make test_mvpq_opq_config 2>&1 | tail -15
```
Expected: FAIL to compile (`set_multivector_pq_opq`/`multivector_pq_opq` undeclared).

- [ ] **Step 3: Header — member, getter, setter decl**

In `atire/atire_segment_index.h`, after `long mvpq_resident_tier_current;`:
```cpp
	long mvpq_opq_current;				// 0 (default, off) / 1 (OPQ rotation enabled); immutable once on
```
Near the other multivector getters:
```cpp
	long set_multivector_pq_opq(long enable);	// 0 ok; nonzero: not open / token-PQ unconfigured / already set to a DIFFERENT value (immutable)
	long multivector_pq_opq(void) { return mvpq_opq_current; }
```

- [ ] **Step 4: Ctor init**

In `atire/atire_segment_index.cpp` constructor, next to `mvpq_resident_tier_current` init:
```cpp
mvpq_opq_current = 0;
```

- [ ] **Step 5: `multivector_pq.config` v3 — load + save**

In `atire/atire_segment_index_vector.cpp`:

`load_multivector_pq_config` — accept v3 (5 vals incl. opq); v1/v2 default opq=0. Replace the version dispatch:
```cpp
char tag[8];
unsigned int version;
long long vals[5];
if (fread(tag, 1, 8, in) != 8 || memcmp(tag, "ANTMVPQC", 8) != 0 || fread(&version, 4, 1, in) != 1)
	{ fclose(in); return 1; }
long ok;
if (version == 1)
	{ ok = (fread(vals, 8, 3, in) == 3); vals[3] = MV_TIER_FLOAT; vals[4] = 0; }
else if (version == 2)
	{ ok = (fread(vals, 8, 4, in) == 4); vals[4] = 0; }
else if (version == 3)
	{ ok = (fread(vals, 8, 5, in) == 5); }
else
	ok = 0;
fclose(in);
if (!ok)
	return 1;

long long m = vals[0], posture = vals[1], rq = vals[2], tier = vals[3], opq = vals[4];
if (m < 1
	|| (posture != PQ_POSTURE_REPLACE && posture != PQ_POSTURE_RERANK)
	|| (rq != RERANK_QUANT_FLOAT && rq != RERANK_QUANT_INT8)
	|| (tier != MV_TIER_FLOAT && tier != MV_TIER_NONE)
	|| (opq != 0 && opq != 1)
	|| (rerank_dimension_current != 0 && rerank_dimension_current % m != 0))
	return 1;

mvpq_m_current = m;
mvpq_posture_current = (long)posture;
mvpq_rerank_quant_current = (long)rq;
mvpq_resident_tier_current = (long)tier;
mvpq_opq_current = (long)opq;
return 0;
```

`save_multivector_pq_config` — bump to v3, write 5 vals:
```cpp
unsigned int version = 3;
long long vals[5] = { mvpq_m_current, mvpq_posture_current, mvpq_rerank_quant_current, mvpq_resident_tier_current, mvpq_opq_current };
long ok = fwrite("ANTMVPQC", 1, 8, out) == 8 && fwrite(&version, 4, 1, out) == 1 && fwrite(vals, 8, 5, out) == 5;
```

- [ ] **Step 6: `set_multivector_pq_opq` (mirror `set_pq_opq`)**

In `atire/atire_segment_index_vector.cpp`, after `set_multivector_resident_tier`:
```cpp
/*
	ATIRE_SEGMENT_INDEX::SET_MULTIVECTOR_PQ_OPQ()
	----------------------------------------------
	Enable OPQ rotation for the token .mvpq store: a learned orthogonal D*D
	rotation applied before subspace splitting, improving MaxSim recall at the
	same m/k (metric-exactly).  Requires token-PQ configured
	(set_multivector_pq_config()).  Immutable once enabled: the SAME value is a
	no-op success (idempotent); flipping it back off (or any other change once
	on) is rejected.  Persists in multivector_pq.config v3.  Writer create()
	sites pass mvpq_opq_current so backfilled/compacted segments train R.
*/
long ATIRE_segment_index::set_multivector_pq_opq(long enable)
{
long want;
if (directory == NULL)
	return 1;
if (!multivector_pq_configured())
	return 1;
want = enable ? 1 : 0;
if (mvpq_opq_current == want)
	return 0;
if (mvpq_opq_current != 0)
	return 1;
mvpq_opq_current = want;
if (save_multivector_pq_config() != 0)
	{ mvpq_opq_current = 0; return 1; }
return 0;
}
```

- [ ] **Step 7: Wire the two mvpq writer `create` sites to pass `mvpq_opq_current`**

- `atire/atire_segment_index_vector.cpp:~1904` (`build_multivector_pq`):
```cpp
long failed = w.create(mvpq_name, rerank_dimension_current, mvpq_m_current, ANT_pq_codec::METRIC_DOT, mvpq_opq_current) != 0;
```
- `atire/atire_segment_index_compaction.cpp:~667`:
```cpp
long failed = w.create(out_mvpq, rerank_dimension_current, mvpq_m_current, ANT_pq_codec::METRIC_DOT, mvpq_opq_current) != 0;
```
(Confirm both by grep; the `.mvec` float writer at compaction:~303 is a DIFFERENT store — do NOT touch it.)

- [ ] **Step 8: Run the test + full token regression**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_opq_config 2>&1 | tail -5
./bin/test_mvpq_opq_config
for t in test_mvpq_opq test_mvpq_store test_pq_token_resident_tier test_v6_token_index test_v6_search_multivector test_v6_compaction test_segment_index; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: `ALL test_mvpq_opq_config PASSED` and every token/engine suite PASSED (default-off writes v1/config-v3-opq=0, byte-identical behavior).

- [ ] **Step 9: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp atire/atire_segment_index_compaction.cpp tests/test_mvpq_opq_config.cpp
git commit -m "feat(mvpq): set_multivector_pq_opq + multivector_pq.config v3 + writer wiring (token epic 1/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Composition (#24 token graph + tiers) + recall

Prove OPQ composes with the PQ-backed token graph and the resident tiers, and improves MaxSim recall on anisotropic data.

**Files:**
- Test: `tests/test_mvpq_opq_compose.cpp` (new)

- [ ] **Step 1: Write the test**

Create `tests/test_mvpq_opq_compose.cpp` (token-engine harness as in Task 2). Cover:
- `test_opq_recall_ge_non_opq`: build two indices on the SAME anisotropic multi-vector data (variance concentrated on half the axes) at the same `m` — one with `set_multivector_pq_opq(1)`, one without. For a set of held-out queries, OPQ MaxSim recall@k (vs exact float MaxSim over the resident `.mvec`) is ≥ the non-OPQ recall. Assert the direction (`recall_opq >= recall_noopq`), not a fixed number — mirror the #22.1 fixture discipline (independent per-component draws so the axis-aligned split isn't a free exact fit).
- `test_opq_composes_with_token_graph`: OPQ + `set_multivector_pq_config(..., PQ_POSTURE_REPLACE, ...)` (PQ-backed token graph path) — `searchRerank`/`search_multivector` returns sane top-k (planted nearest neighbor ranks #1).
- `test_opq_none_tier_reconstruct`: OPQ + `set_multivector_resident_tier(MV_TIER_NONE)` — the reconstruct-from-PQ path (un-rotating via Rᵀ) yields a sane search (no crash, planted NN found).

(Use the exact search/rerank API names from `test_v6_search_multivector.cpp` / `test_pq_token_resident_tier.cpp`; replicate their harness helpers.)

- [ ] **Step 2: Run + iterate**

```bash
make test_mvpq_opq_compose 2>&1 | tail -5
./bin/test_mvpq_opq_compose
```
Expected: `ALL test_mvpq_opq_compose PASSED`. If the recall fixture gives the axis-aligned split a free exact fit (recall_noopq already 1.0), diagnose via reconstruction MSE and rebuild the fixture with independent per-component draws (mirror the #22.1 fix) rather than weakening the assertion.

- [ ] **Step 3: Full regression + commit**

```bash
for t in test_mvpq_opq test_mvpq_opq_config test_mvpq_opq_compose test_mvpq_store test_pq_token_resident_tier test_v6_token_index test_v6_search_multivector test_v6_recall test_v6_compaction test_segment_index; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
git add tests/test_mvpq_opq_compose.cpp
git commit -m "test(mvpq): OPQ composition with #24 token graph + tiers + recall (token epic 1/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Notes for the final holistic review (after all 3 tasks)

- **Byte-identity default off:** no `set_multivector_pq_opq` ⇒ writer emits v1 (opq=0), config v3 with opq=0, all existing token suites unchanged.
- **`.mvpq` v2 correctness:** header offset 52 opq field; R block after codebook; exact-file-size check accounts for R before any `new`; v1 back-compat; forgiving-load degrades on a truncated/inconsistent R.
- **Query rotation at all 3 ADC sites** (`token_score`/`token_prepare_query`/`maxsim`) + `token_reconstruct` un-rotation; `rotate_query_or_null` never leaks (every `delete [] rq`).
- **Writer:** `train_rotation` over the flattened pool; empty-pool → opq=0/v1 graceful; `opq_flag` derived from the trained R pointer; rotation freed on all paths.
- **Compose:** #24 token graph scores through the rotated prepared table; NONE-tier reconstruct un-rotates.
- ASan/UBSan environment-blocked (no makefile hook) — report, don't attempt.
```

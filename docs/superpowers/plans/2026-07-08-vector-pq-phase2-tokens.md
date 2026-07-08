# PQ Phase 2 — Token-Pool Compression Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Product-quantization compress the V6 late-interaction token pool (`.mvec`) into a per-segment `.mvpq` sidecar, scored via ADC-MaxSim, with configurable replace/rerank postures — the token-level sibling of the shipped Phase-1 dense `.pq`.

**Architecture:** A new ragged, self-contained `ANT_multivector_pq_store` reuses the Phase-1 `ANT_pq_codec` unchanged (train one k=256 codebook per segment over the whole token pool, encode each token to `m` bytes). It exposes the same token-level accessors as `ANT_multivector_store` plus `maxsim()` = ADC-MaxSim. In replace posture the candidate MaxSim is scored on ADC; in rerank posture the resident float `.mvec` rescores. The V6 `.tann` token graph is UNCHANGED (built over the resident float `.mvec`); wiring the graph over PQ codes buys no memory while the float stays resident, so it is deferred (paired with dropping the resident float — same reason Phase 1 deferred dense HNSW-over-PQ to #20). All changes are gated on a new `set_multivector_pq_config` so V5/V6 paths stay byte-identical when unconfigured.

**Tech Stack:** C++ (`source/`, `atire/`), reusing `ANT_pq_codec`, `ANT_multivector_store`/`ANT_multivector_source`, `ANT_token_index`, and the per-segment sidecar + compaction + backfill + eager lifecycle.

**Spec:** `docs/superpowers/specs/2026-07-08-vector-pq-phase2-tokens-design.md`. **Issue:** #18.

---

## Repo constraints (read before every task)

- **NO header dependency tracking** → after editing ANY `.h`, run `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding (stale-object runtime SEGV otherwise).
- After an ASan sweep the objects are ASan-instrumented; a full clean rebuild (`rm -f obj/*.o lib/libantelope_engine.a`) is required before a normal (non-ASan) link, else `undefined reference to __asan_version_mismatch_check`.
- Whole repo is `-fPIC`. `source/*.cpp` and `tests/*.cpp` are auto-discovered by the GNUmakefile. Build test `test_foo` (file `tests/test_foo.cpp`) with `make test_foo` → `bin/test_foo`. Tests use `#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)`; exit 0 on pass.
- Config setters are POST-open (call `open(dir)` first).
- Work ONLY in the dedicated worktree the executor creates (branch `feature/vector-pq-phase2`, under `.worktrees/`). Stage only named files — never `git add -A`, never build artifacts (`obj/`, `bin/`, `lib/`, `*.o`, `*.a`) or sidecars (`.mvpq`/`.mvec`/`.tann`/`.pq`/`.vec`). Never `git add docs/business-strategy-2026-07-07.md`.

## Reused interfaces (already exist — do NOT redefine)

`source/pq_codec.h`:
```cpp
class ANT_pq_codec {
public:
    enum { K = 256, KMEANS_ITERS = 25 };
    enum { METRIC_DOT = 0, METRIC_COSINE = 1, METRIC_L2 = 2 };
    static long train(const float *vectors, long long dimension, long long m, long long n, float *codebook); // m∤dim => 1; 0 ok
    static void encode(const float *vector, long long dimension, long long m, const float *codebook, unsigned char *codes);
    static void adc_table(const float *query, long long dimension, long long m, const float *codebook, long metric, double *table); // table[m*K]
    static double adc_score(const unsigned char *codes, long long m, const double *table);
    static void reconstruct(const unsigned char *codes, long long dimension, long long m, const float *codebook, float *out);
};
```
`source/vector_source.h`: abstract `ANT_vector_source` — `document_count/get_dimension/has/get/is_quantized/reconstruct/score`.
`source/multivector_store.h`: `ANT_multivector_store` (float/int8 ragged pool; `maxsim`, `token_*`, `vector_count`, `has`, `max_vector_count`) + `ANT_multivector_source` adapter + `ANT_multivector_store_writer`.
`source/index_tombstones.h`: `ANT_index_tombstones` (`is_deleted(docid)`).
`source/vector_store.h`: `ANT_vector_store::normalize(float*, dim)`, `ANT_vector_candidate`, `ANT_vector_candidate_insert(best,&count,top_k,score,gen,docid)`.

`ATIRE_segment_index` members/methods already present: `default_pq_m(long long dim)` (largest divisor of dim in `[1,min(16,dim)]`), `rerank_dimension_current`, `rerank_quant_current`, `rerank_configured()`, `set_rerank_config`, `segment_filename(buf,size,gen,"ext")`, `candidate_multiplier`, `token_index_M`, `token_index_ef_construction`, `token_top_p`, `enum { RERANK_QUANT_FLOAT=0, RERANK_QUANT_INT8=1 }`, `enum { PQ_POSTURE_REPLACE=0, PQ_POSTURE_RERANK=1 }`, the `struct segment` with `multivectors`/`token_index`, `maxsim_live`, `evaluate_filter_for_segment`/`_for_live`.

## `.mvpq` on-disk layout (pin this exactly)

```
offset 0   : magic  "ANTMVPQ1"          (8 bytes)
offset 8   : version u32 = 1            (4 bytes)
offset 12  : dimension     i64
offset 20  : documents     i64
offset 28  : total_tokens  i64
offset 36  : m             i64
offset 44  : k             i64  (== 256)
offset 52  : counts[documents]          (documents * 4 bytes, int32 per doc)
           : codes[total_tokens * m]    (total_tokens * m bytes)
           : codebook[m * k * (dim/m)]  (256 * dimension floats == 256*dimension*4 bytes)
```
`expected_size = 52 + documents*4 + total_tokens*m + 256*dimension*4`
(uses `m*(dimension/m) == dimension` since `m` divides `dimension`). `offsets[]` are derived at load by prefix-summing `counts[]` (offsets[0]=0, offsets[d+1]=offsets[d]+counts[d]); `sum(counts) == total_tokens` is a load validation.

## File Structure

- **Create** `source/multivector_pq_store.{h,cpp}` — `ANT_multivector_pq_store` (ragged PQ token store, `.mvpq` I/O, ADC-MaxSim, token accessors), `ANT_multivector_pq_store_writer`.
- **Modify** `atire/atire_segment_index.h` — `struct segment` gets `ANT_multivector_pq_store *multivector_pq`; new config/build/policy members + method decls; `enum` reuse.
- **Modify** `atire/atire_segment_index.cpp` — teardown, load-on-open (`append_segment`), eager hook in `flush`.
- **Modify** `atire/atire_segment_index_vector.cpp` — `set_multivector_pq_config`, `load/save_multivector_pq_config`, `build_multivector_pq`, `set_multivector_pq_policy`, `disk_segment_has_multivector_pq`, the replace-posture scorer branch in `multivector_candidates`.
- **Modify** `atire/atire_segment_index_compaction.cpp` — `.mvpq` retrain+renumber + refresh + teardown free (`.tann` rebuild unchanged, over float).
- **Create** tests: `test_mvpq_store`, `test_mvpq_config`, `test_mvpq_backfill`, `test_mvpq_search`, `test_mvpq_rerank`, `test_mvpq_compaction`, `test_mvpq_recall`.

---

## Task 1: `ANT_multivector_pq_store` — `.mvpq` sidecar + ADC-MaxSim + source

**Files:**
- Create: `source/multivector_pq_store.h`, `source/multivector_pq_store.cpp`
- Test: `tests/test_mvpq_store.cpp`

- [ ] **Step 1: Write the header `source/multivector_pq_store.h`**

```cpp
/*
	MULTIVECTOR_PQ_STORE.H -- per-segment PQ-compressed token pool (seg_G.mvpq).
	Ragged, self-contained sibling of ANT_multivector_store: one k=256 codebook
	per segment over the whole token pool, m code bytes per token. Implements the
	V6 token accessors + ADC-MaxSim scoring. Forgiving load: any validation failure -> degraded empty store
	(token_count()==0) so the segment falls back to the .mvec float/int8 pool.
*/
#ifndef MULTIVECTOR_PQ_STORE_H_
#define MULTIVECTOR_PQ_STORE_H_

class ANT_multivector_pq_store
{
private:
	long long dimension, documents, total_tokens, m;
	long metric;
	int *counts;			// documents ints, NULL when empty
	long long *offsets;		// documents+1 prefix sums, NULL when empty
	float *codebook;		// m*256*(dimension/m), NULL when empty
	unsigned char *codes;	// total_tokens*m, NULL when empty
	ANT_multivector_pq_store();
public:
	~ANT_multivector_pq_store();
	static ANT_multivector_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric);
	long long get_m(void) { return m; }
	long long get_dimension(void) { return dimension; }
	long long document_count(void) { return documents; }
	long long token_count(void) { return total_tokens; }
	long tokens_quantized(void) { return 1; }
	long has(long long docid) { return docid >= 0 && docid < documents && counts != 0 && counts[docid] > 0; }
	long long vector_count(long long docid) { return has(docid) ? counts[docid] : 0; }
	long long max_vector_count(void);
	long token_has(long long t) { return t >= 0 && t < total_tokens; }
	const unsigned char *token_codes(long long t) { return token_has(t) ? codes + t*m : 0; }
	void token_reconstruct(long long t, float *out);
	double token_score(long long t, const float *query, long metric_ignored);	// ADC; metric is the store's
	long long token_docid_of(long long t);
	// ADC-MaxSim: build num_query_vecs ADC tables once, then for each of docid's
	// tokens do m lookups per query vec, max per query vec, summed.
	double maxsim(long long docid, const float *query_vecs, long long num_query_vecs);
};

/*
	NOTE: no ANT_vector_source adapter here in Phase 2 — the .tann token graph
	stays over the resident float .mvec, so nothing builds an HNSW over PQ codes
	yet. The token-source/ANT_vector_source adapter is added by the deferred
	"PQ-backed token graph + drop resident float" work.
*/

class ANT_multivector_pq_store_writer
{
private:
	char *filename; long long dimension, m; long metric;
	float *buffer; long long capacity, total_tokens;
	int *counts; long long counts_capacity, documents;
public:
	ANT_multivector_pq_store_writer(); ~ANT_multivector_pq_store_writer();
	long create(const char *path, long long dim, long long m, long metric);
	long append(const float *vectors, long long num_vectors);	// one doc's M_d normalized rows; append(NULL,0) for a doc with no tokens
	long finish(void);		// trains codebook over the whole pool + encodes + writes atomically
	void abandon(void);
};
#endif /* MULTIVECTOR_PQ_STORE_H_ */
```

- [ ] **Step 2: Write the failing test `tests/test_mvpq_store.cpp`**

```cpp
/*
	TEST_MVPQ_STORE.CPP
	-------------------
	ANT_multivector_pq_store: ragged .mvpq round-trip, forgiving load, ADC-MaxSim
	vs exact-float MaxSim within tolerance, token-pool codebook determinism.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/multivector_pq_store.h"
#include "../source/multivector_store.h"
#include "../source/vector_store.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 16

static void norm_vec(long long seed, float *v)
{
unsigned long long s = (unsigned long long)(seed + 1) * 2654435761ULL;
double n = 0.0;
for (int d = 0; d < DIM; d++) { s = s*6364136223846793005ULL+1; v[d] = (float)((double)((s>>33)&0x7fffffff)/(double)0x7fffffff*2.0-1.0); n += (double)v[d]*v[d]; }
n = sqrt(n)+1e-9; for (int d = 0; d < DIM; d++) v[d] = (float)(v[d]/n);
}

/* build a ragged pool: doc d has (d%3)+1 tokens (doc 0 -> 1 token, ... , plus one empty doc) */
static void write_pool(const char *path, long long ndocs)
{
ANT_multivector_pq_store_writer w;
CHECK(w.create(path, DIM, 4, ANT_pq_codec::METRIC_DOT) == 0);
float row[8*DIM];
long long seed = 0;
for (long long d = 0; d < ndocs; d++)
	{
	long long md = (d == 2) ? 0 : (d % 3) + 1;	/* doc 2 has no tokens */
	for (long long t = 0; t < md; t++) norm_vec(seed++, row + t*DIM);
	CHECK(w.append(md > 0 ? row : NULL, md) == 0);
	}
CHECK(w.finish() == 0);
}

static void test_roundtrip_and_ragged(void)
{
char path[] = "/tmp/ant_mvpq_XXXXXX";
int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
const long long N = 12;
write_pool(path, N);

ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, DIM, N, ANT_pq_codec::METRIC_DOT);
CHECK(s->document_count() == N);
CHECK(s->get_m() == 4);
CHECK(s->has(0));
CHECK(!s->has(2));					/* empty doc */
CHECK(s->vector_count(0) == 1);
CHECK(s->vector_count(1) == 2);
CHECK(s->token_docid_of(0) == 0);	/* first token belongs to doc 0 */
long long total = 0; for (long long d = 0; d < N; d++) total += s->vector_count(d);
CHECK(s->token_count() == total);
delete s;
remove(path);
printf("test_roundtrip_and_ragged OK\n");
}

static void test_adc_maxsim_vs_exact(void)
{
char pq_path[] = "/tmp/ant_mvpq_a_XXXXXX";  int a = mkstemp(pq_path); CHECK(a>=0); close(a);
char mv_path[] = "/tmp/ant_mvf_a_XXXXXX";   int b = mkstemp(mv_path); CHECK(b>=0); close(b);
const long long N = 30;

/* identical ragged pool into both a float .mvec and a .mvpq */
ANT_multivector_pq_store_writer pw; CHECK(pw.create(pq_path, DIM, 4, ANT_pq_codec::METRIC_DOT) == 0);
ANT_multivector_store_writer fw;    CHECK(fw.create(mv_path, DIM) == 0);
float row[8*DIM]; long long seed = 0;
for (long long d = 0; d < N; d++)
	{ long long md = (d%4)+1; for (long long t=0;t<md;t++) norm_vec(seed++, row+t*DIM); CHECK(pw.append(row,md)==0); CHECK(fw.append(row,md)==0); }
CHECK(pw.finish() == 0); CHECK(fw.finish() == 0);

ANT_multivector_pq_store *pq = ANT_multivector_pq_store::load(pq_path, DIM, N, ANT_pq_codec::METRIC_DOT);
ANT_multivector_store *mv = ANT_multivector_store::load(mv_path, DIM, N);
CHECK(pq->token_count() == mv->token_count());

float q[3*DIM]; norm_vec(9999, q); norm_vec(9998, q+DIM); norm_vec(9997, q+2*DIM);
double max_abs_err = 0.0;
for (long long d = 0; d < N; d++)
	{
	double e = mv->maxsim(d, q, 3), a2 = pq->maxsim(d, q, 3);
	double err = fabs(e - a2); if (err > max_abs_err) max_abs_err = err;
	}
printf("  ADC-MaxSim max abs err vs exact = %.4f\n", max_abs_err);
CHECK(max_abs_err < 0.30);			/* PQ approximation on 16-D, m=4 */
delete pq; delete mv; remove(pq_path); remove(mv_path);
printf("test_adc_maxsim_vs_exact OK\n");
}

static void test_determinism_and_forgiving(void)
{
char p1[] = "/tmp/ant_mvpq_d1_XXXXXX"; int a=mkstemp(p1); CHECK(a>=0); close(a);
char p2[] = "/tmp/ant_mvpq_d2_XXXXXX"; int b=mkstemp(p2); CHECK(b>=0); close(b);
write_pool(p1, 20); write_pool(p2, 20);
/* byte-identical files (deterministic k-means over identical pool) */
FILE *f1 = fopen(p1,"rb"), *f2 = fopen(p2,"rb");
fseek(f1,0,SEEK_END); fseek(f2,0,SEEK_END); long l1=ftell(f1), l2=ftell(f2);
CHECK(l1 == l2 && l1 > 52);
fseek(f1,0,SEEK_SET); fseek(f2,0,SEEK_SET);
unsigned char *b1=(unsigned char*)malloc(l1), *b2=(unsigned char*)malloc(l2);
CHECK(fread(b1,1,l1,f1)==(size_t)l1); CHECK(fread(b2,1,l2,f2)==(size_t)l2);
CHECK(memcmp(b1,b2,l1) == 0);
fclose(f1); fclose(f2); free(b1); free(b2);

/* forgiving: missing -> empty; truncated -> empty; wrong-dim -> empty */
ANT_multivector_pq_store *miss = ANT_multivector_pq_store::load("/tmp/ant_mvpq_nope_zzz", DIM, 20, ANT_pq_codec::METRIC_DOT);
CHECK(miss->token_count() == 0 && miss->document_count() == 0); delete miss;

FILE *tf = fopen(p1,"rb+"); CHECK(tf!=NULL); CHECK(ftruncate(fileno(tf), 40)==0); fclose(tf);
ANT_multivector_pq_store *trunc = ANT_multivector_pq_store::load(p1, DIM, 20, ANT_pq_codec::METRIC_DOT);
CHECK(trunc->token_count() == 0); delete trunc;

ANT_multivector_pq_store *wd = ANT_multivector_pq_store::load(p2, DIM+1, 20, ANT_pq_codec::METRIC_DOT);
CHECK(wd->token_count() == 0); delete wd;

remove(p1); remove(p2);
printf("test_determinism_and_forgiving OK\n");
}

int main(void)
{
test_roundtrip_and_ragged();
test_adc_maxsim_vs_exact();
test_determinism_and_forgiving();
printf("test_mvpq_store PASSED\n");
return 0;
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `make test_mvpq_store 2>&1 | tail -5`
Expected: FAIL — `multivector_pq_store.h`/`.cpp` don't exist / undefined references.

- [ ] **Step 4: Implement `source/multivector_pq_store.cpp`**

Mirror `source/pq_store.cpp` and `source/multivector_store.cpp`. Full implementation:

```cpp
/*
	MULTIVECTOR_PQ_STORE.CPP -- see header. Ragged PQ token pool, reuses ANT_pq_codec.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multivector_pq_store.h"
#include "pq_codec.h"

ANT_multivector_pq_store::ANT_multivector_pq_store() :
	dimension(0), documents(0), total_tokens(0), m(0), metric(0),
	counts(0), offsets(0), codebook(0), codes(0) {}

ANT_multivector_pq_store::~ANT_multivector_pq_store()
{
delete [] counts; delete [] offsets; delete [] codebook; delete [] codes;
}

long long ANT_multivector_pq_store::max_vector_count(void)
{
long long best = 0;
for (long long d = 0; d < documents; d++) if (counts[d] > best) best = counts[d];
return best;
}

long long ANT_multivector_pq_store::token_docid_of(long long t)
{
if (!token_has(t)) return -1;
/* binary search offsets: largest d with offsets[d] <= t */
long long lo = 0, hi = documents;
while (lo + 1 < hi) { long long mid = (lo+hi)/2; if (offsets[mid] <= t) lo = mid; else hi = mid; }
return lo;
}

void ANT_multivector_pq_store::token_reconstruct(long long t, float *out)
{
if (!token_has(t)) { memset(out, 0, (size_t)(dimension*sizeof(float))); return; }
ANT_pq_codec::reconstruct(codes + t*m, dimension, m, codebook, out);
}

double ANT_multivector_pq_store::token_score(long long t, const float *query, long)
{
if (!token_has(t)) return 0.0;
double *table = new double[(size_t)(m * ANT_pq_codec::K)];
ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);
double s = ANT_pq_codec::adc_score(codes + t*m, m, table);
delete [] table;
return s;
}

double ANT_multivector_pq_store::maxsim(long long docid, const float *query_vecs, long long num_query_vecs)
{
if (!has(docid) || num_query_vecs < 1) return 0.0;
/* one ADC table per query vector, built once */
double *tables = new double[(size_t)(num_query_vecs * m * ANT_pq_codec::K)];
for (long long q = 0; q < num_query_vecs; q++)
	ANT_pq_codec::adc_table(query_vecs + q*dimension, dimension, m, codebook, metric, tables + q*(m*ANT_pq_codec::K));

long long begin = offsets[docid], end = offsets[docid] + counts[docid];
double total = 0.0;
for (long long q = 0; q < num_query_vecs; q++)
	{
	const double *table = tables + q*(m*ANT_pq_codec::K);
	double best = 0.0; int seen = 0;
	for (long long t = begin; t < end; t++)
		{ double s = ANT_pq_codec::adc_score(codes + t*m, m, table); if (!seen || s > best) { best = s; seen = 1; } }
	total += best;
	}
delete [] tables;
return total;
}

/* ---- forgiving load ---- */
static long long read_i64(const unsigned char *p) { long long v; memcpy(&v, p, 8); return v; }

ANT_multivector_pq_store *ANT_multivector_pq_store::load(const char *filename, long long expected_dimension, long long expected_documents, long metric)
{
ANT_multivector_pq_store *s = new ANT_multivector_pq_store();
s->metric = metric;
FILE *in = fopen(filename, "rb");
if (in == NULL) return s;								/* missing -> empty */

/* read the 52-byte header first */
unsigned char hdr[52];
if (fread(hdr, 1, 52, in) != 52) { fclose(in); return s; }
if (memcmp(hdr, "ANTMVPQ1", 8) != 0) { fclose(in); return s; }
unsigned int version; memcpy(&version, hdr+8, 4);
if (version != 1) { fclose(in); return s; }
long long dim = read_i64(hdr+12), docs = read_i64(hdr+20), toks = read_i64(hdr+28), mm = read_i64(hdr+36), kk = read_i64(hdr+44);
if (dim != expected_dimension || docs != expected_documents || kk != ANT_pq_codec::K) { fclose(in); return s; }
if (mm < 1 || dim < 1 || mm > dim || dim % mm != 0 || docs < 0 || toks < 0) { fclose(in); return s; }

/* exact file size check BEFORE allocating */
long long expected_size = 52 + docs*4 + toks*mm + 256*dim*4;
if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return s; }
long long actual = ftell(in);
if (actual != expected_size) { fclose(in); return s; }
if (fseek(in, 52, SEEK_SET) != 0) { fclose(in); return s; }

int *counts = new int[docs > 0 ? docs : 1];
long long *offsets = new long long[docs + 1];
unsigned char *codes = new unsigned char[toks*mm > 0 ? toks*mm : 1];
long long cb_floats = 256*dim;
float *codebook = new float[cb_floats > 0 ? cb_floats : 1];

long ok = 1;
if (docs > 0 && fread(counts, 4, (size_t)docs, in) != (size_t)docs) ok = 0;
if (ok && toks > 0 && fread(codes, 1, (size_t)(toks*mm), in) != (size_t)(toks*mm)) ok = 0;
if (ok && fread(codebook, sizeof(float), (size_t)cb_floats, in) != (size_t)cb_floats) ok = 0;
fclose(in);

/* content validation: prefix-sum counts == total_tokens; code bytes < k (always true for unsigned char, k=256) */
if (ok)
	{
	offsets[0] = 0;
	for (long long d = 0; d < docs; d++)
		{ if (counts[d] < 0) { ok = 0; break; } offsets[d+1] = offsets[d] + counts[d]; }
	if (ok && offsets[docs] != toks) ok = 0;
	}

if (!ok) { delete [] counts; delete [] offsets; delete [] codes; delete [] codebook; return s; }	/* degrade -> empty */

s->dimension = dim; s->documents = docs; s->total_tokens = toks; s->m = mm;
s->counts = counts; s->offsets = offsets; s->codes = codes; s->codebook = codebook;
return s;
}

/* ---- writer ---- */
ANT_multivector_pq_store_writer::ANT_multivector_pq_store_writer() :
	filename(0), dimension(0), m(0), metric(0), buffer(0), capacity(0), total_tokens(0),
	counts(0), counts_capacity(0), documents(0) {}

ANT_multivector_pq_store_writer::~ANT_multivector_pq_store_writer() { abandon(); }

long ANT_multivector_pq_store_writer::create(const char *path, long long dim, long long mm, long met)
{
if (dim < 1 || mm < 1 || mm > dim || dim % mm != 0) return 1;
abandon();
filename = new char[strlen(path)+1]; strcpy(filename, path);
dimension = dim; m = mm; metric = met;
capacity = 1024; buffer = new float[capacity * dimension];
counts_capacity = 256; counts = new int[counts_capacity];
total_tokens = 0; documents = 0;
return 0;
}

long ANT_multivector_pq_store_writer::append(const float *vectors, long long num_vectors)
{
if (filename == NULL) return 1;
if (num_vectors < 0) num_vectors = 0;
if (documents >= counts_capacity)
	{ long long nc = counts_capacity*2; int *n = new int[nc]; memcpy(n, counts, (size_t)(documents*sizeof(int))); delete [] counts; counts = n; counts_capacity = nc; }
if (total_tokens + num_vectors > capacity)
	{ long long nc = capacity; while (total_tokens + num_vectors > nc) nc *= 2; float *n = new float[nc*dimension]; memcpy(n, buffer, (size_t)(total_tokens*dimension*sizeof(float))); delete [] buffer; buffer = n; capacity = nc; }
if (num_vectors > 0 && vectors != NULL)
	memcpy(buffer + total_tokens*dimension, vectors, (size_t)(num_vectors*dimension*sizeof(float)));
counts[documents++] = (int)num_vectors;
total_tokens += num_vectors;
return 0;
}

long ANT_multivector_pq_store_writer::finish(void)
{
if (filename == NULL) return 1;
long long cb_floats = 256*dimension;
float *codebook = new float[cb_floats];
if (ANT_pq_codec::train(buffer, dimension, m, total_tokens, codebook) != 0) { delete [] codebook; return 1; }

unsigned char *codes = new unsigned char[total_tokens*m > 0 ? total_tokens*m : 1];
for (long long t = 0; t < total_tokens; t++)
	ANT_pq_codec::encode(buffer + t*dimension, dimension, m, codebook, codes + t*m);

char *tmp = new char[strlen(filename)+5]; strcpy(tmp, filename); strcat(tmp, ".tmp");
FILE *out = fopen(tmp, "wb");
long ok = out != NULL;
if (ok)
	{
	unsigned int version = 1; long long k = 256;
	ok = fwrite("ANTMVPQ1", 1, 8, out) == 8
		&& fwrite(&version, 4, 1, out) == 1
		&& fwrite(&dimension, 8, 1, out) == 1
		&& fwrite(&documents, 8, 1, out) == 1
		&& fwrite(&total_tokens, 8, 1, out) == 1
		&& fwrite(&m, 8, 1, out) == 1
		&& fwrite(&k, 8, 1, out) == 1
		&& (documents == 0 || fwrite(counts, 4, (size_t)documents, out) == (size_t)documents)
		&& (total_tokens == 0 || fwrite(codes, 1, (size_t)(total_tokens*m), out) == (size_t)(total_tokens*m))
		&& fwrite(codebook, sizeof(float), (size_t)cb_floats, out) == (size_t)cb_floats;
	if (fclose(out) != 0) ok = 0;
	}
if (ok && rename(tmp, filename) != 0) ok = 0;
if (!ok) remove(tmp);
delete [] tmp; delete [] codes; delete [] codebook;
if (ok) abandon();
return ok ? 0 : 1;
}

void ANT_multivector_pq_store_writer::abandon(void)
{
delete [] filename; delete [] buffer; delete [] counts;
filename = 0; buffer = 0; counts = 0; capacity = 0; counts_capacity = 0; total_tokens = 0; documents = 0;
}
```

- [ ] **Step 5: Build and run**

Run: `make test_mvpq_store 2>&1 | tail -5 && ./bin/test_mvpq_store`
Expected: PASS — `test_mvpq_store PASSED` (and the printed ADC-MaxSim error line under 0.30).

- [ ] **Step 6: Commit**

```bash
git add source/multivector_pq_store.h source/multivector_pq_store.cpp tests/test_mvpq_store.cpp
git commit -m "feat(mvpq): ragged PQ token store (.mvpq) + ADC-MaxSim + source"
```

---

## Task 2: `set_multivector_pq_config` + persistence + int8 mutual exclusion

**Files:**
- Modify: `atire/atire_segment_index.h` (members + decls), `atire/atire_segment_index_vector.cpp` (impl)
- Test: `tests/test_mvpq_config.cpp`

- [ ] **Step 1: Header — add members + decls to `atire/atire_segment_index.h`**

In the class private data (near the other rerank/pq config fields) add:
```cpp
	long long mvpq_m_current;			// 0 = token-PQ unconfigured
	long mvpq_posture_current;			// PQ_POSTURE_REPLACE / PQ_POSTURE_RERANK
	long mvpq_rerank_quant_current;		// RERANK_QUANT_FLOAT / RERANK_QUANT_INT8
	long mvpq_eager;					// 0 ondemand (default), 1 eager
```
In the private methods add:
```cpp
	long load_multivector_pq_config(void);
	long save_multivector_pq_config(void);
```
In the public API add:
```cpp
	long set_multivector_pq_config(long long m, long posture, long rerank_quant);	// enable token-PQ (m==0 => default_pq_m over rerank_dimension); persists multivector_pq.config; idempotent same-config, nonzero if already set to a different config, mode invalid, m∤rerank_dimension, rerank(multivectors) not configured, or the .mvec int8 mode already enabled.
	long multivector_pq_configured(void) { return mvpq_m_current != 0; }
	long long multivector_pq_m(void) { return mvpq_m_current; }
	long set_multivector_pq_policy(long eager) { mvpq_eager = eager ? 1 : 0; return 0; }
```

- [ ] **Step 2: Initialize the members in the constructor (`atire/atire_segment_index.cpp`, near `pq_eager = 0;`)**

```cpp
mvpq_m_current = 0;
mvpq_posture_current = PQ_POSTURE_REPLACE;
mvpq_rerank_quant_current = RERANK_QUANT_FLOAT;
mvpq_eager = 0;
```

- [ ] **Step 3: Add the `load_multivector_pq_config()` call in `open()`**

In `atire/atire_segment_index.cpp` `open()`, right after the existing `load_pq_config();` call, add:
```cpp
load_multivector_pq_config();
```

- [ ] **Step 4: Write the failing test `tests/test_mvpq_config.cpp`**

```cpp
/*
	TEST_MVPQ_CONFIG.CPP -- set_multivector_pq_config()/persistence/mutual exclusion
	with the .mvec int8 mode (set_rerank_config's RERANK_QUANT_INT8).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqcfg_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }

int main(void)
{
/* basic set + idempotent + immutable */
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(8, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
CHECK(ix->multivector_pq_configured()); CHECK(ix->multivector_pq_m() == 4);
delete ix; delete [] d;
}
/* m must divide rerank dimension */
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(3, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
CHECK(!ix->multivector_pq_configured());
delete ix; delete [] d;
}
/* requires rerank(multivectors) configured */
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
delete ix; delete [] d;
}
/* mutual exclusion with the .mvec int8 mode, both directions */
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);	/* int8 token pool */
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
CHECK(!ix->multivector_pq_configured());
delete ix; delete [] d;
}
/* default m over rerank dimension (dim 16 -> largest divisor <=16 == 16) */
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(0, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->multivector_pq_m() == 16);
delete ix; delete [] d;
}
/* persistence across reopen */
{
char *d = dir_(); ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(a->open(d) == 0);
CHECK(a->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
delete a;
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(b->open(d) == 0);
CHECK(b->multivector_pq_configured()); CHECK(b->multivector_pq_m() == 4);
delete b; delete [] d;
}
printf("test_mvpq_config PASSED\n");
return 0;
}
```

- [ ] **Step 5: Run to verify it fails**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_config 2>&1 | tail -5`
Expected: FAIL — `set_multivector_pq_config` undefined.

- [ ] **Step 6: Implement in `atire/atire_segment_index_vector.cpp`**

Mirror `load_pq_config`/`save_pq_config`/`set_pq_config` exactly (search for them at ~lines 455/486/527). Add:

```cpp
/*
	ATIRE_SEGMENT_INDEX::LOAD_MULTIVECTOR_PQ_CONFIG() / SAVE_...()
	-------------------------------------------------------------
	Persist token-PQ config in <dir>/multivector_pq.config (magic "ANTMVPQC",
	version 1, three i64: m, posture, rerank_quant).  Defensive parse: any
	mismatch leaves token-PQ unconfigured.  Mirrors load/save_pq_config().
*/
long ATIRE_segment_index::load_multivector_pq_config(void)
{
if (directory == NULL) return 1;
char name[4096]; snprintf(name, sizeof(name), "%s/multivector_pq.config", directory);
FILE *in = fopen(name, "rb"); if (in == NULL) return 1;
char tag[8]; unsigned int version; long long vals[3];
long ok = fread(tag, 1, 8, in) == 8 && memcmp(tag, "ANTMVPQC", 8) == 0
	&& fread(&version, 4, 1, in) == 1 && version == 1
	&& fread(vals, 8, 3, in) == 3;
fclose(in);
if (!ok) return 1;
mvpq_m_current = vals[0];
mvpq_posture_current = (long)vals[1];
mvpq_rerank_quant_current = (long)vals[2];
return 0;
}

long ATIRE_segment_index::save_multivector_pq_config(void)
{
if (directory == NULL) return 1;
char name[4096], tmp[4112];
snprintf(name, sizeof(name), "%s/multivector_pq.config", directory);
snprintf(tmp, sizeof(tmp), "%s.tmp", name);
FILE *out = fopen(tmp, "wb"); if (out == NULL) return 1;
unsigned int version = 1; long long vals[3] = { mvpq_m_current, mvpq_posture_current, mvpq_rerank_quant_current };
long ok = fwrite("ANTMVPQC", 1, 8, out) == 8 && fwrite(&version, 4, 1, out) == 1 && fwrite(vals, 8, 3, out) == 3;
if (fclose(out) != 0) ok = 0;
if (ok && rename(tmp, name) != 0) ok = 0;
if (!ok) remove(tmp);
return ok ? 0 : 1;
}

/*
	ATIRE_SEGMENT_INDEX::SET_MULTIVECTOR_PQ_CONFIG()
	------------------------------------------------
	Enable token-PQ.  Requires rerank(multivectors) configured; m must divide the
	rerank dimension (m==0 => default_pq_m()); mutually exclusive with the .mvec
	int8 mode (rerank_quant_current==RERANK_QUANT_INT8).  Immutable once set.
*/
long ATIRE_segment_index::set_multivector_pq_config(long long m, long posture, long rerank_quant)
{
if (!rerank_configured())							/* needs a token pool */
	return 1;
if (rerank_quant_current == RERANK_QUANT_INT8)		/* mutually exclusive with .mvec int8 */
	return 1;
if (posture != PQ_POSTURE_REPLACE && posture != PQ_POSTURE_RERANK)
	return 1;
if (rerank_quant != RERANK_QUANT_FLOAT && rerank_quant != RERANK_QUANT_INT8)
	return 1;
if (m == 0)
	m = default_pq_m(rerank_dimension_current);
if (m < 1 || m > rerank_dimension_current || rerank_dimension_current % m != 0)
	return 1;
if (multivector_pq_configured())					/* immutable */
	return (mvpq_m_current == m && mvpq_posture_current == posture && mvpq_rerank_quant_current == rerank_quant) ? 0 : 1;
mvpq_m_current = m;
mvpq_posture_current = posture;
mvpq_rerank_quant_current = rerank_quant;
if (save_multivector_pq_config() != 0)
	{ mvpq_m_current = 0; return 1; }
return 0;
}
```
Also add the reciprocal guard to `set_rerank_config` (so the int8 token mode can't be enabled after token-PQ): in `set_rerank_config` (`~line 625`), before setting fields, add:
```cpp
if (quant == RERANK_QUANT_INT8 && multivector_pq_configured())
	return 1;					// mutually exclusive with token-PQ
```

- [ ] **Step 7: Run to verify it passes**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_config 2>&1 | tail -5 && ./bin/test_mvpq_config`
Expected: PASS — `test_mvpq_config PASSED`.

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_mvpq_config.cpp
git commit -m "feat(mvpq): set_multivector_pq_config + persistence + int8 mutual exclusion"
```

---

## Task 3: Segment load `.mvpq` + teardown + `build_multivector_pq` + eager

**Files:**
- Modify: `atire/atire_segment_index.h` (segment field + decls), `atire/atire_segment_index.cpp` (teardown, load, eager), `atire/atire_segment_index_vector.cpp` (build + accessor)
- Test: `tests/test_mvpq_backfill.cpp`

- [ ] **Step 1: Header — `struct segment` field + method decls (`atire/atire_segment_index.h`)**

Add a forward declaration near the other store forward decls:
```cpp
class ANT_multivector_pq_store;
```
In `struct segment`, right after `ANT_token_index *token_index;`, add:
```cpp
	ANT_multivector_pq_store *multivector_pq;	// PQ-compressed token pool (seg_G.mvpq); NULL unless token-PQ configured AND a valid .mvpq loaded/built
```
Add public method decls near `build_token_index`:
```cpp
	long build_multivector_pq(void);					// on-demand backfill; 0 success, 1 if unconfigured / no multivectors
	long disk_segment_has_multivector_pq(long long which);	// test accessor
```

- [ ] **Step 2: Include + teardown + NULL-init + eager hook (`atire/atire_segment_index.cpp`)**

Add the include near the other `../source/*_store.h`:
```cpp
#include "../source/multivector_pq_store.h"
```
In the destructor loop, right after `delete segments[which].token_index;`, add:
```cpp
	delete segments[which].multivector_pq;
```
In `append_segment()`, in the `if (rerank_configured()) { ... }` block that loads `multivectors`/`token_index` (~line 1563), set the new field to NULL by default and load it. Change the block so that after `token_index` is loaded, it reads:
```cpp
	segments[segment_count].multivector_pq = NULL;
	if (multivector_pq_configured())
		{
		char mvpq_filename[1024];
		segment_filename(mvpq_filename, sizeof(mvpq_filename), generation, "mvpq");
		ANT_multivector_pq_store *p = ANT_multivector_pq_store::load(mvpq_filename, rerank_dimension_current, engine->get_document_count(), ANT_pq_codec::METRIC_DOT);
		if (p->document_count() == engine->get_document_count() && p->token_count() > 0)
			segments[segment_count].multivector_pq = p;
		else
			delete p;			/* no/degraded .mvpq -> NULL -> .mvec fallback */
		}
```
And in the matching `else` branch (rerank unconfigured), add `segments[segment_count].multivector_pq = NULL;`.
Add `#include "../source/pq_codec.h"` if not already included (needed for `ANT_pq_codec::METRIC_DOT`).
In `flush()`, right after the existing `if (token_index_eager) build_token_index();` block, add:
```cpp
if (mvpq_eager)
	build_multivector_pq();
```

- [ ] **Step 3: Write the failing test `tests/test_mvpq_backfill.cpp`**

```cpp
/*
	TEST_MVPQ_BACKFILL.CPP -- build_multivector_pq() creates .mvpq for a flushed
	segment, disk_segment_has_multivector_pq reflects it, reopen loads it, eager builds at flush.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 16

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqbf_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }

static void mv(long long seed, float *v)
{ unsigned long long s=(unsigned long long)(seed+1)*2654435761ULL; double n=0; for (int d=0;d<DIM;d++){s=s*6364136223846793005ULL+1; v[d]=(float)((double)((s>>33)&0x7fffffff)/(double)0x7fffffff*2.0-1.0); n+=(double)v[d]*v[d];} n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }

static void fill(ATIRE_segment_index *ix, long long n)
{
float doc[4*DIM]; char key[32], body[64]; long long seed = 0;
for (long long i = 0; i < n; i++)
	{
	long long md = (i%3)+1; for (long long t=0;t<md;t++) mv(seed++, doc+t*DIM);
	sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld body</DOC>", i);
	CHECK(ix->add_document(key, body, NULL, doc, md) >= 0);
	}
CHECK(ix->flush() == 0);
}

int main(void)
{
/* build + reload */
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
fill(ix, 40);
CHECK(ix->disk_segment_has_multivector_pq(0) == 0);
CHECK(ix->build_multivector_pq() == 0);
CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
delete ix;
ATIRE_segment_index *re = new ATIRE_segment_index();
CHECK(re->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(re->open(d) == 0);
CHECK(re->multivector_pq_configured());
CHECK(re->disk_segment_has_multivector_pq(0) == 1);
delete re; delete [] d;
}
/* unconfigured -> no-op nonzero */
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->open(d) == 0);
CHECK(ix->build_multivector_pq() == 1);
delete ix; delete [] d;
}
/* eager builds at flush */
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_policy(1) == 0);
fill(ix, 40);
CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
delete ix; delete [] d;
}
printf("test_mvpq_backfill PASSED\n");
return 0;
}
```

- [ ] **Step 4: Run to verify it fails**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_backfill 2>&1 | tail -5`
Expected: FAIL — `build_multivector_pq`/`disk_segment_has_multivector_pq` undefined.

- [ ] **Step 5: Implement `build_multivector_pq` + accessor (`atire/atire_segment_index_vector.cpp`)**

Mirror `build_token_index` (~line 875) for the loop shape and `build_pq` for the writer/swap. The source of truth is the resident float `.mvec` store (`segments[which].multivectors`), read doc-by-doc via `copy_vectors`:

```cpp
/*
	ATIRE_SEGMENT_INDEX::BUILD_MULTIVECTOR_PQ()
	-------------------------------------------
	On-demand backfill: for every segment with a float .mvec token pool and no
	valid .mvpq, train a per-segment codebook over the token pool + encode +
	write the sidecar, then swap the in-memory store.  The .mvec is KEPT resident
	(exact rerank tier).  Per-segment failures skip; idempotent.  Returns 0, or 1
	if token-PQ is unconfigured / no multivectors.
*/
long ATIRE_segment_index::build_multivector_pq(void)
{
long long which;
char mvpq_name[4096];

if (!multivector_pq_configured() || rerank_dimension_current == 0)
	return 1;

for (which = 0; which < segment_count; which++)
	{
	ANT_multivector_store *mv = segments[which].multivectors;
	if (mv == NULL || mv->tokens_quantized())			/* need a float pool to train from */
		continue;
	long long docs = segments[which].engine->get_document_count();
	if (segments[which].multivector_pq != NULL
		&& segments[which].multivector_pq->document_count() == docs
		&& segments[which].multivector_pq->token_count() > 0)
		continue;										/* already has a valid .mvpq */

	segment_filename(mvpq_name, sizeof(mvpq_name), segments[which].generation, "mvpq");
	ANT_multivector_pq_store_writer w;
	long failed = w.create(mvpq_name, rerank_dimension_current, mvpq_m_current, ANT_pq_codec::METRIC_DOT) != 0;
	long long cap = mv->max_vector_count();
	float *buf = new float[(cap > 0 ? cap : 1) * rerank_dimension_current];
	for (long long d = 0; !failed && d < docs; d++)
		{
		long long md = mv->copy_vectors(d, buf);		/* reconstructed + normalized rows; 0 if absent */
		failed = w.append(md > 0 ? buf : NULL, md) != 0;
		}
	delete [] buf;
	if (!failed)
		failed = w.finish() != 0;
	if (failed)
		w.abandon();
	else
		{
		delete segments[which].multivector_pq;
		segments[which].multivector_pq = ANT_multivector_pq_store::load(mvpq_name, rerank_dimension_current, docs, ANT_pq_codec::METRIC_DOT);
		}
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::DISK_SEGMENT_HAS_MULTIVECTOR_PQ()
	-----------------------------------------------------
	Test accessor: 1 if segment `which` has a non-empty PQ token store.
*/
long ATIRE_segment_index::disk_segment_has_multivector_pq(long long which)
{
return (which >= 0 && which < segment_count && segments[which].multivector_pq != NULL && segments[which].multivector_pq->token_count() > 0) ? 1 : 0;
}
```
Add `#include "../source/multivector_pq_store.h"` and `#include "../source/pq_codec.h"` to `atire/atire_segment_index_vector.cpp` if not present.

- [ ] **Step 6: Run to verify it passes**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_backfill 2>&1 | tail -5 && ./bin/test_mvpq_backfill`
Expected: PASS — `test_mvpq_backfill PASSED`.

- [ ] **Step 7: Regression**

Run: `make test_mvpq_store && ./bin/test_mvpq_store && make test_mvpq_config && ./bin/test_mvpq_config && make test_segment_index && ./bin/test_segment_index`
Expected: all PASSED.

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_mvpq_backfill.cpp
git commit -m "feat(mvpq): per-segment .mvpq load + build_multivector_pq + eager"
```

---

## Task 4: Replace posture (ADC-MaxSim scoring)

**Files:**
- Modify: `atire/atire_segment_index_vector.cpp` (`multivector_candidates`)
- Test: `tests/test_mvpq_search.cpp`

**Context:** The V6 `.tann` token graph stays over the resident float `.mvec` (unchanged — the PQ-backed graph is deferred to the phase that drops the resident float; see issue). PQ affects only the *scoring* step. `multivector_candidates` (`~line 2107`) scores candidates with `mv->maxsim(did, qn, num)` in two places (token-ANN branch line ~2136 and brute-force branch line ~2152) and the live buffer with `maxsim_live`. The replace posture swaps the *disk-segment* scorer to `segments[which].multivector_pq->maxsim(...)` when a valid `.mvpq` exists (else float `mv->maxsim` fallback). The live buffer stays exact float. **No `ANT_token_index` / `.tann` changes in this task.**

- [ ] **Step 1: Write the failing test `tests/test_mvpq_search.cpp`**

```cpp
/*
	TEST_MVPQ_SEARCH.CPP -- replace posture: search_multivector scores via ADC-MaxSim.
	Top-1 matches exact on well-separated data; V5/V6-unconfigured search unchanged.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 16

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqs_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }

/* doc i: a single token dominant on unique axis i (i < DIM) -> unambiguous top-1 */
static void doc_vec(long long i, float *v)
{ for (int d=0;d<DIM;d++) v[d]=0.01f*(float)(((i*3+d)%7)-3); v[i%DIM]+=5.0f;
  double n=0; for(int d=0;d<DIM;d++) n+=(double)v[d]*v[d]; n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }

static void fill(ATIRE_segment_index *ix, long long n)
{ float t[DIM]; char k[32], b[64]; for (long long i=0;i<n;i++){ doc_vec(i,t); sprintf(k,"doc-%lld",i); sprintf(b,"<DOC>term%lld x</DOC>",i); CHECK(ix->add_document(k,b,NULL,t,1)>=0);} CHECK(ix->flush()==0); }

int main(void)
{
const long long N = 12;			/* <= DIM: unique dominant axes */
/* exact (no token-PQ) */
char *de = dir_(); ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ex->open(de) == 0); fill(ex, N);
/* replace-posture token-PQ */
char *dp = dir_(); ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(pq->open(dp) == 0);
CHECK(pq->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
fill(pq, N);
CHECK(pq->build_multivector_pq() == 0);
CHECK(pq->disk_segment_has_multivector_pq(0) == 1);

float q[DIM]; doc_vec(5, q);		/* query == doc-5's token */
CHECK(ex->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(ex->get_hit(0)->filename, "doc-5") == 0);
CHECK(pq->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(pq->get_hit(0)->filename, "doc-5") == 0);	/* ADC-MaxSim top-1 == exact */

/* V5/V6-unconfigured byte-identical: two PQ-less indexes agree rank-for-rank */
char *da = dir_(); ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0); CHECK(a->open(da) == 0); fill(a, N);
char *db = dir_(); ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0); CHECK(b->open(db) == 0); fill(b, N);
float q2[DIM]; doc_vec(9, q2);
long long ca = a->search_multivector(q2, 1, 10), cb = b->search_multivector(q2, 1, 10);
CHECK(ca == cb);
for (long long i = 0; i < ca; i++)
	{ CHECK(strcmp(a->get_hit(i)->filename, b->get_hit(i)->filename) == 0); CHECK(fabs(a->get_hit(i)->score - b->get_hit(i)->score) < 1e-9); }

delete ex; delete pq; delete a; delete b; delete [] de; delete [] dp; delete [] da; delete [] db;
printf("test_mvpq_search PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_search 2>&1 | tail -8 && ./bin/test_mvpq_search`
Expected: FAIL on the PQ top-1 assertion (replace posture not yet scoring via ADC — `multivector_candidates` still uses float `mv->maxsim`).

- [ ] **Step 3: Implement the replace-posture scorer in `multivector_candidates` (`atire/atire_segment_index_vector.cpp`)**

At the top of the per-segment loop body in `multivector_candidates`, pick the disk-segment scorer once:
```cpp
	ANT_multivector_store *mv = segments[which].multivectors;
	if (mv == NULL)
		continue;
	long use_pq = (multivector_pq_configured() && mvpq_posture_current == PQ_POSTURE_REPLACE
		&& segments[which].multivector_pq != NULL
		&& segments[which].multivector_pq->token_count() > 0
		&& segments[which].multivector_pq->document_count() == segments[which].engine->get_document_count());
	ANT_multivector_pq_store *pqs = segments[which].multivector_pq;
```
Then in BOTH the token-ANN branch and the brute-force branch, replace the score expression `mv->maxsim(did, qn, num_query_vecs)` with:
```cpp
	(use_pq ? pqs->maxsim(did, qn, num_query_vecs) : mv->maxsim(did, qn, num_query_vecs))
```
(The `mv->has(did)` gate stays — presence is the same doc set in float and PQ pools. Leave the live-buffer `maxsim_live(...)` merge exactly as-is: the live buffer is always exact float.)

- [ ] **Step 4: Run to verify it passes**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_search 2>&1 | tail -8 && ./bin/test_mvpq_search`
Expected: PASS — `test_mvpq_search PASSED`.

- [ ] **Step 5: Regression (V6 paths unchanged)**

Run: `for t in test_mvpq_backfill test_segment_index test_v6_search_multivector test_v6_token_index test_v6_recall; do make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1; done`
Expected: all PASSED / ALL TESTS PASSED (V6 `.tann` path is untouched).

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_mvpq_search.cpp
git commit -m "feat(mvpq): replace posture ADC-MaxSim scoring"
```

---

## Task 5: Rerank posture (PQ candidate shortlist → exact float MaxSim rescore)

**Files:**
- Modify: `atire/atire_segment_index_vector.cpp` (`multivector_candidates`)
- Test: `tests/test_mvpq_rerank.cpp`

**Context:** In rerank posture, candidate *generation* uses the V6 float token-HNSW (`.tann` over the resident float `.mvec`, unchanged), and the final MaxSim *scoring* uses the resident exact float `mv->maxsim` — i.e. exactly the `use_pq == 0` scoring path from Task 4. So rerank posture is "float `.tann` candidates + float MaxSim"; token-PQ's contribution in rerank is purely the compressed on-disk `.mvpq` (and, in a future phase, a PQ-backed graph). `RERANK_QUANT_INT8` aliases float in Phase 2 (documented). Since both replace and rerank reuse Task 4's scoring switch, this task is mostly a recall lock; it only needs code if the pool must widen (Step 3).

- [ ] **Step 1: Write the failing test `tests/test_mvpq_rerank.cpp`**

```cpp
/*
	TEST_MVPQ_RERANK.CPP -- rerank posture: float token-HNSW candidates -> exact
	float MaxSim rescore. recall@10 vs exact >= replace and >= 0.9.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 32

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqr_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }
static unsigned long long R;
static double nf(void){ R=R*6364136223846793005ULL+1442695040888963407ULL; return (double)((R>>33)&0x7fffffff)/(double)0x7fffffff; }
static void tok(long long seed, float *v){ R=(unsigned long long)(seed+1)*2654435761ULL; double n=0; for(int d=0;d<DIM;d++){v[d]=(float)(nf()*2.0-1.0);n+=(double)v[d]*v[d];} n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }

static void fill(ATIRE_segment_index *ix, long long n)
{ float doc[3*DIM]; char k[32], b[64]; long long seed=0;
  for(long long i=0;i<n;i++){ long long md=(i%3)+1; for(long long t=0;t<md;t++) tok(seed++, doc+t*DIM); sprintf(k,"doc-%lld",i); sprintf(b,"<DOC>term%lld z</DOC>",i); CHECK(ix->add_document(k,b,NULL,doc,md)>=0);} CHECK(ix->flush()==0); }

static ATIRE_segment_index *mk(char **d, long has_pq, long posture)
{ *d=dir_(); ATIRE_segment_index *ix=new ATIRE_segment_index(); CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0); CHECK(ix->open(*d)==0);
  if (has_pq) CHECK(ix->set_multivector_pq_config(8, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0); fill(ix, 300); if (has_pq){ CHECK(ix->build_multivector_pq()==0); CHECK(ix->build_token_index()==0);} return ix; }

static double recall(ATIRE_segment_index *cand, ATIRE_segment_index *ex, long long nq)
{ long long hit=0,tot=0; float q[3*DIM];
  for(long long qi=0;qi<nq;qi++){ long long m=(qi%3)+1; for(long long t=0;t<m;t++) tok(50000+qi*3+t, q+t*DIM);
    long long en=ex->search_multivector(q,m,10); long long e[10]; for(long long i=0;i<en;i++) e[i]=atoll(ex->get_hit(i)->filename+4);
    long long cn=cand->search_multivector(q,m,10); long long c[10]; for(long long i=0;i<cn;i++) c[i]=atoll(cand->get_hit(i)->filename+4);
    for(long long a=0;a<en;a++){ tot++; for(long long bb=0;bb<cn;bb++) if(e[a]==c[bb]){hit++;break;} } }
  return tot? (double)hit/(double)tot : 0.0; }

int main(void)
{
char *de; ATIRE_segment_index *ex = mk(&de, 0, 0);
char *dp; ATIRE_segment_index *rep = mk(&dp, 1, ATIRE_segment_index::PQ_POSTURE_REPLACE);
char *dr; ATIRE_segment_index *rr = mk(&dr, 1, ATIRE_segment_index::PQ_POSTURE_RERANK);
double r_rep = recall(rep, ex, 30), r_rr = recall(rr, ex, 30);
printf("  replace recall@10=%.3f  rerank recall@10=%.3f\n", r_rep, r_rr);
CHECK(r_rr >= 0.9);
CHECK(r_rr >= r_rep - 1e-9);
delete ex; delete rep; delete rr; delete [] de; delete [] dp; delete [] dr;
printf("test_mvpq_rerank PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run to verify it fails or passes**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_rerank 2>&1 | tail -5 && ./bin/test_mvpq_rerank`
Expected: it MAY already pass (rerank posture reuses Task 4's float-scoring path). If `r_rr < 0.9`, the candidate pool is too small — implement Step 3. If it passes, note that and skip Step 3.

- [ ] **Step 3: (Only if recall short) widen the rerank candidate pool**

Rerank precision depends on the token-ANN pool being wide enough that the true top-10 are among the candidates. If `r_rr < 0.9`, in `multivector_candidates` raise the pool for the rerank posture: where `pool_size = top_k * candidate_multiplier` is computed, use a wider multiplier when `multivector_pq_configured() && mvpq_posture_current == PQ_POSTURE_RERANK` (e.g. `pool_size = top_k * candidate_multiplier * 2`). Re-run. Do NOT lower the 0.9 threshold. Report the multiplier used.

- [ ] **Step 4: Regression**

Run: `for t in test_mvpq_search test_mvpq_backfill test_segment_index test_v6_recall; do make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1; done`
Expected: all PASSED.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_mvpq_rerank.cpp
git commit -m "feat(mvpq): rerank posture (PQ candidates -> exact float MaxSim rescore)"
```

---

## Task 6: Compaction rebuilds `.mvpq` (retrain + renumber)

**Files:**
- Modify: `atire/atire_segment_index_compaction.cpp`
- Test: `tests/test_mvpq_compaction.cpp`

**Context:** `atire/atire_segment_index_compaction.cpp` rebuilds `.mvec` then (in the post-merge block) refreshes `multivectors`, rebuilds `.tann` (over the float `.mvec`), then refreshes attributes — all before "Step 6" shuffle. Add a `.mvpq` rebuild (retrain + renumber) that refreshes `output_segment->multivector_pq` after its sidecar is finalized. The `.tann` rebuild is UNCHANGED (it still borrows `multivectors`, not `multivector_pq`, so there's no cross-store lifetime ordering constraint here — the PQ-backed graph is deferred). The one lifetime rule that DOES apply: the Step-6 shuffle teardown must free `multivector_pq` (the dense-PQ C1 leak lesson).

- [ ] **Step 1: Include + teardown free (`atire/atire_segment_index_compaction.cpp`)**

Add `#include "../source/multivector_pq_store.h"` and `#include "../source/pq_codec.h"` near the other store includes. In the Step-6 shuffle teardown loop, right after `delete segments[which].token_index;`, add:
```cpp
			delete segments[which].multivector_pq;
```

- [ ] **Step 2: Rebuild `.mvpq` after the `multivectors` refresh (`atire/atire_segment_index_compaction.cpp`)**

The V5 `multivectors` refresh block reloads `output_segment->multivectors` from the freshly written `.mvec`. Immediately AFTER that refresh, insert the `.mvpq` rebuild + refresh (train over the just-refreshed float pool). Placement relative to the `.tann` block does not matter (the `.tann` graph is built over `multivectors`, not `multivector_pq`):
```cpp
/*
	Token-PQ: rebuild the merged segment's .mvpq over the just-refreshed float
	.mvec token pool (retrain per-segment codebook + renumbered ragged codes),
	then refresh the in-memory multivector_pq.  Best-effort: a failure leaves the
	merged segment on the .mvec fallback.  (No cross-store ordering constraint:
	the .tann graph is built over multivectors, not multivector_pq, in Phase 2.)
*/
if (multivector_pq_configured() && output_segment->multivectors != NULL
	&& !output_segment->multivectors->tokens_quantized() && output_segment->multivectors->token_count() > 0)
	{
	char out_mvpq[4096];
	segment_filename(out_mvpq, sizeof(out_mvpq), output_generation, "mvpq");
	long long docs = output_segment->engine->get_document_count();
	ANT_multivector_store *mv = output_segment->multivectors;
	ANT_multivector_pq_store_writer w;
	long failed = w.create(out_mvpq, rerank_dimension_current, mvpq_m_current, ANT_pq_codec::METRIC_DOT) != 0;
	long long cap = mv->max_vector_count();
	float *buf = new float[(cap > 0 ? cap : 1) * rerank_dimension_current];
	for (long long d = 0; !failed && d < docs; d++)
		{ long long md = mv->copy_vectors(d, buf); failed = w.append(md > 0 ? buf : NULL, md) != 0; }
	delete [] buf;
	if (!failed) failed = w.finish() != 0;
	if (failed) w.abandon();
	delete output_segment->multivector_pq;
	output_segment->multivector_pq = ANT_multivector_pq_store::load(out_mvpq, rerank_dimension_current, docs, ANT_pq_codec::METRIC_DOT);
	}
else
	{
	delete output_segment->multivector_pq;
	output_segment->multivector_pq = NULL;
	}
```

The V6 `.tann` rebuild block is UNCHANGED (it still builds over `output_segment->multivectors`, the resident float pool) — do not touch it.

- [ ] **Step 3: Write the failing test `tests/test_mvpq_compaction.cpp`**

```cpp
/*
	TEST_MVPQ_COMPACTION.CPP -- compact() rebuilds the merged .mvpq (retrain +
	renumber); search_multivector stays correct; float fallback after .mvpq loss.
	Run under ASan detect_leaks=1 to guard the shuffle-teardown free of
	multivector_pq (the dense-PQ C1 leak lesson).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 16

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqc_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }
static void dv(long long i, float *v){ for(int d=0;d<DIM;d++) v[d]=0.01f*(float)(((i*3+d)%7)-3); v[i%DIM]+=5.0f; double n=0; for(int d=0;d<DIM;d++) n+=(double)v[d]*v[d]; n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }
static void add(ATIRE_segment_index *ix, long long from, long long to){ float t[DIM]; char k[32],b[64]; for(long long i=from;i<to;i++){ dv(i,t); sprintf(k,"doc-%lld",i); sprintf(b,"<DOC>term%lld q</DOC>",i); CHECK(ix->add_document(k,b,NULL,t,1)>=0);} }

int main(void)
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
add(ix, 0, 6); CHECK(ix->flush() == 0);
add(ix, 6, 12); CHECK(ix->flush() == 0);
CHECK(ix->disk_segment_count() == 2);
CHECK(ix->build_multivector_pq() == 0);
CHECK(ix->build_token_index() == 0);
CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
CHECK(ix->disk_segment_has_multivector_pq(1) == 1);

float q[DIM]; dv(7, q);
CHECK(ix->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "doc-7") == 0);

long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
CHECK(ix->compact(gens, 2) == 0);
CHECK(ix->disk_segment_count() == 1);
CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
CHECK(ix->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "doc-7") == 0);		/* renumbering correct */

long long out_gen = ix->disk_segment_generation(0);
delete ix;

char mvpq[4096]; snprintf(mvpq, sizeof(mvpq), "%s/seg_%06lld.mvpq", d, out_gen); remove(mvpq);
ATIRE_segment_index *re = new ATIRE_segment_index();
CHECK(re->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(re->open(d) == 0);
CHECK(re->disk_segment_has_multivector_pq(0) == 0);			/* no .mvpq -> float fallback */
CHECK(re->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(re->get_hit(0)->filename, "doc-7") == 0);		/* still correct via .mvec */
delete re; delete [] d;
printf("test_mvpq_compaction PASSED\n");
return 0;
}
```
(If the segment filename format differs from `seg_%06lld.mvpq`, `ls` the dir after the merge and match the actual name — as the V6/dense-PQ compaction tests do.)

- [ ] **Step 4: Run to verify it fails, then passes**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_compaction 2>&1 | tail -8 && ./bin/test_mvpq_compaction`
Expected: after implementing Steps 1–2, PASS — `test_mvpq_compaction PASSED`.

- [ ] **Step 5: Regression + ASan leak check**

Run: `for t in test_mvpq_search test_mvpq_rerank test_segment_index test_v6_compaction; do make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1; done`
Then ASan (rebuild instrumented, then clean afterward):
```
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_compaction CC='g++ -fsanitize=address,undefined -g -fno-omit-frame-pointer' 2>&1 | tail -2
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_mvpq_compaction 2>&1 | grep -iE "multivector_pq|pq_codec|token_index|Direct leak|Indirect leak|use-after-free|SUMMARY" | head -30
rm -f obj/*.o lib/libantelope_engine.a
```
Expected: `test_mvpq_compaction PASSED`; ASan shows NO `multivector_pq`/`pq_codec` leak and no use-after-free (only the known ~1MB-each `ANT_file::setvbuff` leaks may remain).

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index_compaction.cpp tests/test_mvpq_compaction.cpp
git commit -m "feat(mvpq): compaction retrain+renumber .mvpq (free multivector_pq in teardown)"
```

---

## Task 7: Recall sanity at default m + ASan/UBSan sweep

**Files:**
- Test: `tests/test_mvpq_recall.cpp`

- [ ] **Step 1: Write the recall test `tests/test_mvpq_recall.cpp`**

```cpp
/*
	TEST_MVPQ_RECALL.CPP -- recall sanity at the DEFAULT m (set_multivector_pq_config(0,...)):
	rerank posture recall@10 vs exact MaxSim >= 0.9 on 400 docs of dim-32 tokens.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 32
#define NDOCS 400

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqrec_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }
static unsigned long long R;
static double nf(void){ R=R*6364136223846793005ULL+1442695040888963407ULL; return (double)((R>>33)&0x7fffffff)/(double)0x7fffffff; }
static void tok(long long seed, float *v){ R=(unsigned long long)(seed+1)*2654435761ULL; double n=0; for(int d=0;d<DIM;d++){v[d]=(float)(nf()*2.0-1.0);n+=(double)v[d]*v[d];} n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }
static void fill(ATIRE_segment_index *ix){ float doc[3*DIM]; char k[32],b[64]; long long seed=0; for(long long i=0;i<NDOCS;i++){ long long md=(i%3)+1; for(long long t=0;t<md;t++) tok(seed++, doc+t*DIM); sprintf(k,"doc-%lld",i); sprintf(b,"<DOC>term%lld z</DOC>",i); CHECK(ix->add_document(k,b,NULL,doc,md)>=0);} CHECK(ix->flush()==0); }

static double recall(ATIRE_segment_index *cand, ATIRE_segment_index *ex, long long nq)
{ long long hit=0,tot=0; float q[3*DIM];
  for(long long qi=0;qi<nq;qi++){ long long m=(qi%3)+1; for(long long t=0;t<m;t++) tok(70000+qi*3+t, q+t*DIM);
    long long en=ex->search_multivector(q,m,10); long long e[10]; for(long long i=0;i<en;i++) e[i]=atoll(ex->get_hit(i)->filename+4);
    long long cn=cand->search_multivector(q,m,10); long long c[10]; for(long long i=0;i<cn;i++) c[i]=atoll(cand->get_hit(i)->filename+4);
    for(long long a=0;a<en;a++){ tot++; for(long long bb=0;bb<cn;bb++) if(e[a]==c[bb]){hit++;break;} } }
  return tot? (double)hit/(double)tot : 0.0; }

int main(void)
{
char *de = dir_(); ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0); CHECK(ex->open(de) == 0); fill(ex);
char *dr = dir_(); ATIRE_segment_index *rr = new ATIRE_segment_index();
CHECK(rr->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0); CHECK(rr->open(dr) == 0);
CHECK(rr->set_multivector_pq_config(0, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* default m */
CHECK(rr->multivector_pq_m() == 16);	/* default_pq_m(32) */
fill(rr); CHECK(rr->build_multivector_pq() == 0); CHECK(rr->build_token_index() == 0);
double r = recall(rr, ex, 40);
printf("  default m=%lld  rerank recall@10=%.3f\n", (long long)rr->multivector_pq_m(), r);
CHECK(r >= 0.9);
delete ex; delete rr; delete [] de; delete [] dr;
printf("test_mvpq_recall PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run recall**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_mvpq_recall 2>&1 | tail -5 && ./bin/test_mvpq_recall`
Expected: prints the recall line and `test_mvpq_recall PASSED`. If `r < 0.9` at default m=16, first confirm `build_multivector_pq`/`build_token_index` ran; the legitimate fix is widening the rerank candidate pool (Task 5 Step 3) — never lower the 0.9 threshold. Report the pool multiplier if changed.

- [ ] **Step 3: ASan/UBSan sweep over all mvpq paths**

```
rm -f obj/*.o lib/libantelope_engine.a bin/test_mvpq_*
for t in test_mvpq_store test_mvpq_config test_mvpq_backfill test_mvpq_search test_mvpq_rerank test_mvpq_compaction test_mvpq_recall; do
  make $t CC='g++ -fsanitize=address,undefined -g -fno-omit-frame-pointer' 2>&1 | tail -2
done
for t in test_mvpq_store test_mvpq_config test_mvpq_backfill test_mvpq_search test_mvpq_rerank test_mvpq_compaction test_mvpq_recall; do
  echo "=== $t ==="; ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./bin/$t 2>&1 | tail -20
done
```
Triage: fix any ASan/UBSan finding attributable to `multivector_pq_store.*`, `pq_codec.*`, or the mvpq blocks in the segment index (heap-overflow, UAF, uninitialized value, misalignment). Pre-existing legacy-lexical misalignment in `source/memory_index_one*` / packed-posting casts is OUT OF SCOPE (same as prior sweeps). Rebuild clean afterward: `rm -f obj/*.o lib/libantelope_engine.a`.

- [ ] **Step 4: Full suite on a clean (non-ASan) build**

```
rm -f obj/*.o lib/libantelope_engine.a bin/test_*
for t in test_mvpq_store test_mvpq_config test_mvpq_backfill test_mvpq_search test_mvpq_rerank test_mvpq_compaction test_mvpq_recall test_segment_index test_v6_compaction test_v6_recall test_pq_recall; do
  make $t >/dev/null 2>&1 && printf "%-24s %s\n" "$t" "$(./bin/$t 2>&1 | tail -1)" || printf "%-24s BUILD-FAIL\n" "$t"
done
```
Expected: all PASSED (the trailing V6/dense-PQ regressions prove no cross-feature breakage).

- [ ] **Step 5: Commit**

```bash
git add tests/test_mvpq_recall.cpp
# plus any mvpq source file fixed during the sweep
git commit -m "test(mvpq): recall sanity at default m + ASan/UBSan sweep"
```

---

## After Task 7

Dispatch a holistic review over the whole Phase-2 diff (`git diff master...HEAD`) focusing on: `.mvpq` load validation (validate-before-allocate, exact-size, `sum(counts)==total_tokens`, all buffers freed on every error branch), ADC-MaxSim correctness (per-query-token tables built once; max-per-query-token summed; L2 sign), `multivector_pq` lifetime (**free at every teardown site incl. the compaction shuffle** — the dense-PQ C1 leak lesson; targeted `detect_leaks=1` on the compaction test), replace vs rerank correctness, V5/V6-unconfigured byte-identicalness (incl. the `.tann`/token-graph path untouched), `.mvec`-int8 vs `.mvpq`-PQ mutual exclusion + config immutability/persistence. Fix Critical/Important with regression tests. Then finishing-a-development-branch — verify the full suite green on a clean build (`rm -f obj/*.o lib/libantelope_engine.a && make all && <run mvpq + regression tests>`) before merging locally to master.

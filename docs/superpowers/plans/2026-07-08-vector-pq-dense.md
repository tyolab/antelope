# Product Quantization (PQ) — Dense Vectors, Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A per-segment product-quantization compression tier for the dense per-document vectors — a higher-compression sibling to V4 int8 — usable as a flat ADC scan and (via the V6 `ANT_vector_source` seam) an HNSW backend, with configurable replace / shortlist-rerank query postures.

**Architecture:** A pure PQ codec (`pq_codec`) does deterministic per-subspace k-means + encode + ADC + reconstruct. `ANT_pq_store` wraps a per-segment `.pq` sidecar and implements `ANT_vector_source`. A parallel `vector_candidates_pq` gatherer (selected only when PQ is configured, leaving the float/int8 paths byte-identical) serves the replace posture directly and the rerank posture by rescoring a PQ shortlist with the retained exact float/int8 tier.

**Tech Stack:** C++ (`source/`, `atire/`), reusing `ANT_vector_source`/`ANT_hnsw` (V3/V6), the per-segment sidecar + compaction + backfill machinery, and the existing `exact_vectors` rerank tier.

**Spec:** `docs/superpowers/specs/2026-07-08-vector-pq-dense-design.md`

**Milestones:** codec unit-tested after Task 1; `ANT_pq_store` load/source after Task 3; PQ search (replace + HNSW) after Task 5; rerank posture + config after Task 6; backfill + eager after Task 7; compaction after Task 8; recall + sanitizer after Task 9. Phase 1 is a shippable milestone; token-pool PQ (Phase 2) is a separate later spec.

---

## Repo facts every task needs

- **Build:** `make all` then `make engine_lib`. **NO header dependency tracking** — after editing ANY `.h`, `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding or you link a stale/inconsistent archive (has caused runtime SEGVs). Tasks touching a header MUST clear objs.
- **Tests:** `tests/*.cpp` auto-discovered (a new `source/*.cpp` is auto-linked too — no makefile edit). Build+run one: `make <name> && ./bin/<name>`. Convention: `#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)`, a `main()` that runs the tests and prints a PASS line, exit 0 on success. Mirror `tests/test_hnsw.cpp`.
- **ASan/UBSan:** no make target; use `make all engine_lib <tests> CC='g++ -fsanitize=address,undefined -g'` (CC is a plain assignment, so this applies to compile+link and keeps the makefile's `-ldl -lpthread -lz …`), run with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1`. The pre-existing `ANT_file::setvbuff` leak + legacy-lexical misaligned-pointer UB are OUT OF SCOPE.
- **V4 config (extend, don't break):** `enum { QUANTIZE_OFF=0, QUANTIZE_REPLACE=1, QUANTIZE_EXACT=2 }`, `enum { RERANK_QUANT_FLOAT=0, RERANK_QUANT_INT8=1 }`, `long quantization_current`, `set_quantization(mode)` (immutable, persists `quantization.config`), `build_quantized()`, per-segment `ANT_vector_store *vectors` (`.qvec` int8 or `.vec` float) + `ANT_vector_store *exact_vectors` (float, for exact-mode rerank).
- **V6 seam:** `ANT_vector_source` (abstract; `document_count/get_dimension/has/get/is_quantized/reconstruct/score`). `ANT_hnsw::build/search` take `ANT_vector_source*`; the int8 branch already uses `reconstruct()` and only calls `get()` when `!is_quantized()` — so a PQ source returning `get()==NULL` under `is_quantized()==true` is safe.
- **Search publish path:** `reset_results()` → gather `ANT_vector_candidate best[top_k]` via `ANT_vector_candidate_insert(best,&count,top_k,score,generation,docid)` → `qsort(best,count,sizeof(*best),vector_candidate_compare)` → per candidate `resolve_hit_filename`/`append_result`/`populate_hit_payload`/score/filename (see `search_vector_impl` in `atire/atire_segment_index_vector.cpp`). Hits read via `get_hit(i)->{filename,generation,docid,score}`.
- **Sidecars:** per segment `seg_<G>.vec/.qvec/.vsig/.hnsw/.mvec/.tann`. PQ adds `seg_<G>.pq`. `segment_filename(buf,sizeof,generation,"pq")`. Forgiving load (corrupt ⇒ empty ⇒ fallback). Config setters are POST-open. Repo-wide `-fPIC`.

---

## Task 1: PQ codec (`pq_codec`) — deterministic k-means, encode, ADC, reconstruct

**Files:** Create `source/pq_codec.h`, `source/pq_codec.cpp`; Test `tests/test_pq_codec.cpp`.

- [ ] **Step 1: Write `source/pq_codec.h`** — pure static codec (no I/O, no engine deps):

```cpp
/*
	PQ_CODEC.H -- product-quantization primitives (Phase 1). D-dim vectors split
	into m subvectors of D/m dims; per subspace a k=256 k-means codebook; a
	vector -> m code bytes. Deterministic (fixed seed + iters) so a rebuild is
	byte-identical. Pure/stateless; no file or engine dependencies.
*/
#ifndef PQ_CODEC_H_
#define PQ_CODEC_H_

class ANT_pq_codec
{
public:
	enum { K = 256, KMEANS_ITERS = 25 };
	enum { METRIC_DOT = 0, METRIC_COSINE = 1, METRIC_L2 = 2 };	// mirrors ANT_vector_store metrics

	// m must divide dimension (else returns 1, no output). Trains m codebooks by
	// k-means over `n` present vectors (row-major vectors[n*dimension]); writes
	// codebook[m*K*(dimension/m)] (subspace s, centroid c, dim d at
	// [((s*K)+c)*(dimension/m)+d]). Deterministic. 0 on success.
	static long train(const float *vectors, long long dimension, long long m, long long n, float *codebook);

	// Encode one vector -> m code bytes (nearest centroid per subspace, L2 in subspace).
	static void encode(const float *vector, long long dimension, long long m, const float *codebook, unsigned char *codes);

	// Build the per-query ADC table[m*K]: table[s*K+c] = contribution of subspace s,
	// centroid c to the kernel between `query` and any doc whose subspace-s code is c.
	// For DOT/COSINE: dot(query_subvec_s, centroid). For L2: -||query_subvec_s - centroid||^2.
	// A doc's kernel = sum_s table[s*K + codes[s]] (higher = better for all metrics).
	static void adc_table(const float *query, long long dimension, long long m, const float *codebook, long metric, double *table);

	// Kernel of a doc's codes against a prebuilt adc table: sum_s table[s*K+codes[s]].
	static double adc_score(const unsigned char *codes, long long m, const double *table);

	// Reconstruct approximate float vector from codes (concatenate the chosen centroids).
	static void reconstruct(const unsigned char *codes, long long dimension, long long m, const float *codebook, float *out);
};
#endif /* PQ_CODEC_H_ */
```

- [ ] **Step 2: Failing test `tests/test_pq_codec.cpp`** (run `make test_pq_codec` → link error first):

```cpp
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../source/pq_codec.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)

static void test_m_must_divide(void)
{
	float cb[1]; float v[6] = {0};
	CHECK(ANT_pq_codec::train(v, 6, 4, 1, cb) == 1);	// 4 does not divide 6
}

static void test_determinism_and_recall(void)
{
	long long dim = 16, m = 4, n = 200, i;
	float *data = new float[n*dim];
	srand(7);
	for (i = 0; i < n*dim; i++) data[i] = (float)(rand()%200-100)/100.0f;
	long long cbsize = m * ANT_pq_codec::K * (dim/m);
	float *cb1 = new float[cbsize], *cb2 = new float[cbsize];
	CHECK(ANT_pq_codec::train(data, dim, m, n, cb1) == 0);
	CHECK(ANT_pq_codec::train(data, dim, m, n, cb2) == 0);
	CHECK(memcmp(cb1, cb2, cbsize*sizeof(float)) == 0);		// deterministic

	// encode -> reconstruct error is bounded; ADC score ~= reconstruct-then-dot
	unsigned char codes[4];
	double table[4*256];
	float q[16]; for (i=0;i<dim;i++) q[i]=(float)(rand()%200-100)/100.0f;
	ANT_pq_codec::adc_table(q, dim, m, cb1, ANT_pq_codec::METRIC_DOT, table);
	double maxerr = 0;
	for (long long d = 0; d < n; d++)
		{
		ANT_pq_codec::encode(data+d*dim, dim, m, cb1, codes);
		float recon[16]; ANT_pq_codec::reconstruct(codes, dim, m, cb1, recon);
		double adc = ANT_pq_codec::adc_score(codes, m, table);
		double rdot = 0; for (i=0;i<dim;i++) rdot += (double)q[i]*recon[i];
		double e = fabs(adc - rdot); if (e > maxerr) maxerr = e;
		}
	CHECK(maxerr < 1e-3);		// ADC == reconstruct-then-dot (both read the same centroids)
	delete[] data; delete[] cb1; delete[] cb2;
}

static void test_degenerate_subspace(void)
{
	long long dim = 4, m = 2, n = 10, i;
	float *data = new float[n*dim];
	for (i = 0; i < n*dim; i++) data[i] = 3.0f;	// all identical -> single distinct centroid
	long long cbsize = m*ANT_pq_codec::K*(dim/m);
	float *cb = new float[cbsize];
	CHECK(ANT_pq_codec::train(data, dim, m, n, cb) == 0);
	unsigned char codes[2]; float recon[4];
	ANT_pq_codec::encode(data, dim, m, cb, codes);
	ANT_pq_codec::reconstruct(codes, dim, m, cb, recon);
	for (i=0;i<dim;i++) CHECK(fabs(recon[i]-3.0f) < 1e-4);
	delete[] data; delete[] cb;
}

int main(void)
{
	test_m_must_divide();
	test_determinism_and_recall();
	test_degenerate_subspace();
	printf("test_pq_codec PASSED\n");
	return 0;
}
```

- [ ] **Step 3: Implement `source/pq_codec.cpp`** — `train`: per subspace, k-means with a **fixed seed** (deterministic init: pick the first K distinct subvectors, or k-means++ with a fixed RNG seed — the plan mandates a fixed `ANT_mersenne_twister` seed like `ANT_HNSW_SEED` for reproducibility), `KMEANS_ITERS` Lloyd iterations, assign by min L2 in the subspace, update centroids as cluster means, deterministic tie-break (lowest centroid index). Fewer than K distinct points → leave extra centroids as duplicates of existing (encode still deterministic). `encode`: per subspace nearest centroid (L2). `adc_table`: per subspace per centroid, DOT/COSINE → dot(query_subvec, centroid), L2 → `-sum((q-c)^2)`. `adc_score`: sum lookups. `reconstruct`: copy chosen centroids into `out`. Return 1 from `train` if `m<1 || dimension%m != 0`.

- [ ] **Step 4: Run `make test_pq_codec && ./bin/test_pq_codec`** → `PASSED`.

- [ ] **Step 5: Commit** — `git add source/pq_codec.h source/pq_codec.cpp tests/test_pq_codec.cpp && git commit -m "feat(pq): deterministic PQ codec (k-means, encode, ADC, reconstruct)"`.

---

## Task 2: `ANT_pq_store` — `.pq` sidecar save + forgiving load

**Files:** Create `source/pq_store.h`, `source/pq_store.cpp`; Test `tests/test_pq_store.cpp`.

- [ ] **Step 1: `source/pq_store.h`** — the store + writer:

```cpp
/*
	PQ_STORE.H -- per-segment PQ-compressed dense vectors (seg_G.pq). Implements
	ANT_vector_source so ANT_hnsw and the ADC scan share one backend. Forgiving
	load: any validation failure -> degraded empty store (fallback to float/int8).
*/
#ifndef PQ_STORE_H_
#define PQ_STORE_H_
#include "vector_source.h"

class ANT_pq_store : public ANT_vector_source
{
private:
	long long dimension, documents, m;
	long metric;
	unsigned char *presence;	// (documents+7)/8, NULL when empty
	float *codebook;			// m*K*(dimension/m), NULL when empty
	unsigned char *codes;		// documents*m, NULL when empty
	ANT_pq_store();
public:
	~ANT_pq_store();
	static ANT_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric);
	long long get_m(void) { return m; }
	// ANT_vector_source:
	long long document_count(void) override { return documents; }
	long long get_dimension(void) override { return dimension; }
	long has(long long docid) override { return presence != NULL && docid >= 0 && docid < documents && (presence[docid/8] & (1 << (docid%8))) != 0; }
	const float *get(long long docid) override { (void)docid; return 0; }		// PQ: no zero-copy float; callers use reconstruct()
	long is_quantized(void) override { return 1; }
	void reconstruct(long long docid, float *out) override;						// codes[docid] -> approx float
	double score(long long docid, const float *query, long metric) override;	// exact ADC: builds a table each call (Task 3 adds a fast path)
	// scan-side helpers (Task 3):
	const unsigned char *codes_for(long long docid) { return has(docid) ? codes + docid*m : 0; }
	const float *get_codebook(void) { return codebook; }
};

class ANT_pq_store_writer
{
private:
	char *filename; long long dimension, m; long metric;
	float *buffer; long long capacity, documents; unsigned char *presence;
public:
	ANT_pq_store_writer(); ~ANT_pq_store_writer();
	long create(const char *path, long long dim, long long m, long metric);	// 0 ok
	long append(const float *vector_or_null);		// buffers one doc (NULL => absent row)
	long finish(void);								// trains codebook over present vecs, encodes, writes .pq atomically; 0 ok
	void abandon(void);
};
#endif /* PQ_STORE_H_ */
```

- [ ] **Step 2: Failing test `tests/test_pq_store.cpp`** — write 3 present + 1 absent doc via the writer, `finish()`, `ANT_pq_store::load(path, dim, 4, DOT)`; assert `document_count()==4`, `has(0)&&!has(3)`, `get_m()==m`, `reconstruct(0,out)` ≈ the original within PQ tolerance. Forgiving cases: nonexistent path → `document_count()==0`; truncate the file → empty; wrong `expected_dimension` → empty; a `.pq` written with m=4 loaded with expected doc-count mismatch → empty. (Mirror `tests/test_multivector_store.cpp` / your prior `.tann` load tests for the writer→load helper + corruption idioms.)

- [ ] **Step 3: Implement `source/pq_store.cpp`** — writer `finish()`: build a dense `float buffer[documents*dim]` (absent rows zeroed, tracked in `presence`), gather the present rows, `ANT_pq_codec::train(present, dim, m, present_count, codebook)`, encode every present doc, write atomically (`.tmp`+rename): header `magic "ANTPQ001"` (u64 via memcpy of 8 chars), `version` u32=1, `dimension`/`documents`/`m`/`k=256` i64, presence bitmap, codebook floats, codes bytes. `load()`: **validate before allocate** — magic/version/dim==expected/documents==expected/m in [1,dimension] and `dimension%m==0`/k==256/exact file size = `44 + presence_bytes + m*256*(dim/m)*4 + documents*m`; then read; **content check** every code byte < 256 (trivially true for u8, but validate `m>0`) and presence within bounds; any failure → the pre-made empty store. `reconstruct`/`score` delegate to `ANT_pq_codec`. (`score()` here builds an ADC table per call — Task 3 adds the batched fast path; correctness first.)

- [ ] **Step 4: `make test_pq_store && ./bin/test_pq_store`** → PASS. Rebuild engine (`rm obj/*.o lib/libantelope_engine.a && make all && make engine_lib`) and run `test_hnsw` → still PASS (new source auto-links, no existing behavior touched).

- [ ] **Step 5: Commit** — `feat(pq): ANT_pq_store .pq sidecar + forgiving load + ANT_vector_source`.

---

## Task 3: ADC scan path + PQ-backed HNSW smoke

**Files:** Modify `source/pq_store.{h,cpp}` (batched ADC scan helper); Test `tests/test_pq_store.cpp` (extend) + `tests/test_pq_hnsw.cpp`.

- [ ] **Step 1: Add a batched ADC scan** to `ANT_pq_store` so a query builds the `m*K` table once, then scores all docs — the hot path the gatherer uses:

```cpp
// Fill best[top_k] with the top-k docids by ADC kernel (higher=better), honoring
// presence, tombstones, and an optional docid filter bitset. generation tags rows.
void scan_adc(const float *query, long metric, ANT_index_tombstones *tombstones, long long generation,
	ANT_vector_candidate *best, long long *best_count, long long top_k, const unsigned char *filter_bits);
```
Implement with one `ANT_pq_codec::adc_table` then a loop over docs: skip `!has`, tombstoned, or filtered; `ANT_pq_codec::adc_score(codes_for(docid), m, table)` → `ANT_vector_candidate_insert`. (`#include "index_tombstones.h"`, `vector_store.h` for `ANT_vector_candidate`.)

- [ ] **Step 2: Failing test** — (a) extend `test_pq_store.cpp`: `scan_adc` top-k over a small set equals a brute-force ADC ranking; filter bitset + a tombstone exclude the right docs. (b) `tests/test_pq_hnsw.cpp`: build an `ANT_hnsw` over an `ANT_pq_store` (as an `ANT_vector_source`), search, and assert recall@10 vs an exact float ranking ≥ 0.8 on a synthetic set — proves the PQ source drives HNSW (the graph calls `reconstruct`/`score`, never `get()`).

- [ ] **Step 3: Implement** `scan_adc`; the HNSW smoke needs no product code (the V6 `ANT_vector_source` retarget already lets `ANT_hnsw::build(pq_store, …)` work). If `test_pq_hnsw` reveals the graph calling `get()` on the PQ source, that is a bug in the caller guard — but per V6 review the int8 branch only calls `get()` under `!is_quantized()`; confirm and, if needed, note it.

- [ ] **Step 4: `make test_pq_store test_pq_hnsw && ./bin/test_pq_store && ./bin/test_pq_hnsw`** → PASS. `test_hnsw` still PASS.

- [ ] **Step 5: Commit** — `feat(pq): batched ADC scan + PQ-backed HNSW`.

---

## Task 4: PQ config (`set_pq_config`) + persistence + mutual exclusion with int8

**Files:** Modify `atire/atire_segment_index.h`, `atire/atire_segment_index.cpp`; Test `tests/test_pq_config.cpp`.

- [ ] **Step 1: Header** — add PQ posture enum + members + setter:

```cpp
enum { PQ_POSTURE_REPLACE = 0, PQ_POSTURE_RERANK = 1 };
// members:
long long pq_m_current;          // 0 = PQ off
long pq_posture_current;         // PQ_POSTURE_REPLACE / PQ_POSTURE_RERANK
long pq_rerank_quant_current;    // RERANK_QUANT_FLOAT / RERANK_QUANT_INT8 (rerank posture only)
long pq_eager;                   // 0 = ondemand (default), 1 = eager
// public:
long set_pq_config(long long m, long posture, long rerank_quant);  // 0 ok; nonzero on: vectors unconfigured, m<1 || dimension%m!=0, int8 quantization already set (mutually exclusive), or already set to a DIFFERENT config (immutable). m==0 with a sentinel selects the default rule.
long pq_configured(void) { return pq_m_current != 0; }
long set_pq_policy(long eager);           // 1 eager / 0 ondemand; returns 0
long build_pq(void);                       // Task 7
```

- [ ] **Step 2: Failing test `tests/test_pq_config.cpp`** — open an index with dim=16 vectors; `set_pq_config(4, PQ_POSTURE_REPLACE, RERANK_QUANT_FLOAT)` → 0; re-setting the SAME config → 0 (idempotent); a DIFFERENT m → nonzero (immutable); `set_quantization(QUANTIZE_REPLACE)` after PQ is set → nonzero (mutual exclusion), and vice-versa; `set_pq_config(3, …)` (3 ∤ 16) → nonzero; `set_pq_config` before vectors configured → nonzero. Reopen the index → `pq_configured()` true and `pq_m_current`/posture restored from `pq.config`.

- [ ] **Step 3: Implement** — `set_pq_config`: guard `vector_dimension_current != 0`; **default rule** when `m==0`: `m = default_pq_m(dimension)` = the largest divisor of `dimension` that is `<= 16` (so `min(16,D)` rounded down to a divisor; e.g. D=16→16? no: pick largest divisor ≤16 → 16 divides 16 →16; D=128→16; D=100→ divisors ≤16: 10; D=17 (prime)→1). Reject if int8 `quantization_current != QUANTIZE_OFF`. Immutable: if `pq_m_current != 0` and args differ → nonzero. Persist `pq.config` (magic, m, posture, rerank_quant) atomically; load it in the open path (mirror `load`/`save` of `quantization.config`). Make `set_quantization` reject when `pq_configured()` (add the reciprocal guard). Init the members in the constructor (`pq_m_current=0; pq_posture_current=0; pq_rerank_quant_current=0; pq_eager=0;`).

- [ ] **Step 4: `rm obj/*.o lib/libantelope_engine.a && make all && make engine_lib && make test_pq_config && ./bin/test_pq_config`** → PASS. `test_segment_index` → still PASS (int8/quantization config unchanged when PQ unused).

- [ ] **Step 5: Commit** — `feat(pq): set_pq_config + pq.config persistence + int8 mutual exclusion`.

---

## Task 5: Segment load `.pq` + `vector_candidates_pq` gatherer + replace posture search

**Files:** Modify `atire/atire_segment_index.h` (per-segment `pq_vectors` field; gatherer decl), `atire/atire_segment_index.cpp` (segment load + teardown), `atire/atire_segment_index_vector.cpp` (gatherer + search dispatch); Test `tests/test_pq_search.cpp`.

- [ ] **Step 1: Header** — in `struct segment` add `ANT_pq_store *pq_vectors;` (next to `vectors`/`exact_vectors`; `class ANT_pq_store;` forward decl). Declare `long long vector_candidates_pq(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter);`.

- [ ] **Step 2: Segment load + teardown** (`atire/atire_segment_index.cpp`) — in the segment-open block, when `pq_configured()`: `segment_filename(pqname,…,generation,"pq"); segments[sc].pq_vectors = ANT_pq_store::load(pqname, vector_dimension_current, docs, vector_metric);` else NULL. In REPLACE posture the float `.vec` may be absent — keep loading `.vec`/`.qvec` into `.vectors` as today for the rerank tier / fallback (a degraded `.pq` must fall back to `.vectors`). Add `delete segments[which].pq_vectors;` next to every `delete segments[which].vectors;` teardown site (destructor + compaction). `#include "../source/pq_store.h"`.

- [ ] **Step 3: Failing test `tests/test_pq_search.cpp`** — index with dim=16 vectors, `set_pq_config(4, PQ_POSTURE_REPLACE, FLOAT)` after open, add ~30 docs with known dense vectors, `flush()`, `build_pq()` (Task 7 — for THIS task drive the search after a manual build; if `build_pq` isn't in yet, gate this test on it OR write it to run after Task 7 lands). Assert `search_vector(q, 10)` returns a sane approximate top-10 whose #1 is the true nearest (planted). Also assert that with PQ **unconfigured**, `search_vector` is byte-identical to today (fixed query, identical top-k gen/docid/score vs a non-PQ index) — the regression lock. (Sequencing note: this task's search assertions depend on `build_pq` from Task 7; implement the gatherer + dispatch here, and land the full search assertions once Task 7 is in — or reorder Task 7 before this task's Step 4. The plan's executor should build Task 7 immediately after Task 5's gatherer compiles.)

- [ ] **Step 4: Implement** — `vector_candidates_pq` (mirror `vector_candidates_hnsw`'s structure): for each segment with `pq_vectors != NULL && pq_vectors->document_count()>0`: build the per-segment filter bitset (`evaluate_filter_for_segment`), then `pq_vectors->scan_adc(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits)` (or, when a PQ-backed `.hnsw` exists, navigate it — defer HNSW-over-PQ wiring to a follow-up within this task only if cheap; ADC scan is the required path). `delete [] fbits`. Then live buffer via `scan_live_buffer_exact` (float, exact) + `evaluate_filter_for_live`. In `search_vector_impl`, dispatch to `vector_candidates_pq` when `pq_configured() && pq_posture_current == PQ_POSTURE_REPLACE`; all non-PQ modes unchanged (byte-identical). The publish tail is the shared one.

- [ ] **Step 5: `rm obj/*.o … && make … && make test_pq_search test_segment_index && ./bin/test_pq_search && ./bin/test_segment_index`** → PASS (PQ approximate top-k sane; non-PQ byte-identical).

- [ ] **Step 6: Commit** — `feat(pq): .pq segment load + vector_candidates_pq + replace-posture search`.

---

## Task 6: Rerank posture (PQ shortlist → exact rescore)

**Files:** Modify `atire/atire_segment_index_vector.cpp`; Test `tests/test_pq_search.cpp` (extend).

- [ ] **Step 1: Failing test** — index with `set_pq_config(4, PQ_POSTURE_RERANK, RERANK_QUANT_FLOAT)`, dim=16, ~50 docs, `flush()`, `build_pq()`. Assert `search_vector(q, 10)` **recall@10 vs exact float ≥ the replace posture's recall AND ≥ 0.9** (rerank restores precision). Assert the retained exact float tier is used (a doc whose PQ score mis-ranks it but whose exact score is top-1 lands #1). Add a `RERANK_QUANT_INT8` variant (rescore via the `.qvec`/int8 exact tier) asserting recall ≥ threshold.

- [ ] **Step 2: Implement** — in `search_vector_impl` (or a `search_vector_pq_rerank` helper), when `pq_configured() && pq_posture_current == PQ_POSTURE_RERANK`: gather a shortlist of `top_k * candidate_multiplier` via `vector_candidates_pq`, then **rescore** each shortlisted candidate with the exact tier — `segments[which].exact_vectors` (float) when `pq_rerank_quant_current==RERANK_QUANT_FLOAT`, else the int8 `segments[which].vectors` — replacing the ADC score with the exact `src->score(docid, query, vector_metric)`, then re-sort and publish top_k. Mirror the V6 `search_multivector` candidate→rescore→publish shape. Ensure the exact tier is loaded in RERANK posture (segment open must retain `exact_vectors` float — set the same way `QUANTIZE_EXACT` does; the plan wires RERANK posture to load `exact_vectors` from `.vec`).

- [ ] **Step 3: `make … && ./bin/test_pq_search` → PASS; `test_segment_index` → PASS.**

- [ ] **Step 4: Commit** — `feat(pq): rerank posture (PQ shortlist -> exact float/int8 rescore)`.

---

## Task 7: `build_pq()` backfill + eager/ondemand policy

**Files:** Modify `atire/atire_segment_index_vector.cpp` (build_pq), `atire/atire_segment_index.cpp` (eager at flush, set_pq_policy); Test `tests/test_pq_build.cpp`.

- [ ] **Step 1: Failing test `tests/test_pq_build.cpp`** — (a) ondemand: after `flush()`, `segments[0].pq_vectors` is absent (add a test accessor `long disk_segment_has_pq(long long which)` returning `pq_vectors != NULL && pq_vectors->document_count()>0`) → 0; `build_pq()` → 0 and the accessor → 1; `search_vector` now uses the PQ path and returns the same top-k as an exact reference on a full-recall set. (b) eager: `set_pq_policy(1)` before adds → `flush()` yields `disk_segment_has_pq(0)==1` with no explicit backfill. (c) `build_pq()` with PQ unconfigured → nonzero no-op.

- [ ] **Step 2: Implement `build_pq()`** (mirror `build_quantized()`): `if (!pq_configured() || vector_dimension_current==0) return 1;` per disk segment with a `.vec`/`.vectors` and no valid `.pq`: reconstruct/gather the segment's dense float vectors (reload the float `.vec`, like `build_hnsw` reloads it), `ANT_pq_store_writer` create(pqname, dim, pq_m_current, vector_metric) → append each doc (NULL for absent) → finish(), then load the fresh `.pq` into `segments[which].pq_vectors` (swap, delete old). Best-effort per segment. Eager: at the end of `flush()` (after the new segment is registered, same spot V6 put the token-index eager hook), `if (pq_eager) build_pq();`. `set_pq_policy` sets the flag. Add `disk_segment_has_pq`.

- [ ] **Step 3: `rm obj/*.o … && make … && make test_pq_build && ./bin/test_pq_build` → PASS; guards (`test_pq_search`, `test_segment_index`) PASS.**

- [ ] **Step 4: Commit** — `feat(pq): build_pq backfill + eager/ondemand policy`.

---

## Task 8: Compaction rebuilds `.pq` (retrain + renumber)

**Files:** Modify `atire/atire_segment_index_compaction.cpp`; Test `tests/test_pq_compaction.cpp`.

- [ ] **Step 1: Failing test `tests/test_pq_compaction.cpp`** — PQ configured, docs across 2 flushes, `build_pq()`, capture `search_vector(q,10)` baseline, `compact(gens,2)`, assert the merged index answers `search_vector(q,10)` with the same top-k (renumbered codes correct) and `disk_segment_has_pq(0)==1` (rebuilt). Fallback case: delete the merged `.pq`, reopen, assert `search_vector` still correct via float/int8 fallback.

- [ ] **Step 2: Implement** — after the merged `.vec` is written and the merged segment registered (`output_segment`), rebuild `.pq` over the merged floats: reload the merged `.vec`, `ANT_pq_store_writer` over the renumbered surviving docs (mirror the `.mvec`/`.hnsw`/`.tann` rebuild blocks' renumbering discipline), save, and **refresh `output_segment->pq_vectors` AFTER the store is finalized** (V6 borrowed-store lesson: don't leave a stale in-memory pointer; delete the old, load the fresh `.pq`). Best-effort, non-fatal (a failed `.pq` leaves the merged segment float/int8). Add `delete segments[which].pq_vectors;` at the compaction teardown site if not already added in Task 5.

- [ ] **Step 3: `make … && ./bin/test_pq_compaction` → PASS; `test_segment_index` (its compaction/maintain tests) → PASS.**

- [ ] **Step 4: Commit** — `feat(pq): compaction retrains + renumbers the .pq sidecar`.

---

## Task 9: Recall sanity + ASan/UBSan

**Files:** Test `tests/test_pq_recall.cpp`; possibly tune the default-`m` rule in `atire/atire_segment_index.cpp`.

- [ ] **Step 1: Recall test** — 500 docs, dim 32, random unit vectors + 3 planted near-query docs; PQ rerank posture at the DEFAULT m (via `set_pq_config(0,…)`); `build_pq()`; recall@10 of `search_vector` vs exact float ≥ 0.9; all 3 planted docs recalled; the replace posture recall is also measured and reported (expected lower). If rerank recall < 0.9 at default m, raise the default-m rule (larger m = finer codebook) until it passes, record the choice.

- [ ] **Step 2: Run → observe recall, tune default-m if needed, rerun → PASS.**

- [ ] **Step 3: ASan/UBSan sweep** — `rm obj/*.o lib/libantelope_engine.a; make all engine_lib test_pq_codec test_pq_store test_pq_hnsw test_pq_search test_pq_build test_pq_compaction test_pq_recall CC='g++ -fsanitize=address,undefined -g'`; run each with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1`. Expect no ASan/UBSan ERROR on PQ paths (codebook/codes/ADC-table allocs, load validation, HNSW-over-PQ, compaction rebuild). Fix any V6-style borrowed-store / OOB / leak in PQ code (the known setvbuff leak + legacy-lexical UB are out of scope). Restore the normal build (`rm obj/*.o lib/libantelope_engine.a && make all && make engine_lib`) and re-run `test_segment_index` green.

- [ ] **Step 4: Commit** — `test(pq): recall sanity + tuned default m + ASan/UBSan clean`.

---

## Final review + finish

After Task 9: dispatch a holistic review over the whole PQ diff (`git diff master...HEAD`) focusing on: `.pq` load validation (validate-before-allocate, exact-size, code/presence bounds, all buffers freed on every error branch), codec determinism + `m∤D` guard, the borrowed-`pq_vectors` lifetime across teardown/build_pq-swap/compaction-refresh (the V6 UAF class), replace vs rerank correctness, the exact-tier retention in rerank posture, PQ-unconfigured byte-identicalness, int8/PQ mutual-exclusion + config immutability/persistence, and no leaks at the `.pq` lifecycle sites. Fix Critical/Important with regression tests. Then finishing-a-development-branch — verify the full suite green on a clean build (`rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && <run tests>`) before merging locally to master.

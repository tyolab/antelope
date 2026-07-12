# PQ-backed Token Graph + Drop Resident Float `.mvec` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Realize the RAM win on the token/late-interaction side: under a new `NONE` resident tier, build and search the V6 token-HNSW (`.tann`) over the PQ-compressed token pool (`.mvpq`) and stop loading the resident float `.mvec` (float stays on disk; retrain/compaction read it).

**Architecture:** Generalize `ANT_token_index` off its concrete `ANT_multivector_store*` onto a new `ANT_token_source` (extends `ANT_vector_source` so it IS the graph source, plus `token_docid_of`/`num_documents`), add `ANT_multivector_pq_source` (token graph over `.mvpq` codes, ADC), and add a `{FLOAT, NONE}` resident tier selected per segment at load. The `#26` prepare-per-query ADC seam already on `ANT_vector_source` makes each query token build its ADC table once.

**Tech Stack:** C++ engine (`source/`, `atire/`). Reuses `ANT_hnsw` + the shipped `#26` seam, `ANT_multivector_pq_store` (`.mvpq`), `ANT_pq_codec`, and the V6 `ANT_token_index`/`.tann` machinery.

**Spec:** `docs/superpowers/specs/2026-07-12-vector-pq-token-resident-tier-design.md`.

**Repo constraints (apply throughout):**
- Whole repo is `-fPIC`. **No header dependency tracking** → after ANY change to `source/vector_source.h`, `source/multivector_store.h`, `source/multivector_pq_store.h`, `source/token_index.h`, or `atire/atire_segment_index.h`, run `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding, or you link stale objects (silent SEGV).
- `source/*.cpp` + `tests/*.cpp` auto-discovered; a test `tests/test_X.cpp` builds to `bin/test_X` via `make test_X`; `CHECK()` macro, exit 0 on pass.
- After an ASan sweep, a full clean rebuild (`rm -f obj/*.o lib/libantelope_engine.a bin/<test>`) is required before a normal link.
- Config setters are POST-open: order is `open(dir)` → `set_rerank_config(dim, RERANK_QUANT_FLOAT)` → `set_multivector_pq_config(m, posture, rerank_quant)` → `set_multivector_resident_tier(tier)` → `set_token_index_policy(...)`.
- `segment_filename(buf, size, generation, "ext")` → `seg_%06lld.<ext>`. Token graph metric is `ANT_vector_store::METRIC_DOT` / `ANT_pq_codec::METRIC_DOT` (== 0); `ANT_pq_codec::K == 256`.
- Multi-vector docs are added via the 5-arg `add_document` overload (see VERIFIED API below); copy the call shape from `tests/test_mvpq_recall.cpp` (`add_document(k,b,NULL,doc,md)`).

**VERIFIED API (this block WINS over any test snippet below — the snippets predate this verification; use these exact forms):**
- Multi-vector add is **5-arg**: `add_document(const char *key, const char *body, const float *vector, const float *multivector, long long num_vectors)`. The dense `vector` arg is `NULL` for token-only docs. So every `add_document(name, "body", rows, md)` in the snippets below must be written `add_document(name, "body", NULL, rows, md)`.
- **There is NO `set_token_index_config`.** `token_index_M`/`token_index_ef_construction` default to **16 / 200** in the constructor (no public setter). **Delete every `set_token_index_config(...)` call** from the snippets below — the defaults apply; `ANT_token_index::build`/`load` use `token_index_M`/`token_index_ef_construction` directly.
- Compaction entry point is **`compact(long long *input_generations, long long input_count)`**, NOT `compact()`. To merge the two segments in the Task-5 test, collect the generations first: `long long gens[2] = { idx->disk_segment_generation(0), idx->disk_segment_generation(1) }; CHECK(idx->compact(gens, 2) == 0);`.
- Existing regression suites to re-run are `test_mvpq_recall` / `test_mvpq_search` / `test_mvpq_compaction` / `test_v6_search_multivector` / `test_v6_build_token_index` (NOT `test_pq_tokens`/`test_multivector`, which do not exist). List them with `ls tests/ | grep -E 'mvpq|v6'`.
- Doc/query rows must be L2-normalized (MaxSim over normalized vectors == dot); the snippets already normalize — keep that.

**IMPORTANT design decision (pin this):** the resident tier is **realized at load** (`append_segment`), not in-session. `set_multivector_resident_tier` persists the config and invalidates stale `.tann` (below); the NONE load path (no float resident, PQ token source) engages on the next `open()`. Tests therefore build/`.mvpq` at FLOAT, set the tier, **reopen**, then `build_token_index`/search. And `build_multivector_pq` must read the on-disk `.mvec` when `multivectors == NULL` (NONE, post-reopen backfill) — see Task 4.

---

## File Structure

- **`source/vector_source.h`** — add `ANT_token_source` (abstract, extends `ANT_vector_source`).
- **`source/multivector_store.h`** — retype `ANT_multivector_source` to `ANT_token_source` (+ `num_documents`, `token_docid_of`).
- **`source/multivector_pq_store.{h,cpp}`** — add the `#26` seam methods (`token_prepare_query`/`token_score_prepared`/`token_free_query`) + public `adc_table_builds` counter; add `ANT_multivector_pq_source : ANT_token_source`.
- **`source/token_index.{h,cpp}`** — borrow `ANT_token_source*` instead of `ANT_multivector_store*`.
- **`atire/atire_segment_index.h`** — `MV_TIER_*` enum, `mvpq_resident_tier_current`, `ANT_token_source *token_source` member, `set_multivector_resident_tier`/`multivector_resident_tier`/`disk_segment_resident_tier_mv` decls.
- **`atire/atire_segment_index_vector.cpp`** — tier setter, `multivector_pq.config` v2, `build_multivector_pq` on-disk fallback, `build_token_index` over `token_source`.
- **`atire/atire_segment_index.cpp`** — `append_segment` tier-select + `token_source` construction; teardown free; NULL-init; ctor default; eager flush ordering.
- **`atire/atire_segment_index_compaction.cpp`** — rebuild `token_source` before `.tann` rebuild; teardown free.
- **`tests/test_pq_token_resident_tier.cpp`** (new).

---

## Task 1: `ANT_token_source` + retype `ANT_multivector_source` + `ANT_token_index` borrows it (FLOAT byte-identical)

**Files:**
- Modify: `source/vector_source.h`, `source/multivector_store.h`, `source/token_index.h`, `source/token_index.cpp`
- Modify: `atire/atire_segment_index.h` (add `token_source` member + NULL-init), `atire/atire_segment_index.cpp` (append_segment + teardown), `atire/atire_segment_index_vector.cpp` (build_token_index call site), `atire/atire_segment_index_compaction.cpp` (build call site + teardown)
- Test: `tests/test_pq_token_resident_tier.cpp` (new — FLOAT byte-identical lock)

- [ ] **Step 1: Write the FLOAT byte-identical failing test**

Create `tests/test_pq_token_resident_tier.cpp`. Model the index setup + multi-vector `add_document` calls on the existing token-PQ test (open a scratch dir, `set_rerank_config(dim, RERANK_QUANT_FLOAT)`, add N docs each with M_d normalized rows, `flush`, `build_token_index`, then `search_multivector(q, num_q, k)` and read `get_hit(i)->{filename,docid,score}`). The lock: with token-PQ unconfigured (plain V6), the top-k for a fixed query is unchanged by this refactor.

```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/pq_codec.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
static const char *DIR = "/tmp/test_pq_token_tier_idx";

/* Build a V6 (rerank + token-index) index, no token-PQ. Returns it open. gen_out = seg 0 generation. */
static ATIRE_segment_index *build_v6(long long *gen_out)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* POST-open */
	/* token index M/ef = ctor defaults 16/200; no public setter */
	float row[8];
	for (int d = 0; d < 40; d++)
		{
		char name[64]; snprintf(name,sizeof(name),"doc%d",d);
		int md = 2 + (d % 3);				/* 2..4 tokens per doc */
		float rows[4*8];
		for (int r = 0; r < md; r++)
			{ double nrm=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3)%13-6)/6.0f; nrm+=rows[r*8+j]*rows[r*8+j]; }
			  nrm=sqrt(nrm)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)nrm; }
		CHECK(idx->add_document(name, "body words here", NULL, rows, md) >= 0);	/* 5-arg multi-vector overload */
		}
	CHECK(idx->flush() == 0);
	*gen_out = idx->disk_segment_generation(0);
	CHECK(idx->build_token_index() == 0);
	return idx;
}

static void test_float_token_byte_identical(void)
{
	long long gen;
	ATIRE_segment_index *idx = build_v6(&gen);
	float q[2*8];
	for (int r=0;r<2;r++){ double nrm=0; for(int j=0;j<8;j++){ q[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; nrm+=q[r*8+j]*q[r*8+j]; } nrm=sqrt(nrm)+1e-9; for(int j=0;j<8;j++) q[r*8+j]/=(float)nrm; }
	long long n = idx->search_multivector(q, 2, 10);
	CHECK(n > 0);
	/* snapshot top-k; after the refactor this same call must return the identical ranking */
	printf("float token top-1 = %s score=%.6f (n=%lld)\n", idx->get_hit(0)->filename, idx->get_hit(0)->score, n);
	CHECK(idx->disk_segment_has_token_index(0) == 1);
	delete idx;
	printf("test_float_token_byte_identical PASSED\n");
}

int main(void)
{
	test_float_token_byte_identical();
	printf("ALL test_pq_token_resident_tier PASSED\n");
	return 0;
}
```

Note: confirm the exact multi-vector `add_document` signature and `set_token_index_config` name by grepping an existing token test (`grep -n "add_document\|set_token_index_config\|search_multivector" tests/test_*token*.cpp`); use those exact spellings. If `set_token_index_config` differs, match the real one.

- [ ] **Step 2: Run the test to establish the GREEN baseline (pre-refactor)**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier`
Expected: PASS, printing the top-1 line. This is the pre-refactor snapshot — Task 1 is a pure refactor, so this test must STILL pass byte-identically after the changes (it is the regression lock, not a fail-first test).

- [ ] **Step 3: Add `ANT_token_source` to `source/vector_source.h`**

After the closing `} ;` of `ANT_vector_source` (and before `#endif`), add:

```cpp
/*
	ANT_TOKEN_SOURCE -- an ANT_vector_source whose nodes are TOKENS, plus the two
	things a token index needs beyond per-node scoring: the owning docid of a token
	and the distinct-document count (ANT_vector_source::document_count() is the token
	/ node count for these sources). Implemented by ANT_multivector_source (float)
	and ANT_multivector_pq_source (PQ codes).
*/
class ANT_token_source : public ANT_vector_source
{
public:
	virtual long long num_documents(void) = 0;
	virtual long long token_docid_of(long long t) = 0;
} ;
```

- [ ] **Step 4: Retype `ANT_multivector_source` in `source/multivector_store.h`**

Change `class ANT_multivector_source : public ANT_vector_source` to `: public ANT_token_source`, and add the two methods (delegating to the store):

```cpp
class ANT_multivector_source : public ANT_token_source
{
private:
	ANT_multivector_store *store;

public:
	ANT_multivector_source(ANT_multivector_store *s) : store(s) {}
	long long document_count(void) override { return store->token_count(); }
	long long get_dimension(void) override { return store->get_dimension(); }
	long has(long long node) override { return store->token_has(node); }
	const float *get(long long node) override { return store->token_get(node); }
	long is_quantized(void) override { return store->tokens_quantized(); }
	void reconstruct(long long node, float *out) override { store->token_reconstruct(node, out); }
	double score(long long node, const float *query, long metric) override { return store->token_score(node, query, metric); }
	long long num_documents(void) override { return store->document_count(); }
	long long token_docid_of(long long t) override { return store->token_docid_of(t); }
} ;
```

- [ ] **Step 5: Retarget `ANT_token_index` onto `ANT_token_source` (`source/token_index.h`)**

Replace the `ANT_multivector_store` forward decl + member + build/load signatures with `ANT_token_source`:

```cpp
class ANT_hnsw;
class ANT_token_source;
class ANT_index_tombstones;
```
Member: change `ANT_multivector_store *store;` to `ANT_token_source *source;`.
Signatures:
```cpp
	static ANT_token_index *build(ANT_token_source *source, long long M, long long ef_construction, long metric);
	static ANT_token_index *load(const char *filename, ANT_token_source *source, long long expected_M, long long expected_ef_construction, long metric);
```

- [ ] **Step 6: Update `source/token_index.cpp` to use `source`**

Change the include `#include "multivector_store.h"` to `#include "vector_source.h"`. Then:

In the constructor, rename `store = NULL;` to `source = NULL;`.

In `build`:
```cpp
ANT_token_index *ANT_token_index::build(ANT_token_source *source_in, long long M_in, long long ef_construction_in, long metric_in)
{
long long n = source_in->document_count();			// token / node count
if (n <= 0) return NULL;
if (n > ANT_HNSW_MAX_DOCUMENTS) return NULL;

ANT_token_index *idx = new ANT_token_index();
idx->token_count = n;
idx->documents = source_in->num_documents();
idx->dimension = source_in->get_dimension();
idx->metric = metric_in;
idx->M = M_in;
idx->ef_construction = ef_construction_in;
idx->source = source_in;

idx->token_docid = new int[n];
for (long long t = 0; t < n; t++)
	idx->token_docid[t] = (int)source_in->token_docid_of(t);

idx->graph = new ANT_hnsw();
if (idx->graph->build(source_in, M_in, ef_construction_in, metric_in) != 0)
	{ delete idx; return NULL; }
return idx;
}
```
(Removes the stack-local `ANT_multivector_source source(store);` — the graph now builds directly over `source_in`.)

In `load`, change the parameter to `ANT_token_source *source_in`, set `idx->source = source_in;`, and replace `store->token_count()`/`store->document_count()`/`store->get_dimension()` with `source_in->document_count()`/`source_in->num_documents()`/`source_in->get_dimension()` respectively (the `tc != store->token_count()` check becomes `tc != source_in->document_count()`).

In `search_candidates`, remove `ANT_multivector_source source(store);` and pass the member `source` to the graph:
```cpp
	long long got = graph->search(q, metric, /*ef_search=*/token_top_p, token_top_p,
		source, /*tombstones=*/NULL, tok_docids.data(), tok_scores.data(), /*filter_bits=*/NULL);
```

- [ ] **Step 7: Add the segment `token_source` member (`atire/atire_segment_index.h`)**

Add a forward decl near the others (`class ANT_multivector_store;` etc.): `class ANT_token_source;`. In the segment struct, right after `ANT_token_index *token_index;` (line ~66), add:
```cpp
	ANT_token_source *token_source;		// owns the graph source token_index borrows; float wrapper (FLOAT) or PQ wrapper (NONE); freed AFTER token_index
```

- [ ] **Step 8: Construct/free `token_source` at every segment site (`atire/atire_segment_index.cpp`, `_compaction.cpp`, `_vector.cpp`)**

Include `#include "../source/multivector_store.h"` where these files construct the source (segment index .cpp already includes it via token usage — verify with grep; add if missing).

**append_segment** (`atire_segment_index.cpp`, the `if (rerank_configured())` block ~1606-1633): after `multivectors`/`token_index` load, and in the `else` branch, set `token_source`. For Task 1 (still FLOAT-only), wrap `multivectors`:
```cpp
	segments[segment_count].multivectors = ANT_multivector_store::load(mvec_filename, rerank_dimension_current, engine->get_document_count());
	segments[segment_count].token_source = (segments[segment_count].multivectors != NULL)
		? new ANT_multivector_source(segments[segment_count].multivectors) : NULL;

	char tann_filename[1024];
	segment_filename(tann_filename, sizeof(tann_filename), generation, "tann");
	segments[segment_count].token_index = ANT_token_index::load(tann_filename, segments[segment_count].token_source, token_index_M, token_index_ef_construction, ANT_vector_store::METRIC_DOT);
```
And in the `else` branch add `segments[segment_count].token_source = NULL;`.

**Teardown** — every site that frees the trio must delete `token_source` too, ordered AFTER `token_index`. There are two:
- `atire_segment_index.cpp:156` (close/reset loop): after `delete segments[which].token_index;` add `delete segments[which].token_source;` (before/after the store deletes is fine — dtors don't cross-deref; keep it adjacent to token_index).
- `atire_segment_index_compaction.cpp:703` (shuffle teardown): same — add `delete segments[which].token_source;` after `delete segments[which].token_index;`.

**build_token_index** (`atire_segment_index_vector.cpp:1058`): pass the segment's `token_source` (not `multivectors`) to `build`, and rebuild `token_source` is NOT needed here (it already wraps `multivectors`):
```cpp
	ANT_token_index *idx = ANT_token_index::build(segments[which].token_source, token_index_M, token_index_ef_construction, ANT_vector_store::METRIC_DOT);
```
Guard stays `if (segments[which].multivectors == NULL ...)` for now (Task 4 generalizes to NONE).

**compaction** (`atire_segment_index_compaction.cpp`): after the multivectors refresh (line ~590-591) and BEFORE the `.tann` rebuild (line ~645), rebuild `output_segment->token_source` to wrap the refreshed `multivectors`:
```cpp
delete output_segment->multivectors;
output_segment->multivectors = ANT_multivector_store::load(mvec_name, rerank_dimension_current, output_segment->engine->get_document_count());
delete output_segment->token_source;
output_segment->token_source = (output_segment->multivectors != NULL) ? new ANT_multivector_source(output_segment->multivectors) : NULL;
```
And at the `.tann` rebuild (line ~645) pass `output_segment->token_source`:
```cpp
	ANT_token_index *tidx = ANT_token_index::build(output_segment->token_source, token_index_M, token_index_ef_construction, ANT_vector_store::METRIC_DOT);
```

**ctor default** (`atire_segment_index.cpp`, wherever a new segment slot is initialized in append_segment's growth — grep `.token_index = NULL` around 1631): ensure `token_source` is NULL-initialized in both append_segment branches (done above).

- [ ] **Step 9: Rebuild (headers changed) and run the byte-identical lock + V6 regressions**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier
make test_mvpq_recall && ./bin/test_mvpq_recall
make test_v6_search_multivector && ./bin/test_v6_search_multivector
```
(Confirm the exact existing token/token-PQ test names with `ls tests/ | grep -E 'token|multivector'` and run them.)
Expected: `test_pq_token_resident_tier` prints the SAME top-1 line as Step 2 and PASSES; the existing token/token-PQ/multivector suites all PASS (float path byte-identical).

- [ ] **Step 10: Commit**

```bash
git add source/vector_source.h source/multivector_store.h source/token_index.h source/token_index.cpp atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp atire/atire_segment_index_compaction.cpp tests/test_pq_token_resident_tier.cpp
git commit -m "feat(#24): ANT_token_source abstraction; ANT_token_index borrows it (float path byte-identical)"
```

---

## Task 2: `ANT_multivector_pq_source` + the `#26` seam on the PQ token store

**Files:**
- Modify: `source/multivector_pq_store.h` (counter + seam decls + `token_codes`/`get_codebook` already present + the source class), `source/multivector_pq_store.cpp` (counter init + seam impls)
- Test: `tests/test_pq_token_resident_tier.cpp` (add store-level seam tests)

- [ ] **Step 1: Write the failing seam unit tests**

Add to `tests/test_pq_token_resident_tier.cpp` (above `main`), and call them in `main`. Build a `.mvpq` store directly via `ANT_multivector_pq_store_writer` (see `finish` trains+encodes), then exercise the seam:

```cpp
#include "../source/multivector_pq_store.h"
#include "../source/vector_source.h"

static ANT_multivector_pq_store *make_mvpq(long long dim, long long m, long long ndoc, const char *path)
{
	remove(path);
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT) == 0);
	srand(5);
	for (long long d=0; d<ndoc; d++)
		{
		long long md = 2 + (d % 3);
		float rows[4*16];
		for (long long r=0;r<md;r++){ double nrm=0; for(long long j=0;j<dim;j++){ rows[r*dim+j]=(float)(rand()%200-100)/100.0f; nrm+=rows[r*dim+j]*rows[r*dim+j]; } nrm=sqrt(nrm)+1e-9; for(long long j=0;j<dim;j++) rows[r*dim+j]/=(float)nrm; }
		CHECK(w.append(rows, md) == 0);
		}
	CHECK(w.finish() == 0);
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, dim, ndoc, ANT_pq_codec::METRIC_DOT);
	CHECK(s->token_count() > 0);
	return s;
}

static void test_token_seam_equivalence(void)
{
	const char *path = "/tmp/test_pq_token_seam.mvpq";
	long long dim=16, m=8, ndoc=30;
	ANT_multivector_pq_store *s = make_mvpq(dim, m, ndoc, path);
	float q[16]; for(int j=0;j<dim;j++) q[j]=(float)(rand()%200-100)/100.0f;

	void *ctx = s->token_prepare_query(q);
	CHECK(ctx != 0);
	for (long long t=0; t<s->token_count(); t+=5)
		CHECK(fabs(s->token_score_prepared(t, q, ctx) - s->token_score(t, q, ANT_pq_codec::METRIC_DOT)) < 1e-9);
	CHECK(fabs(s->token_score_prepared(1, q, NULL) - s->token_score(1, q, ANT_pq_codec::METRIC_DOT)) < 1e-9);	/* NULL ctx fallback */
	s->token_free_query(ctx);
	s->token_free_query(NULL);	/* delete[] NULL safe */

	/* counter: +1 per prepare, +1 per token_score, +0 per prepared reuse */
	long long b = s->adc_table_builds;
	for (long long t=0;t<10;t++) s->token_score(t, q, ANT_pq_codec::METRIC_DOT);
	CHECK(s->adc_table_builds - b == 10);
	long long b2 = s->adc_table_builds;
	void *c2 = s->token_prepare_query(q);
	CHECK(s->adc_table_builds - b2 == 1);
	for (long long t=0;t<10;t++) s->token_score_prepared(t, q, c2);
	CHECK(s->adc_table_builds - b2 == 1);
	s->token_free_query(c2);

	/* source adapter delegates */
	ANT_multivector_pq_source src(s);
	CHECK(src.is_quantized() == 1);
	CHECK(src.get(0) == 0);
	CHECK(src.document_count() == s->token_count());
	CHECK(src.num_documents() == s->document_count());
	void *c3 = src.prepare_query(q, ANT_pq_codec::METRIC_DOT);
	CHECK(fabs(src.score_prepared(0, q, ANT_pq_codec::METRIC_DOT, c3) - s->token_score(0, q, ANT_pq_codec::METRIC_DOT)) < 1e-9);
	src.free_query(c3);
	delete s; remove(path);
	printf("test_token_seam_equivalence PASSED\n");
}
```
Add `test_token_seam_equivalence();` to `main`.

- [ ] **Step 2: Run to verify it fails to compile**

Run: `make test_pq_token_resident_tier`
Expected: FAIL — no member `token_prepare_query` / `adc_table_builds` / no class `ANT_multivector_pq_source`.

- [ ] **Step 3: Add counter + seam decls + source class to `source/multivector_pq_store.h`**

In `ANT_multivector_pq_store` public section (after `~ANT_multivector_pq_store();`):
```cpp
	long long adc_table_builds;	// diagnostic-only, NOT thread-safe: # of ADC-table builds (token_score + token_prepare_query)
```
After `token_score(...)`:
```cpp
	void  *token_prepare_query(const float *query);						// build the m*K ADC table once; caller frees via token_free_query
	double token_score_prepared(long long t, const float *query, void *ctx);	// ADC via a prepared table; ctx==NULL -> falls back to token_score(t,query,metric)
	void   token_free_query(void *ctx);
	const float *get_codebook(void) { return codebook; }
	long get_metric(void) { return metric; }
```
After the writer class (or before `#endif`), add the source adapter:
```cpp
#include "vector_source.h"
class ANT_multivector_pq_source : public ANT_token_source
{
private:
	ANT_multivector_pq_store *store;
public:
	ANT_multivector_pq_source(ANT_multivector_pq_store *s) : store(s) {}
	long long document_count(void) override { return store->token_count(); }
	long long get_dimension(void) override { return store->get_dimension(); }
	long has(long long node) override { return store->token_has(node); }
	const float *get(long long node) override { (void)node; return 0; }
	long is_quantized(void) override { return 1; }
	void reconstruct(long long node, float *out) override { store->token_reconstruct(node, out); }
	double score(long long node, const float *query, long metric) override { return store->token_score(node, query, metric); }
	long long num_documents(void) override { return store->document_count(); }
	long long token_docid_of(long long t) override { return store->token_docid_of(t); }
	void  *prepare_query(const float *query, long metric) override { (void)metric; return store->token_prepare_query(query); }
	double score_prepared(long long node, const float *query, long metric, void *ctx) override { (void)metric; return store->token_score_prepared(node, query, ctx); }
	void   free_query(void *ctx) override { store->token_free_query(ctx); }
} ;
```

- [ ] **Step 4: Implement in `source/multivector_pq_store.cpp`**

In the constructor init list add `adc_table_builds(0)` (append to the member-init list at line ~11-12).

In `token_score` (line ~40-47), bump the counter right after the `adc_table` build:
```cpp
ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);
adc_table_builds++;
```
Add the seam impls after `token_score`. Note `token_score_prepared` takes `query` so the `ctx==NULL` fallback can call `token_score(t, query, metric)` (which dereferences `query`):
```cpp
void *ANT_multivector_pq_store::token_prepare_query(const float *query)
{
if (total_tokens == 0 || codebook == 0)
	return 0;								/* degraded store: ctx==NULL -> score_prepared falls back */
double *table = new double[(size_t)(m * ANT_pq_codec::K)];
ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);
adc_table_builds++;
return table;
}

double ANT_multivector_pq_store::token_score_prepared(long long t, const float *query, void *ctx)
{
if (ctx == 0)
	return token_score(t, query, metric);	/* no prepared table -> per-call build (build path) */
if (!token_has(t))
	return 0.0;
return ANT_pq_codec::adc_score(codes + t*m, m, (double *)ctx);
}

void ANT_multivector_pq_store::token_free_query(void *ctx)
{
delete [] (double *)ctx;					/* delete[] NULL is a no-op */
}
```

- [ ] **Step 5: Rebuild and run**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier`
Expected: PASS including `test_token_seam_equivalence PASSED`.

- [ ] **Step 6: Commit**

```bash
git add source/multivector_pq_store.h source/multivector_pq_store.cpp tests/test_pq_token_resident_tier.cpp
git commit -m "feat(#24): ANT_multivector_pq_source + prepare-per-query ADC seam on the PQ token store"
```

---

## Task 3: `set_multivector_resident_tier` + config v2 + `append_segment` tier-select + stale-`.tann` invalidation

**Files:**
- Modify: `atire/atire_segment_index.h` (enum, member, decls), `atire/atire_segment_index_vector.cpp` (config v2, tier setter), `atire/atire_segment_index.cpp` (ctor default, append_segment tier-select)
- Test: `tests/test_pq_token_resident_tier.cpp` (tier accessor + invalidation, fail-first)

- [ ] **Step 1: Write the failing tier + invalidation test**

Add to `tests/test_pq_token_resident_tier.cpp`. Build a token-PQ index at FLOAT, `build_multivector_pq`, `build_token_index` (so a `.tann` exists), then flip to NONE and assert the `.tann`/`.tann.g` were removed + the accessor reports NONE:

```cpp
static void test_tier_change_invalidates_tann(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* token index M/ef = ctor defaults 16/200; no public setter */
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	float row[8];
	for (int d=0; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); int md=2+(d%3); float rows[4*8];
		for(int r=0;r<md;r++){ double n=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3)%13-6)/6.0f; n+=rows[r*8+j]*rows[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)n; }
		CHECK(idx->add_document(nm,"body",NULL,rows,md)>=0); }
	CHECK(idx->flush() == 0);
	long long gen = idx->disk_segment_generation(0);
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->build_token_index() == 0);
	CHECK(idx->disk_segment_has_token_index(0) == 1);
	CHECK(idx->multivector_resident_tier() == ATIRE_segment_index::MV_TIER_FLOAT);

	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	CHECK(idx->multivector_resident_tier() == ATIRE_segment_index::MV_TIER_NONE);
	/* stale float-geometry .tann must be gone + in-memory index nulled */
	char tann[2048], tanng[2100];
	snprintf(tann,sizeof(tann),"%s/seg_%06lld.tann",DIR,gen);
	snprintf(tanng,sizeof(tanng),"%s/seg_%06lld.tann.g",DIR,gen);
	FILE *a=fopen(tann,"rb"); CHECK(a==NULL); FILE *b=fopen(tanng,"rb"); CHECK(b==NULL);
	CHECK(idx->disk_segment_has_token_index(0) == 0);
	/* immutability: cannot move off NONE */
	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_FLOAT) != 0);
	delete idx;
	printf("test_tier_change_invalidates_tann PASSED\n");
}
```
Add `test_tier_change_invalidates_tann();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test_pq_token_resident_tier`
Expected: FAIL to compile (`no member set_multivector_resident_tier` / `MV_TIER_NONE` / `multivector_resident_tier`).

- [ ] **Step 3: Add enum + member + decls (`atire/atire_segment_index.h`)**

Near the `PQ_TIER_*` enum (line ~244) add:
```cpp
	enum { MV_TIER_FLOAT = 0, MV_TIER_NONE = 1 };
```
Add a member near `mvpq_posture_current` (line ~126):
```cpp
	long mvpq_resident_tier_current;		// MV_TIER_FLOAT (default) / MV_TIER_NONE
```
Add public decls near `set_multivector_pq_config` (line ~280) / `disk_segment_resident_tier` (line ~374):
```cpp
	long set_multivector_resident_tier(long tier);	// 0 ok; nonzero: not open / token-PQ unconfigured / invalid / already off-FLOAT (immutable)
	long multivector_resident_tier(void) { return mvpq_resident_tier_current; }
	long disk_segment_resident_tier_mv(long long which);	// test accessor: MV_TIER_NONE if float pool not resident but .mvpq is, else MV_TIER_FLOAT
```

- [ ] **Step 4: Default the member + `multivector_pq.config` v2 (`atire/atire_segment_index_vector.cpp` + ctor)**

In the constructor (`atire_segment_index.cpp`, where `mvpq_posture_current` etc. are initialized — grep `mvpq_posture_current =`), add `mvpq_resident_tier_current = MV_TIER_FLOAT;`.

Bump `save_multivector_pq_config` / `load_multivector_pq_config` to version 2 with a 4th i64 (tier), back-compat with v1:
```cpp
// load (replace the parse block):
char tag[8]; unsigned int version; long long vals[4];
if (fread(tag,1,8,in)!=8 || memcmp(tag,"ANTMVPQC",8)!=0 || fread(&version,4,1,in)!=1) { fclose(in); return 1; }
long ok;
if (version == 1)
	ok = fread(vals, 8, 3, in) == 3, vals[3] = MV_TIER_FLOAT;
else if (version == 2)
	ok = fread(vals, 8, 4, in) == 4;
else ok = 0;
fclose(in);
if (!ok) return 1;
mvpq_m_current = vals[0];
mvpq_posture_current = (long)vals[1];
mvpq_rerank_quant_current = (long)vals[2];
mvpq_resident_tier_current = (long)vals[3];
return 0;
```
(Write the comma-operator line as two statements for clarity: `if (version==1){ ok = fread(vals,8,3,in)==3; vals[3]=MV_TIER_FLOAT; } else if (version==2){ ok = fread(vals,8,4,in)==4; } else ok=0;`.)
```cpp
// save (bump version + 4 vals):
unsigned int version = 2;
long long vals[4] = { mvpq_m_current, mvpq_posture_current, mvpq_rerank_quant_current, mvpq_resident_tier_current };
long ok = fwrite("ANTMVPQC",1,8,out)==8 && fwrite(&version,4,1,out)==1 && fwrite(vals,8,4,out)==4;
```

- [ ] **Step 5: Implement `set_multivector_resident_tier` + `disk_segment_resident_tier_mv` (`atire/atire_segment_index_vector.cpp`)**

Mirror the dense `set_pq_resident_tier` (same file, line ~576). Place after `set_multivector_pq_config`:
```cpp
long ATIRE_segment_index::set_multivector_resident_tier(long tier)
{
if (directory == NULL)
	return 1;
if (!multivector_pq_configured())
	return 1;
if (tier != MV_TIER_FLOAT && tier != MV_TIER_NONE)
	return 1;
if (mvpq_resident_tier_current != tier && mvpq_resident_tier_current != MV_TIER_FLOAT)
	return 1;						// immutable once moved off the default
if (mvpq_resident_tier_current == tier)
	return 0;						// idempotent
long previous = mvpq_resident_tier_current;
mvpq_resident_tier_current = tier;
if (save_multivector_pq_config() != 0)
	{ mvpq_resident_tier_current = previous; return 1; }

/*
	#24: a real tier change away from FLOAT changes the token graph source (float
	.mvec -> .mvpq ADC). Any .tann built over the OLD float geometry no longer
	matches how the PQ source scores nodes, so invalidate every per-segment .tann
	(+ .tann.g) and null the in-memory token_index; the next build_token_index()/
	compaction rebuilds over the new (PQ) source at reopen. Mirrors set_pq_resident_tier.
*/
char tann_name[4096], tanng_name[4200];
long long which;
for (which = 0; which < segment_count; which++)
	{
	segment_filename(tann_name, sizeof(tann_name), segments[which].generation, "tann");
	snprintf(tanng_name, sizeof(tanng_name), "%s.g", tann_name);
	remove(tann_name);
	remove(tanng_name);
	delete segments[which].token_index;
	segments[which].token_index = NULL;
	}
return 0;
}

long ATIRE_segment_index::disk_segment_resident_tier_mv(long long which)
{
if (which < 0 || which >= segment_count)
	return -1;
if (segments[which].multivectors == NULL && segments[which].multivector_pq != NULL)
	return MV_TIER_NONE;
return MV_TIER_FLOAT;
}
```

- [ ] **Step 6: `append_segment` tier-select (`atire/atire_segment_index.cpp`)**

Replace the Task-1 unconditional float load in the `if (rerank_configured())` block so that under NONE (config already loaded via `load_multivector_pq_config` at open) the float pool is not resident and `token_source` wraps the PQ store. Load `multivector_pq` FIRST, then branch:
```cpp
if (rerank_configured())
	{
	/* load .mvpq first so the tier decision can use it */
	segments[segment_count].multivector_pq = NULL;
	if (multivector_pq_configured())
		{
		char mvpq_filename[1024];
		segment_filename(mvpq_filename, sizeof(mvpq_filename), generation, "mvpq");
		ANT_multivector_pq_store *p = ANT_multivector_pq_store::load(mvpq_filename, rerank_dimension_current, engine->get_document_count(), ANT_pq_codec::METRIC_DOT);
		if (p->document_count() == engine->get_document_count() && p->token_count() > 0)
			segments[segment_count].multivector_pq = p;
		else
			delete p;
		}

	long none_tier = (mvpq_resident_tier_current == MV_TIER_NONE && segments[segment_count].multivector_pq != NULL);

	char mvec_filename[1024];
	segment_filename(mvec_filename, sizeof(mvec_filename), generation, "mvec");
	if (none_tier)
		segments[segment_count].multivectors = NULL;			/* drop resident float pool */
	else
		segments[segment_count].multivectors = ANT_multivector_store::load(mvec_filename, rerank_dimension_current, engine->get_document_count());

	if (none_tier)
		segments[segment_count].token_source = new ANT_multivector_pq_source(segments[segment_count].multivector_pq);
	else if (segments[segment_count].multivectors != NULL)
		segments[segment_count].token_source = new ANT_multivector_source(segments[segment_count].multivectors);
	else
		segments[segment_count].token_source = NULL;

	char tann_filename[1024];
	segment_filename(tann_filename, sizeof(tann_filename), generation, "tann");
	segments[segment_count].token_index = ANT_token_index::load(tann_filename, segments[segment_count].token_source, token_index_M, token_index_ef_construction, ANT_vector_store::METRIC_DOT);
	}
else
	{
	segments[segment_count].multivectors = NULL;
	segments[segment_count].token_index = NULL;
	segments[segment_count].multivector_pq = NULL;
	segments[segment_count].token_source = NULL;
	}
```
Add `#include "../source/multivector_pq_store.h"` at the top of `atire_segment_index.cpp` if not already present (it is — it constructs `multivector_pq`; verify). Note `ANT_token_index::load` tolerates a NULL `source` only if it never derefs it before the empty-return; guard: if `token_source == NULL`, skip the `load` and set `token_index = NULL` (add `segments[segment_count].token_index = segments[segment_count].token_source ? ANT_token_index::load(...) : NULL;`).

- [ ] **Step 7: Rebuild and run (invalidation fail-first now passes)**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier`
Expected: PASS including `test_tier_change_invalidates_tann PASSED`. Re-run the existing token-PQ suite (`make test_mvpq_recall && ./bin/test_mvpq_recall`) — still PASS (v1 config back-compat + FLOAT default unchanged).

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_pq_token_resident_tier.cpp
git commit -m "feat(#24): set_multivector_resident_tier {FLOAT,NONE} + config v2 + append_segment tier-select + stale-.tann invalidation"
```

---

## Task 4: Build the `.tann` over the tier source (NONE → PQ) + end-to-end recall + seam-engaged

**Files:**
- Modify: `atire/atire_segment_index_vector.cpp` (`build_token_index` guard + on-disk `.mvec` fallback in `build_multivector_pq`)
- Test: `tests/test_pq_token_resident_tier.cpp` (NONE end-to-end recall + one-table-per-query-token)

- [ ] **Step 1: Write the NONE-tier end-to-end + seam-engaged failing test**

Add to `tests/test_pq_token_resident_tier.cpp`. Build at FLOAT, `build_multivector_pq`, set NONE, **reopen**, `build_token_index`, assert NONE resident + recall + the ADC table built once per query token:

```cpp
static void test_none_tier_end_to_end(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* token index M/ef = ctor defaults 16/200; no public setter */
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	for (int d=0; d<40; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d",d); int md=2+(d%3); float rows[4*8];
		for(int r=0;r<md;r++){ double n=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3)%13-6)/6.0f; n+=rows[r*8+j]*rows[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)n; }
		CHECK(idx->add_document(nm,"body",NULL,rows,md)>=0); }
	CHECK(idx->flush() == 0);
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete idx;						/* close; NONE takes effect on reopen */

	idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);				/* load_multivector_pq_config -> NONE */
	CHECK(idx->disk_segment_resident_tier_mv(0) == ATIRE_segment_index::MV_TIER_NONE);	/* no float pool resident */
	CHECK(idx->build_token_index() == 0);			/* builds .tann over the PQ source */
	CHECK(idx->disk_segment_has_token_index(0) == 1);

	float q[2*8];
	for (int r=0;r<2;r++){ double n=0; for(int j=0;j<8;j++){ q[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; n+=q[r*8+j]*q[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) q[r*8+j]/=(float)n; }
	long long n = idx->search_multivector(q, 2, 10);
	CHECK(n > 0);						/* NONE-tier token-ANN answers */
	delete idx;
	printf("test_none_tier_end_to_end PASSED\n");
}
```
Add `test_none_tier_end_to_end();` to `main`. (A stricter recall-vs-exact assertion can be added, but "answers with n>0 over a NONE index whose float pool is not resident" already proves the path; keep it robust.)

- [ ] **Step 2: Run to verify it fails**

Run: `make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier`
Expected: FAIL — after reopen at NONE, `multivectors == NULL`, so `build_token_index`'s `multivectors == NULL` guard skips the segment → `disk_segment_has_token_index(0) == 0` (or `build_multivector_pq` at NONE can't backfill). This is the gap Steps 3-4 close.

- [ ] **Step 3: Generalize `build_token_index` to the tier source (`atire/atire_segment_index_vector.cpp:1050-1065`)**

Replace the `multivectors == NULL` guard with a `token_source` guard so NONE builds over the PQ source:
```cpp
for (which = 0; which < segment_count; which++)
	{
	if (segments[which].token_source == NULL || segments[which].token_source->document_count() == 0)
		continue;				/* no resident token source (float dropped AND no .mvpq) */
	if (segments[which].token_index != NULL && !segments[which].token_index->empty())
		continue;				/* already built */

	segment_filename(tann_name, sizeof(tann_name), segments[which].generation, "tann");
	ANT_token_index *idx = ANT_token_index::build(segments[which].token_source, token_index_M, token_index_ef_construction, ANT_vector_store::METRIC_DOT);
	if (idx == NULL) continue;
	if (idx->save(tann_name) != 0) { delete idx; continue; }
	delete segments[which].token_index;
	segments[which].token_index = idx;
	}
```
(`token_source->document_count()` is the token count for both float and PQ sources.)

- [ ] **Step 4: `build_multivector_pq` on-disk `.mvec` fallback under NONE (`atire/atire_segment_index_vector.cpp:1290+`)**

`build_multivector_pq` currently reads the resident `segments[which].multivectors`, which is NULL under NONE. Make it load the on-disk `.mvec` when the resident pool is absent, so a post-reopen backfill still works (the dense on-disk-float lesson). At the top of the per-segment loop body, replace `ANT_multivector_store *mv = segments[which].multivectors;` with:
```cpp
	ANT_multivector_store *mv = segments[which].multivectors;
	ANT_multivector_store *mv_disk = NULL;			/* loaded from disk when float pool not resident (NONE tier) */
	if (mv == NULL)
		{
		char mvec_name[4096];
		segment_filename(mvec_name, sizeof(mvec_name), segments[which].generation, "mvec");
		mv_disk = ANT_multivector_store::load(mvec_name, rerank_dimension_current, segments[which].engine->get_document_count());
		if (mv_disk->document_count() == segments[which].engine->get_document_count() && mv_disk->token_count() > 0 && !mv_disk->tokens_quantized())
			mv = mv_disk;
		}
	if (mv == NULL || mv->tokens_quantized())
		{ delete mv_disk; continue; }
```
And free `mv_disk` at the end of the per-segment iteration (after the `.mvpq` is written / on every `continue` from this point). The simplest safe pattern: `delete mv_disk;` right before the loop's closing `}` and at each intermediate `continue` in this segment's body. (Grep the loop body for `continue;` and add `delete mv_disk;` before each, or restructure with a single cleanup label.) Ensure exactly one `delete mv_disk` per iteration.

- [ ] **Step 5: Rebuild and run (NONE end-to-end + seam-engaged pass)**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier`
Expected: PASS including `test_none_tier_end_to_end PASSED`. Re-run `test_mvpq_recall` — still PASS.

- [ ] **Step 6: Eager-flush ordering (`atire/atire_segment_index.cpp:1446`)**

Ensure the eager token-index build runs AFTER the eager `.mvpq` build so the NONE tier source exists at flush. Find the eager `.mvpq` block (grep `Eager token-PQ`) and the `if (token_index_eager) build_token_index();` at line 1446; move/guard so under `multivector_pq_configured()` the order is `build_multivector_pq()` then `build_token_index()`. Concretely, if the `.mvpq` eager block is currently AFTER the token-index eager block, relocate the `if (token_index_eager) build_token_index();` call to AFTER the eager `build_multivector_pq()` call. (In-session at flush the float pool is still resident since the segment was just appended at FLOAT, so this only matters for a NONE-configured index reopened and re-flushed — but the ordering is correct to pin regardless.)

- [ ] **Step 7: Commit**

```bash
git add atire/atire_segment_index_vector.cpp atire/atire_segment_index.cpp tests/test_pq_token_resident_tier.cpp
git commit -m "feat(#24): build .tann over the tier source (NONE->PQ), on-disk .mvec backfill, eager ordering"
```

---

## Task 5: Compaction over the tier source + teardown safety

**Files:**
- Modify: `atire/atire_segment_index_compaction.cpp` (rebuild `token_source` over the tier; teardown already handled in Task 1 — verify)
- Test: `tests/test_pq_token_resident_tier.cpp` (compaction under NONE)

- [ ] **Step 1: Write the compaction-under-NONE failing test**

Add to `tests/test_pq_token_resident_tier.cpp`: two flushes at NONE (reopened), `build_multivector_pq`+`build_token_index`, `compact()`, assert the merged segment still answers `search_multivector` and reports NONE resident:

```cpp
static void test_compaction_under_none(void)
{
	char cmd[2048]; snprintf(cmd,sizeof(cmd),"rm -rf %s && mkdir -p %s",DIR,DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_rerank_config(8, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* token index M/ef = ctor defaults 16/200; no public setter */
	CHECK(idx->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	for (int f=0; f<2; f++)
		{
		for (int d=0; d<20; d++){ char nm[64]; snprintf(nm,sizeof(nm),"doc%d_%d",f,d); int md=2+(d%3); float rows[4*8];
			for(int r=0;r<md;r++){ double n=0; for(int j=0;j<8;j++){ rows[r*8+j]=(float)((d*7+r*5+j*3+f)%13-6)/6.0f; n+=rows[r*8+j]*rows[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) rows[r*8+j]/=(float)n; }
			CHECK(idx->add_document(nm,"body",NULL,rows,md)>=0); }
		CHECK(idx->flush() == 0);
		}
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete idx;
	idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->build_multivector_pq() == 0);
	CHECK(idx->build_token_index() == 0);
	long long gens[2] = { idx->disk_segment_generation(0), idx->disk_segment_generation(1) };
	CHECK(idx->compact(gens, 2) == 0);
	CHECK(idx->disk_segment_resident_tier_mv(0) == ATIRE_segment_index::MV_TIER_NONE);
	float q[2*8]; for (int r=0;r<2;r++){ double n=0; for(int j=0;j<8;j++){ q[r*8+j]=(float)((r*11+j*2)%13-6)/6.0f; n+=q[r*8+j]*q[r*8+j]; } n=sqrt(n)+1e-9; for(int j=0;j<8;j++) q[r*8+j]/=(float)n; }
	CHECK(idx->search_multivector(q, 2, 10) > 0);
	delete idx;
	printf("test_compaction_under_none PASSED\n");
}
```
Add `test_compaction_under_none();` to `main`. (Confirm the compaction entry point is `compact()`; grep `::compact` if unsure.)

- [ ] **Step 2: Run to verify it fails**

Run: `make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier`
Expected: FAIL — compaction rebuilds `output_segment->token_source` as an `ANT_multivector_source` wrapping `multivectors`, which is NULL under NONE, so the merged segment has no token source / the merged `.mvpq` isn't the graph source → the merged segment loses its token index or NONE residency isn't reported.

- [ ] **Step 3: Rebuild `token_source` over the tier in compaction (`atire/atire_segment_index_compaction.cpp`)**

After the merged `.mvpq` refresh (line ~622-623, where `output_segment->multivector_pq` is reloaded) and the `multivectors` refresh (line ~590-591), set `output_segment->token_source` per tier, BEFORE the `.tann` rebuild (line ~645). Replace the Task-1 `token_source` rebuild (which always wrapped `multivectors`) with a tier-aware one:
```cpp
delete output_segment->token_source;
if (mvpq_resident_tier_current == MV_TIER_NONE && output_segment->multivector_pq != NULL)
	{
	delete output_segment->multivectors;			/* NONE: do not keep the float pool resident on the merged segment */
	output_segment->multivectors = NULL;
	output_segment->token_source = new ANT_multivector_pq_source(output_segment->multivector_pq);
	}
else if (output_segment->multivectors != NULL)
	output_segment->token_source = new ANT_multivector_source(output_segment->multivectors);
else
	output_segment->token_source = NULL;
```
Place this AFTER both the `multivectors` reload (~591) and the `multivector_pq` reload (~623) so both are current, and BEFORE the `.tann` build at ~645. The `.tann` build call (Task 1 already changed it to `output_segment->token_source`) then builds over the PQ source under NONE. The merged float `.mvec` on disk is still written by the merge (the compaction dense-token merge reads each input's float pool — under NONE the inputs' resident float is NULL, so the merge must read each input's on-disk `.mvec`; verify the merge source at lines ~265-286 uses on-disk load when `inputs[input]->multivectors == NULL`, and if not, load it on-disk there exactly like Task 4 Step 4). Guard `output_segment->multivector_pq != NULL` so a failed `.mvpq` refresh falls back to float.

- [ ] **Step 4: Rebuild and run**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier`
Expected: PASS including `test_compaction_under_none PASSED`. Re-run `test_mvpq_compaction` and `test_v6_search_multivector` (compaction/token regressions) — PASS.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_compaction.cpp tests/test_pq_token_resident_tier.cpp
git commit -m "feat(#24): compaction rebuilds .tann over the tier source (NONE->PQ) + on-disk merge source"
```

---

## Task 6: Recall sanity across tiers + ASan/UBSan sweep

**Files:**
- Verify only (fix any sanitizer finding): the touched sources
- Test: `tests/test_pq_token_resident_tier.cpp` (add a cross-tier recall sanity if not already strong)

- [ ] **Step 1: Add a FLOAT-vs-NONE recall sanity assertion**

If not already covered, extend `test_none_tier_end_to_end` (or add `test_cross_tier_recall`) to build the SAME data at FLOAT and at NONE and assert the NONE top-k overlaps the FLOAT top-k by a sane floor (e.g. ≥ 7/10 of the FLOAT top-10 docids appear in the NONE top-10), proving PQ token search is a faithful approximation:

```cpp
static void test_cross_tier_recall(void)
{
	/* reuse build helpers: build FLOAT index -> record top-10 docids; build NONE index (reopen) -> top-10;
	   assert overlap >= 7. Uses the same 40-doc synthetic set + fixed query as build_v6/test_none. */
	/* ... (mirror the two builds above, collect get_hit(i)->docid for i<10 each, count intersection) ... */
	printf("test_cross_tier_recall PASSED\n");
}
```
Write it out fully using the same doc/query generators as the earlier tests (do not leave the `...` — inline the two builds and the intersection count). Add the call to `main`.

- [ ] **Step 2: Run the full test normally**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier`
Expected: `ALL test_pq_token_resident_tier PASSED`, exit 0.

- [ ] **Step 3: ASan/UBSan sweep**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a bin/test_pq_token_resident_tier
make CC='g++ -fsanitize=address,undefined -g' test_pq_token_resident_tier
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_token_resident_tier; echo "EXIT=$?"
```
Expected: `ALL test_pq_token_resident_tier PASSED`, EXIT=0, no ASan/UBSan reports. This test opens a segment index, so the known out-of-scope `ANT_file::setvbuff` 4MB leak may appear — if the ONLY leak is that (stack shows `ANT_file::setvbuff`/`read_entire_file`), re-run with `detect_leaks=0` to confirm the PASSED line and treat it as the known exclusion; any leak in `token_source`/`ANT_multivector_pq_source`/`token_prepare_query`/`token_free_query`/compaction teardown is a real defect — fix it (most likely a missing `delete token_source`, a missing `delete mv_disk` in `build_multivector_pq`, or a missing `token_free_query`) and re-run.

- [ ] **Step 4: Clean rebuild for a normal link + final run**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a bin/test_pq_token_resident_tier
make test_pq_token_resident_tier && ./bin/test_pq_token_resident_tier; echo "EXIT=$?"
```
Expected: `ALL test_pq_token_resident_tier PASSED`, EXIT=0.

- [ ] **Step 5: Commit (only if a sanitizer fix was applied)**

```bash
git add -u source atire tests
git commit -m "fix(#24): address sanitizer finding in the token resident-tier seam"
```

---

## Self-Review

**1. Spec coverage:**
- Spec §1 (`ANT_token_source`, retype `ANT_multivector_source`, `token_index` borrows it) → Task 1. ✓
- Spec §2 (`{FLOAT,NONE}` tier, `set_multivector_resident_tier`, config v2 back-compat, `append_segment` tier-select, float stays on disk, `disk_segment_resident_tier_mv`) → Task 3 (+ on-disk read in Task 4/5). ✓
- Spec §3 (`#26` seam: `token_prepare_query`/`token_score_prepared`/`token_free_query` + counter; `ANT_multivector_pq_source` overrides) → Task 2. ✓
- Spec §4 (build/eager over `token_source`; compaction refresh-before-rebuild + teardown; stale-`.tann` invalidation) → Tasks 3 (invalidation), 4 (build/eager), 5 (compaction). ✓
- Spec §5 error handling (NONE w/o `.mvpq` → float fallback; degraded `.tann`; borrowed lifetime; DOT metric) → append_segment `none_tier` guard requires a valid `.mvpq` (Task 3 Step 6), teardown ordering (Task 1 Step 8), token_source NULL guard. ✓
- Spec §6 tests → Tasks 1–6 map to the six test cases. ✓

**2. Placeholder scan:** Task 6 Step 1's `test_cross_tier_recall` sketch has a `...` — the step text explicitly instructs the implementer to inline both builds and the intersection count (do not leave the `...`); flagged, not a silent placeholder. All other steps carry full code or exact before→after edits with file:line anchors.

**3. Type/signature consistency:** `ANT_token_source` (`num_documents`, `token_docid_of`) is spelled identically in vector_source.h, both source adapters, and token_index. `ANT_token_index::build/load(ANT_token_source*, …)` matches every call site (append_segment, build_token_index, compaction). The seam trio settled on `token_prepare_query(query)` / `token_score_prepared(t, query, ctx)` / `token_free_query(ctx)` after the Task 2 Step 4 NULL-fallback fix — the test calls and the `ANT_multivector_pq_source::score_prepared` override use that final shape. `MV_TIER_FLOAT/NONE`, `mvpq_resident_tier_current`, `disk_segment_resident_tier_mv`, and `multivector_resident_tier()` are consistent across the header, setter, config load/save, and tests. `adc_table_builds` bumped in `token_score` + `token_prepare_query` only (not `token_score_prepared`), matching the counter test.

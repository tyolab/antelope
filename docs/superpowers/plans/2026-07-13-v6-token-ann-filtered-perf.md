# V6 Token-ANN Filtered Completeness + Candidate-Scratch Perf Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Guarantee filtered `search_multivector` never under-fills a segment that has ≥`top_k` matching docs (#16), and remove the per-segment/per-query O(documents) scratch allocation in `ANT_token_index::search_candidates` (#17).

**Architecture:** #17 first (isolated, behavior-preserving): replace three `documents`-sized per-call vectors with epoch-stamped reusable member buffers reset lazily per touched doc (mirrors `ANT_hnsw`'s `visited_epoch`). Then #16: in `multivector_candidates`, when a filter is active and the token-ANN candidate count for a segment is `< top_k`, fall through to the exact brute-force scan for that segment (a superset — replace, don't append, to avoid double-insertion into the shared top-k heap); the exact scan is extracted into a shared helper.

**Tech Stack:** C++ engine — `source/token_index.{h,cpp}`, `atire/atire_segment_index.h`, `atire/atire_segment_index_vector.cpp`; tests under `tests/*.cpp` (auto-discovered, built to `bin/<name>` via `make <name>`, `CHECK()` macro, exit 0 = pass).

---

## Repo setup notes (read first)

- Fresh worktree: `mkdir -p obj bin lib` and copy the prebuilt `external/**/*.a` (libz/libbz2/liblzo2/libsnappy/libstemmer) from the main checkout — they're gitignored build products; the link fails without them.
- After ANY header change (`token_index.h`, `atire_segment_index.h`): `rm -f obj/*.o lib/libantelope_engine.a` before rebuild — the engine has no header dependency tracking, stale objects link an inconsistent archive.
- Build a test: `make <name>` (e.g. `make test_v6_filter_underfill`) → `bin/<name>`; run `bin/<name>`, exit 0 on pass. `source/*.cpp` and `tests/*.cpp` are auto-discovered — no makefile edit needed for a new test file.
- The `ls: cannot access 'tools/*.cpp'` build line is harmless.

## File Structure

- `source/token_index.h` — add 4 scratch members + a private helper decl to `ANT_token_index` (#17).
- `source/token_index.cpp` — rewrite `search_candidates` body to epoch-stamped scratch (#17).
- `atire/atire_segment_index.h` — declare `multivector_scan_segment_exact` private helper (#16).
- `atire/atire_segment_index_vector.cpp` — extract the exact-scan helper + add the under-fill top-up in `multivector_candidates` (#16).
- `tests/test_v6_token_scratch.cpp` — new (#17 behavior-neutrality across repeated calls).
- `tests/test_v6_filter_underfill.cpp` — new (#16 selective-filter under-fill regression with a built `.tann`).

---

## Task 1: #17 — epoch-stamped candidate scratch

**Files:**
- Modify: `source/token_index.h` (add scratch members)
- Modify: `source/token_index.cpp:160-204` (`search_candidates`)
- Test: `tests/test_v6_token_scratch.cpp` (new)

- [ ] **Step 1: Write the failing behavior-neutrality test**

Create `tests/test_v6_token_scratch.cpp`. It builds a small `.tann` segment, then runs the SAME query twice and TWO different queries in sequence on one index, asserting identical/correct top-k each time — a missed per-call scratch reset would corrupt the second call. (Written before the refactor, it passes on the current code too; it is the lock that the refactor must not break. Its real value is catching a bad reset in Step 3, so keep it and re-run after.)

```cpp
/*
	TEST_V6_TOKEN_SCRATCH.CPP -- #17: search_candidates reuses epoch-stamped
	scratch across calls. Locks behavior-neutrality: repeated and interleaved
	queries on one index must each return the correct top-k (a stale scratch
	reset would corrupt the 2nd+ call).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"
#include "../source/filter.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_v6_scratch_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

// dim=4 single-token docs; dot-product order is hand-computable.
static ATIRE_segment_index *build_fixture(char **dir_out)
{
char *dir = make_index_dir();
*dir_out = dir;
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

struct { const char *key; float v[4]; } docs[5] =
	{
	{ "d0", {1.0f, 0.0f, 0.0f, 0.0f} },
	{ "d1", {0.9f, 0.1f, 0.0f, 0.0f} },
	{ "d2", {0.0f, 1.0f, 0.0f, 0.0f} },
	{ "d3", {0.1f, 0.9f, 0.0f, 0.0f} },
	{ "d4", {0.0f, 0.0f, 1.0f, 0.0f} },
	};
for (long long i = 0; i < 5; i++)
	{
	char doc[64]; sprintf(doc, "<DOC>%s</DOC>", docs[i].key);
	CHECK(ix->add_document(docs[i].key, doc, NULL, docs[i].v, 1) >= 0);
	}
CHECK(ix->flush() == 0);
CHECK(ix->build_token_index() == 0);   // segment gets a .tann -> token-ANN path active
return ix;
}

int main(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

float qa[4] = {1.0f, 0.0f, 0.0f, 0.0f};   // nearest: d0 then d1
float qb[4] = {0.0f, 1.0f, 0.0f, 0.0f};   // nearest: d2 then d3

// same query twice -> identical top hit both times (scratch reused, not corrupted)
CHECK(ix->search_multivector(qa, 1, 2) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);
CHECK(ix->search_multivector(qa, 1, 2) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);

// interleave a different query -> its own correct top hit (no carry-over from qa)
CHECK(ix->search_multivector(qb, 1, 2) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "d2") == 0);

// back to qa -> still correct
CHECK(ix->search_multivector(qa, 1, 2) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);

delete ix;
delete [] dir;
printf("ALL TESTS PASSED\n");
return 0;
}
```

- [ ] **Step 2: Build and run it against current code (baseline PASS)**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_v6_token_scratch && ./bin/test_v6_token_scratch
```
Expected: `ALL TESTS PASSED` (locks current behavior; the refactor must keep this green).

- [ ] **Step 3: Add the epoch-stamped scratch members to `ANT_token_index`**

In `source/token_index.h`, inside the `private:` section (after `ANT_token_source *source;`, before `ANT_token_index();`), add — note the non-reentrancy comment:

```cpp
	// #17: reusable candidate scratch (sized `documents`), reset lazily per touched
	// doc via scratch_epoch instead of O(documents) zeroing each search_candidates call.
	// Makes search_candidates NON-reentrant per instance -- consistent with the engine's
	// single-threaded-search model (shared results buffer) and ANT_hnsw's visited_epoch.
	std::vector<double>    scratch_provisional;    // valid where scratch_touched_epoch[d]==scratch_epoch
	std::vector<long long> scratch_seen_query;     // query token that last contributed to d (this epoch)
	std::vector<long long> scratch_touched_epoch;  // last epoch d was touched (replaces the per-call in_touched)
	long long              scratch_epoch;          // bumped once per search_candidates call
```

`token_index.h` does not currently include `<vector>`; add `#include <vector>` near the top of the header (after the include guard, before the forward declarations) so the member types resolve.

Initialize `scratch_epoch = 0;` in the constructor. In `source/token_index.cpp`, the constructor body (~line 12) currently sets `graph = NULL; token_docid = NULL; ...`. Append `scratch_epoch = 0;` to that initializer line (the three vectors default-construct empty).

- [ ] **Step 4: Rewrite `search_candidates` to use the scratch (no per-call O(documents) work)**

Replace the body of `search_candidates` (`source/token_index.cpp:167-203`, from the three `std::vector` scratch declarations through the output loop) with the epoch-stamped version. The early-guard at 164-165 stays unchanged above this.

```cpp
// lazily size the reusable scratch to `documents` (once; grows only if documents grows)
if ((long long)scratch_touched_epoch.size() < documents)
	{
	scratch_provisional.assign((size_t)documents, 0.0);
	scratch_seen_query.assign((size_t)documents, -1);
	scratch_touched_epoch.assign((size_t)documents, 0);   // 0 != any epoch>=1 -> nothing looks touched
	}
scratch_epoch++;                                          // new call -> all prior touches invalidated

std::vector<long long> touched;                           // O(candidates), not O(documents)
std::vector<long long> tok_docids((size_t)token_top_p);
std::vector<double>    tok_scores((size_t)token_top_p);

for (long long i = 0; i < num_query_vecs; i++)
	{
	const float *q = query + i * dimension;
	// tombstones/filter passed as NULL: nodes are tokens, not docids -> admit at doc level below
	long long got = graph->search(q, metric, /*ef_search=*/token_top_p, token_top_p,
		source, /*tombstones=*/NULL, tok_docids.data(), tok_scores.data(), /*filter_bits=*/NULL);
	for (long long r = 0; r < got; r++)
		{
		long long t = tok_docids[r];               // token id
		if (t < 0 || t >= token_count) continue;
		long long d = token_docid[t];              // owning docid
		if (d < 0 || d >= documents) continue;
		if (tombstones != NULL && tombstones->is_deleted(d)) continue;
		if (filter_bits != NULL && !(filter_bits[d >> 3] & (1 << (d & 7)))) continue;
		if (scratch_touched_epoch[d] != scratch_epoch)   // first time this doc appears this call
			{
			scratch_touched_epoch[d] = scratch_epoch;
			scratch_provisional[d]   = 0.0;
			scratch_seen_query[d]    = -1;
			touched.push_back(d);
			}
		// results are descending by kernel, so the FIRST hit for this (query token, doc) is its best
		if (scratch_seen_query[d] == i) continue;        // already took this query token's best for d
		scratch_seen_query[d] = i;
		scratch_provisional[d] += tok_scores[r];
		}
	}

long long out_n = (long long)touched.size();
if (out_n > max_candidates) out_n = max_candidates;
std::partial_sort(touched.begin(), touched.begin() + out_n, touched.end(),
	[&](long long a, long long b){ return scratch_provisional[a] > scratch_provisional[b]; });
for (long long j = 0; j < out_n; j++)
	out_docids[j] = touched[j];
return out_n;
```

Note the comparator now reads `scratch_provisional` (was `provisional`). No other function references the old locals.

- [ ] **Step 5: Rebuild and re-run the neutrality test + the existing token-ANN suite**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_v6_token_scratch && ./bin/test_v6_token_scratch
make test_v6_search_multivector && ./bin/test_v6_search_multivector
make test_v6_filter && ./bin/test_v6_filter
make test_v6_token_index && ./bin/test_v6_token_index
```
Expected: all print `ALL TESTS PASSED` — identical results, proving the scratch reuse is behavior-neutral.

- [ ] **Step 6: ASan/UBSan sweep on the changed path**

Run (the engine's ASan build; exclude the known out-of-scope `ANT_file::setvbuff` leak):
```bash
rm -f obj/*.o lib/libantelope_engine.a
make CFLAGS_EXTRA="-fsanitize=address,undefined -g" test_v6_token_scratch 2>/dev/null || make test_v6_token_scratch
ASAN_OPTIONS=detect_leaks=1 ./bin/test_v6_token_scratch
```
(If the makefile has no `CFLAGS_EXTRA` hook, use the project's established ASan invocation — grep the makefile / prior test notes; report if the harness isn't available. After an ASan build, a full `rm -f obj/*.o lib/libantelope_engine.a` clean rebuild is required before a normal non-ASan link.)
Expected: no new ASan/UBSan reports on the scratch path.

- [ ] **Step 7: Commit**

```bash
git add source/token_index.h source/token_index.cpp tests/test_v6_token_scratch.cpp
git commit -m "perf(v6): epoch-stamped candidate scratch, no per-query O(documents) alloc (#17)"
```

---

## Task 2: #16 — filtered completeness via brute-force top-up

**Files:**
- Modify: `atire/atire_segment_index.h` (declare the helper, near line 228-229)
- Modify: `atire/atire_segment_index_vector.cpp:2523-2604` (`multivector_candidates`)
- Test: `tests/test_v6_filter_underfill.cpp` (new)

- [ ] **Step 1: Write the failing under-fill regression test**

Create `tests/test_v6_filter_underfill.cpp`. Fixture: `set_candidate_multiplier(1)` (so filtered `eff_top_p = token_top_p*1 = 32`), then **40 "beta" docs** clustered at the query direction `(1,0,0,0)` (distinct tiny perturbations) plus **3 "acme" docs** at the orthogonal direction `(0,1,0,0)`. Build the `.tann`. Query `(1,0,0,0)`, filter `tenant=="acme"`, `top_k=3`: the 32 nearest tokens are all beta, so the token-ANN path admits 0 acme (`n=0`) — pre-fix it under-fills to 0; post-fix the brute-force top-up returns all 3 acme. The fail-first at n=0 also proves the `.tann` token path (not brute-force) is active.

```cpp
/*
	TEST_V6_FILTER_UNDERFILL.CPP -- #16: filtered search_multivector must not
	under-fill a .tann segment. 40 "beta" docs cluster at the query direction
	and 3 "acme" docs sit orthogonal to it; with candidate_multiplier=1 the
	token-ANN nearest-32 set is entirely beta, so the token path alone admits
	zero acme under a tenant=="acme" filter. The brute-force top-up must still
	return the full top_k=3 of acme docs -- the same result the no-.tann
	(brute-force) path yields.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"
#include "../source/filter.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_v6_underfill_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

static ATIRE_segment_index *build_fixture(char **dir_out)
{
char *dir = make_index_dir();
*dir_out = dir;
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
ix->set_candidate_multiplier(1);   // filtered eff_top_p = token_top_p(32)*1 -> 32 nearest tokens

ANT_attribute_schema s;
s.add_field("tenant", ANT_attribute_schema::TYPE_STRING, 0);
CHECK(ix->set_attributes_config(s) == 0);

// 40 beta docs clustered near (1,0,0,0); tiny distinct perturbations keep them the nearest tokens
for (int i = 0; i < 40; i++)
	{
	float v[4] = { 1.0f, 0.001f * (float)i, 0.0f, 0.0f };
	char key[32]; sprintf(key, "beta_%02d", i);
	char doc[64]; sprintf(doc, "<DOC>%s</DOC>", key);
	ANT_attribute_set A(ix->attribute_schema());
	A.set_string(0, "beta");
	CHECK(ix->add_document(key, doc, NULL, v, 1, &A) >= 0);
	}
// 3 acme docs at the orthogonal direction (0,1,0,0) -> far from the query, never in the nearest-32
struct { const char *key; float v[4]; } acme[3] =
	{
	{ "acme_a", {0.0f, 1.0f,  0.0f,   0.0f} },
	{ "acme_b", {0.0f, 0.99f, 0.10f,  0.0f} },
	{ "acme_c", {0.0f, 0.99f, 0.0f,   0.10f} },
	};
for (int i = 0; i < 3; i++)
	{
	char doc[64]; sprintf(doc, "<DOC>%s</DOC>", acme[i].key);
	ANT_attribute_set A(ix->attribute_schema());
	A.set_string(0, "acme");
	CHECK(ix->add_document(acme[i].key, doc, NULL, acme[i].v, 1, &A) >= 0);
	}
CHECK(ix->flush() == 0);
CHECK(ix->build_token_index() == 0);   // build .tann -> token-ANN path active for this segment
return ix;
}

static void test_selective_filter_no_under_fill(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
ANT_filter *f = ANT_filter::eq_string("tenant", "acme");
CHECK(f->build(ix->attribute_schema()) == 0);

// 3 acme docs match; token-ANN alone returns 0 (all nearest-32 are beta) -> top-up must fill to 3
long long n = ix->search_multivector(q, 1, 3, f);
CHECK(n == 3);
for (long long i = 0; i < n; i++)
	CHECK(strncmp(ix->get_hit(i)->filename, "acme_", 5) == 0);

delete f;
delete ix;
delete [] dir;
printf("test_selective_filter_no_under_fill OK\n");
}

static void test_unfiltered_ann_unchanged(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

// unfiltered small top_k: pure ANN path (no top-up, fbits==NULL) still returns top hits, nearest first
float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
long long n = ix->search_multivector(q, 1, 5);
CHECK(n == 5);
for (long long i = 0; i < n; i++)
	CHECK(strncmp(ix->get_hit(i)->filename, "beta_", 5) == 0);   // all 5 nearest are beta
CHECK(strcmp(ix->get_hit(0)->filename, "beta_00") == 0);         // exact (1,0,0,0) is the max-dot doc

delete ix;
delete [] dir;
printf("test_unfiltered_ann_unchanged OK\n");
}

int main(void)
{
test_selective_filter_no_under_fill();
test_unfiltered_ann_unchanged();
printf("ALL TESTS PASSED\n");
return 0;
}
```

- [ ] **Step 2: Build and run it to watch #16 FAIL (under-fill)**

Run:
```bash
make test_v6_filter_underfill && ./bin/test_v6_filter_underfill
```
Expected: FAIL at `CHECK(n == 3)` in `test_selective_filter_no_under_fill` — the token-ANN path returns 0 acme (n==0), proving both that the `.tann` token path is active and that it under-fills under the selective filter. (`test_unfiltered_ann_unchanged` should already PASS.)

- [ ] **Step 3: Declare the exact-scan helper in the header**

In `atire/atire_segment_index.h`, in the `private:` section right after the `multivector_candidates` declaration (line 229), add:

```cpp
		// #16: exact filtered MaxSim scan of one flushed segment into best[] (shared by the
		// no-.tann branch and the token-ANN under-fill top-up). fbits: per-segment match bitset or NULL.
		void multivector_scan_segment_exact(long long which, const float *qn, long long num_query_vecs,
			long long top_k, ANT_vector_candidate *best, long long *best_count,
			ANT_multivector_store *mv, ANT_multivector_pq_store *pqs, long use_pq, const unsigned char *fbits);
```

- [ ] **Step 4: Extract the helper and add the top-up in `multivector_candidates`**

In `atire/atire_segment_index_vector.cpp`, define the helper just above `multivector_candidates` — its body is the exact-scan loop currently in the `else` branch (lines ~2572-2583):

```cpp
void ATIRE_segment_index::multivector_scan_segment_exact(long long which, const float *qn, long long num_query_vecs,
	long long top_k, ANT_vector_candidate *best, long long *best_count,
	ANT_multivector_store *mv, ANT_multivector_pq_store *pqs, long use_pq, const unsigned char *fbits)
{
long long docs = segments[which].engine->get_document_count();
for (long long did = 0; did < docs; did++)
	{
	if (mv != NULL ? !mv->has(did) : !pqs->has(did))
		continue;
	if (segments[which].tombstones != NULL && segments[which].tombstones->is_deleted(did))
		continue;
	if (fbits != NULL && !(fbits[did >> 3] & (1 << (did & 7))))
		continue;
	ANT_vector_candidate_insert(best, best_count, top_k, (use_pq ? pqs->maxsim(did, qn, num_query_vecs) : mv->maxsim(did, qn, num_query_vecs)), segments[which].generation, did);
	}
}
```

Then rewrite the token-ANN / else block inside `multivector_candidates` (lines ~2549-2584) to:

```cpp
	if (segments[which].token_index != NULL && !segments[which].token_index->empty())
		{
		/* token-ANN shortlist; filtered queries widen the pool to compensate for
		   doc-level admission happening post-hoc. If a selective filter still leaves
		   the segment short of top_k (under-fill), fall through to the exact scan
		   below -- it finds every matching doc (a superset), so no completeness gap. */
		long long eff_top_p = (fbits != NULL) ? token_top_p * candidate_multiplier : token_top_p;
		long long eff_pool  = (fbits != NULL) ? pool_size * candidate_multiplier : pool_size;
		long long *cand = new long long[eff_pool > 0 ? eff_pool : 1];
		long long n = segments[which].token_index->search_candidates(qn, num_query_vecs, eff_top_p, eff_pool, segments[which].tombstones, fbits, cand);

		if (fbits != NULL && n < top_k)
			multivector_scan_segment_exact(which, qn, num_query_vecs, top_k, best, &best_count, mv, pqs, use_pq, fbits);
		else
			for (long long p = 0; p < n; p++)
				{
				long long did = cand[p];
				if (mv != NULL ? !mv->has(did) : !pqs->has(did))
					continue;   /* tombstone+filter already applied by search_candidates */
				ANT_vector_candidate_insert(best, &best_count, top_k, (use_pq ? pqs->maxsim(did, qn, num_query_vecs) : mv->maxsim(did, qn, num_query_vecs)), segments[which].generation, did);
				}
		delete [] cand;
		}
	else
		multivector_scan_segment_exact(which, qn, num_query_vecs, top_k, best, &best_count, mv, pqs, use_pq, fbits);
```

Confirm the old inline `else`-branch scan body (the `for (did...)` loop) is fully replaced by the helper call — no duplicated scan remains. `pool_size` and `best_count` are the existing locals in `multivector_candidates`; the helper takes `&best_count`.

- [ ] **Step 5: Rebuild and run the #16 test (now PASSES) + the token-ANN suite**

Header changed (`atire_segment_index.h`) → clean rebuild:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_v6_filter_underfill && ./bin/test_v6_filter_underfill
make test_v6_filter && ./bin/test_v6_filter
make test_v6_search_multivector && ./bin/test_v6_search_multivector
make test_v6_rerank_fallback && ./bin/test_v6_rerank_fallback
make test_v6_token_scratch && ./bin/test_v6_token_scratch
```
Expected: all `ALL TESTS PASSED`. The under-fill test now returns the full `top_k=3` acme; the existing filtered/unfiltered suites are unchanged.

- [ ] **Step 6: ASan/UBSan sweep**

Run the project's ASan build over the two new tests (as in Task 1 Step 6), excluding the known `ANT_file::setvbuff` leak. Expected: no new reports (the helper does no new allocation; the top-up reuses the existing scan). Then a full `rm -f obj/*.o lib/libantelope_engine.a` clean rebuild to restore a normal link.

- [ ] **Step 7: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index_vector.cpp tests/test_v6_filter_underfill.cpp
git commit -m "fix(v6): brute-force top-up on filtered token-ANN under-fill (#16)"
```

---

## Self-review notes

- **Spec coverage:** #17 → Task 1 (epoch scratch + neutrality test); #16 → Task 2 (helper extraction + `fbits && n < top_k` top-up + under-fill regression). Both acceptance criteria mapped: #16 full top_k under selective filter with `.tann` built (Task 2 Step 5); #17 no per-query O(documents) alloc + identical results (Task 1 Steps 4-5).
- **Sequencing:** #17 first so #16's new fallback is written against the final `search_candidates` (spec §4).
- **Type consistency:** helper name `multivector_scan_segment_exact` identical in header decl (Task 2 Step 3) and definition/calls (Step 4). Scratch member names `scratch_provisional` / `scratch_seen_query` / `scratch_touched_epoch` / `scratch_epoch` identical across header (Task 1 Step 3) and body (Step 4).
- **No double-insertion (#16):** the top-up REPLACES the token-ANN insert loop (else-branch of `if (fbits && n < top_k)`), never runs both — brute-force is a superset, `ANT_vector_candidate_insert` has no docid dedupe.
- **Line numbers are indicative** — implementer confirms by grep before editing (repo constraint).

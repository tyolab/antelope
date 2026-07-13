# V6 Token-ANN Filtered Completeness + Candidate-Scratch Perf Design

**Status:** proposed 2026-07-13, pending user review. Issues **#16** (Important — filtered token-ANN under-fill) + **#17** (Minor — per-query O(documents) scratch). Bundled: same file (`source/token_index.cpp` + its one caller `atire/atire_segment_index_vector.cpp`), same V6-review origin. *(Two framing decisions — brute-force top-up for #16, and bundling — were my recommended defaults chosen while the user was away; confirm at the review gate.)*

**Goal:** (#16) Guarantee filtered `search_multivector` returns a full `top_k` whenever ≥`top_k` matching docs exist in a segment, regardless of whether a `.tann` token-ANN graph is built — closing the best-effort under-fill gap. (#17) Remove the per-segment, per-query O(documents) scratch allocation/zeroing in `ANT_token_index::search_candidates`.

**Architecture (one sentence):** In `multivector_candidates`, detect token-ANN under-fill under an active filter and fall through to the already-present exact brute-force scan for that segment (a superset, so replace-not-append); and in `search_candidates`, replace the three `documents`-sized per-call vectors with epoch-stamped reusable buffers reset lazily per touched doc (mirroring `ANT_hnsw`'s `visited_epoch`).

**Tech stack:** C++ engine (`source/token_index.{h,cpp}`, `atire/atire_segment_index_vector.cpp`), tests `tests/*.cpp` (auto-discovered, `bin/<name>` via `make <name>`, `CHECK()`).

**Scope:** engine-only, two co-located changes. Behavior-neutral for #17 (identical candidates); recall-improving + behavior-preserving-on-non-selective-filters for #16.

---

## 1. #16 — filtered completeness via brute-force top-up (`atire/atire_segment_index_vector.cpp`)

**Today** (`multivector_candidates`, ~2549–2569): when a segment has a built `token_index`, filtered queries widen `eff_top_p`/`eff_pool` by `candidate_multiplier` and call `search_candidates(..., fbits, cand)`, then insert each returned candidate. This is best-effort: under a selective filter, matching docs whose tokens aren't among the widened nearest set are missed → the segment can return `< top_k` even when ≥`top_k` matching docs exist. The `else` branch (~2570–2584) is the **exact brute-force scan** (iterate all docs, apply tombstone+filter, insert) already used by segments with no `.tann`.

**Fix:** when a filter is active and the token-ANN candidate count for a segment falls short of `top_k`, use the exact brute-force scan for that segment instead of the token-ANN candidates. The brute-force scan finds every matching doc — a **superset** of the token-ANN matches — so it *replaces* (not appends to) the token-ANN contribution, avoiding double-insertion into the shared `best` top-k heap.

**Structure.** Extract the existing per-segment exact scan into a small helper to keep it DRY:
```cpp
// exact filtered MaxSim scan of one flushed segment into `best` (the current `else`-branch body)
void ATIRE_segment_index::multivector_scan_segment_exact(long long which, const float *qn, long long num_query_vecs,
    long long top_k, ANT_vector_candidate *best, long long *best_count, ANT_multivector_store *mv,
    ANT_multivector_pq_store *pqs, long use_pq, const unsigned char *fbits);
```
Then the token_index branch becomes:
```cpp
if (segments[which].token_index != NULL && !segments[which].token_index->empty())
    {
    long long eff_top_p = (fbits != NULL) ? token_top_p * candidate_multiplier : token_top_p;
    long long eff_pool  = (fbits != NULL) ? pool_size * candidate_multiplier : pool_size;
    long long *cand = new long long[eff_pool > 0 ? eff_pool : 1];
    long long n = segments[which].token_index->search_candidates(qn, num_query_vecs, eff_top_p, eff_pool, segments[which].tombstones, fbits, cand);

    if (fbits != NULL && n < top_k)
        // token-ANN under-filled under a selective filter: the exact scan is a superset -> use it instead
        multivector_scan_segment_exact(which, qn, num_query_vecs, top_k, best, &best_count, mv, pqs, use_pq, fbits);
    else
        for (long long p = 0; p < n; p++)
            {
            long long did = cand[p];
            if (mv != NULL ? !mv->has(did) : !pqs->has(did)) continue;
            ANT_vector_candidate_insert(best, &best_count, top_k, (use_pq ? pqs->maxsim(did, qn, num_query_vecs) : mv->maxsim(did, qn, num_query_vecs)), segments[which].generation, did);
            }
    delete [] cand;
    }
else
    multivector_scan_segment_exact(which, qn, num_query_vecs, top_k, best, &best_count, mv, pqs, use_pq, fbits);
```
The `else` (no-`.tann`) branch now also calls the same helper — one exact-scan implementation, two call sites.

**Trigger rationale.** `n` is the tombstone+filter-applied candidate count from `search_candidates`. `n < top_k` (only when `fbits != NULL`) means the token-ANN path could not supply a full `top_k` under the filter → fall back for completeness. Unfiltered queries (`fbits == NULL`) are unchanged — pure ANN. When there are genuinely `< top_k` matching docs, brute-force returns exactly that set (still complete). Cost: the exact scan runs only under selective filters where the token-ANN was already unreliable — the tradeoff the issue names.

**Comment update.** Replace the "best-effort, NOT a no-under-fill guarantee" comment block with the completeness guarantee (brute-force top-up on under-fill).

**Acceptance:** filtered `search_multivector` returns a full `top_k` whenever ≥`top_k` matching docs exist in a segment, whether or not `.tann` is built; unfiltered results byte-identical to today.

## 2. #17 — epoch-stamped candidate scratch (`source/token_index.{h,cpp}`)

**Today** (`search_candidates`, ~167–170): three `documents`-sized containers allocated + initialized every call — `provisional` (double, 0.0), `in_touched` (char, 0), `seen_query` (long long, -1) — plus the small `touched` working list. Per segment, per query.

**Fix:** make the three `documents`-sized buffers **reusable members**, reset lazily via a per-call epoch stamp (mirroring `ANT_hnsw::build`'s `visited_epoch`). Add to `ANT_token_index`:
```cpp
std::vector<double>    scratch_provisional;   // sized `documents`, valid where touched_epoch[d]==scratch_epoch
std::vector<long long> scratch_seen_query;    // which query token last contributed (valid this epoch)
std::vector<long long> scratch_touched_epoch; // last epoch a doc was touched (replaces in_touched)
long long              scratch_epoch = 0;
```
Allocate once (lazily on first `search_candidates`, sized to `documents`; `touched_epoch` init to 0 so no doc matches epoch>0 initially). Each call bumps `scratch_epoch` and resets nothing O(documents); the loop body resets per touched doc:
```cpp
scratch_epoch++;
std::vector<long long> &touched = ...;   // local, O(candidates)
...
long long d = token_docid[t];
if (d < 0 || d >= documents) continue;
if (tombstones && tombstones->is_deleted(d)) continue;
if (filter_bits && !(filter_bits[d>>3] & (1<<(d&7)))) continue;
if (scratch_touched_epoch[d] != scratch_epoch)   // first time this doc appears this call
    {
    scratch_touched_epoch[d] = scratch_epoch;
    scratch_provisional[d]   = 0.0;
    scratch_seen_query[d]    = -1;
    touched.push_back(d);
    }
if (scratch_seen_query[d] == i) continue;         // already took this query token's best for d
scratch_seen_query[d] = i;
scratch_provisional[d] += tok_scores[r];
```
`partial_sort` and output loop read `scratch_provisional` exactly as before. `touched` stays a per-call local (its size is O(candidates), not O(documents) — no cost concern). `scratch_epoch` is `long long`; wrap after 2^63 calls is not a practical concern (optionally, on wrap, re-zero `touched_epoch` — a one-line guard, YAGNI otherwise).

**Reentrancy note.** Shared scratch makes `search_candidates` non-reentrant per `ANT_token_index` instance — consistent with the engine's existing single-threaded-search assumption (the per-instance results buffer is already not thread-safe; the Python/MCP layer serializes with a lock) and with `ANT_hnsw`'s `visited_epoch`. Document it at the buffer declarations.

**Acceptance:** no per-query O(documents) allocation/zeroing on the candidate path; identical candidate results (behavior-neutral); a microbenchmark showing the win on a many-segment index.

## 3. Testing

- **#16 regression** (`tests/`): build a segment with a `.tann` token index; add ≥`top_k` docs matching a selective filter whose tokens are deliberately NOT the globally-nearest to the query tokens (so the token-ANN nearest set misses them); assert filtered `search_multivector` (or `search_rerank`/the token path) returns a full `top_k` of matching docs — the same result the brute-force (no-`.tann`) path yields. A companion assert on an unfiltered query confirms the ANN path is unchanged.
- **#17 behavior-neutrality:** an existing token-ANN search test (or a new one) asserting identical candidate/result ordering before vs. the epoch-stamp change; run repeated searches on one index to exercise scratch reuse across calls (catch a missed reset). A lightweight timing/allocation check (or reasoning in the plan) demonstrates the removed O(documents) work; a full microbenchmark is optional if a timing harness isn't readily available (report if environment-blocked).
- ASan/UBSan sweep on the new/changed paths (known out-of-scope `ANT_file::setvbuff` leak excluded).

## 4. Sequencing (TDD tasks)

1. **#17 scratch:** add the epoch-stamped member buffers to `ANT_token_index`, rewrite `search_candidates` to lazy-reset; behavior-neutrality test (identical results across repeated calls). *(Do #17 first — it's behavior-preserving and isolated, so #16's new fallback is written against the final `search_candidates`.)*
2. **#16 completeness:** extract `multivector_scan_segment_exact` helper (both call sites), add the `fbits && n < top_k` brute-force top-up, update the comment; selective-filter under-fill regression test (full `top_k` with vs. without `.tann`).

## 5. Repo constraints

Header changes → `rm -f obj/*.o lib/libantelope_engine.a` before rebuild (no header dep tracking); fresh worktree needs `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered, tests → `bin/<name>` via `make <name>`; config setters POST-open; `ANT_vector_candidate_insert` is a bounded top-k heap insert with no docid dedupe (hence replace-not-append in #16); `token_docid[t]` maps token→owning docid; `evaluate_filter_for_segment` returns a per-segment docid bitset (NULL when no filter), caller frees. Confirm exact signatures/line numbers by grep before editing (line numbers here are indicative).

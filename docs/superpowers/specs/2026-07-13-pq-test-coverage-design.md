# PQ Test Coverage Gaps Design

**Status:** approved 2026-07-13, ready for implementation planning. Issue #21.

**Goal:** Close the two Phase-1-review test-coverage Minors (M2, M3) with new/extended tests that lock behavior the shipped code already handles. Test additions plus one tiny enabler: two public one-line getters (`pq_posture()`, `pq_rerank_quant()`) mirroring the existing `pq_m()`, since the posture/quant members are currently private and M3 needs to observe their round-trip.

**Architecture (one sentence):** Add a focused new `tests/test_pq_metrics.cpp` for cosine/L2 end-to-end PQ search (both postures + the mixed PQ/float-fallback ordering), and extend two existing tests (`test_pq_store.cpp` forgiving-load, `test_pq_config.cpp` persistence) with the missing cases.

**Tech stack:** C++ engine tests (`tests/*.cpp`, auto-discovered, built to `bin/<name>` via `make <name>`, `CHECK()` macro). Exercises the shipped dense-PQ stack (`ANT_pq_store`, `vector_candidates_pq`/`_rerank`, `ANT_pq_codec::adc_table`, `load_pq_config`).

**Scope:** engine test-only. If any new case reveals a real defect in shipped code, that is a finding to fix (not expected — the review classed these as Minors because the code handles them; the tests are locks).

---

## 1. M2 — cosine/L2 end-to-end (`tests/test_pq_metrics.cpp`, new)

The existing `test_pq_search`/`recall`/`rerank` all use `VECTOR_METRIC_DOT`, leaving untested: the cosine-normalize branches in `vector_candidates_pq`/`vector_candidates_pq_rerank`, and the L2 sign convention in `adc_table()` (negated squared distance → "higher = better", which must stay consistent when a not-yet-built segment falls back to `ANT_vector_store::scan` mid-query). Add, for **each of `VECTOR_METRIC_COSINE` and `VECTOR_METRIC_L2`**:

- **Replace posture recall:** build an exact float index (no PQ) and a PQ index (`set_pq_config(m, PQ_POSTURE_REPLACE, RERANK_QUANT_FLOAT)`) over identical planted data; for a fixed query, assert the planted-nearest doc appears in the PQ index's `search_vector(q, k)` top-k (recall floor — replace-ADC is approximate). Mirrors the existing DOT `test_pq_search` structure but with the cosine/L2 metric.
- **Rerank posture exactness:** same data with `PQ_POSTURE_RERANK`; assert the PQ index's top-1 equals the exact float index's top-1 (rerank rescores through the resident float tier → exact).
- **Mixed PQ/float-fallback ordering:** one index, PQ configured; add docs and `flush()` (segment 0), `build_pq()` (segment 0 gets `.pq`), then add more docs and `flush()` again (segment 1 has NO `.pq` yet). A single `search_vector(q, k)` then scores segment 0 via ADC (`pq_vectors`) and segment 1 via the per-segment float fallback (`ANT_vector_store::scan`); assert the merged top-k recalls the planted nearest across BOTH segments — proving the L2 negated-squared-distance sign keeps ADC scores and float-scan scores comparable in one merge. Plant the global nearest in segment 1 (the float-fallback segment) so a sign/scale mismatch between the two paths would demote it out of the top-k and fail the test.

Data/query generators mirror `test_pq_search.cpp` (small `DIM`, a handful of docs, `add_document(key, body, vec)` 3-arg dense form, `get_hit(i)->filename`). Use `disk_segment_has_pq(which)` to confirm the intended per-segment `.pq` presence in the mixed case.

## 2. M3 — store + config gaps

**`tests/test_pq_store.cpp` (`forgiving_load`):** it covers missing/truncated/wrong-dim/wrong-docs but not:
- **Bad magic:** write a `.pq` file whose 8-byte magic is wrong (otherwise well-formed header) → `ANT_pq_store::load(path, dim, docs, metric)` returns a degraded empty store (`document_count()==0`).
- **Size-consistent m/dim mismatch:** craft a header whose `m`/`dimension` differ from the `expected_dimension` passed to `load` but whose on-disk size still matches what that header implies → `load` must reject on the `stored_dimension != expected_dimension` (or `stored_m` divisibility) check, degrading to empty, NOT mis-parse. (Confirms the exact-size gate is not the only guard.) Write it via `ANT_pq_store_writer` at one (dim,m) then `load` with a different expected dim, or hand-write the header — whichever cleanly hits a size-consistent mismatch.

**`atire/atire_segment_index.h` (the one production change):** add two public inline getters beside `pq_m()` — `long pq_posture(void) { return pq_posture_current; }` and `long pq_rerank_quant(void) { return pq_rerank_quant_current; }` — mirroring the existing `pq_m()`/`pq_resident_tier()` accessors (the members are otherwise private, so posture/quant persistence is unobservable from a test).

**`tests/test_pq_config.cpp`:** the persistence test asserts `pq_m()` round-trips but not posture/rerank_quant. Add a case: `set_pq_config(m, PQ_POSTURE_RERANK, RERANK_QUANT_INT8)` (a non-default posture+quant; note V4 int8 must not be enabled — PQ and V4 int8 are mutually exclusive, and `RERANK_QUANT_INT8` here is the PQ rerank-tier quant, valid), `flush`, close, reopen, and assert `pq_posture() == PQ_POSTURE_RERANK` and `pq_rerank_quant() == RERANK_QUANT_INT8` (restored by `load_pq_config`).

## 3. Testing / success

All new cases build and PASS against the current master (they lock already-correct behavior). Run under ASan/UBSan (the known out-of-scope `ANT_file::setvbuff` leak excluded). The only production change is the two inline getters in §2 (header change → `rm -f obj/*.o lib/libantelope_engine.a` before rebuild); if a test case fails, investigate — a genuine defect is fixed in the smallest relevant source file and noted, otherwise the test is corrected to match the real (correct) contract.

## 4. Sequencing (TDD tasks)

1. **M2:** `tests/test_pq_metrics.cpp` — cosine + L2 × {replace recall, rerank exact} + the mixed PQ/float-fallback ordering case.
2. **M3:** extend `test_pq_store.cpp` `forgiving_load` (bad-magic + size-consistent mismatch) and `test_pq_config.cpp` (posture+quant round-trip) + ASan/UBSan sweep on the new/changed tests.

## 5. Repo constraints

`source/*.cpp`+`tests/*.cpp` auto-discovered; tests build to `bin/<name>` via `make <name>` (`CHECK()`, exit 0 on pass); config setters POST-open, `set_vector_config` PRE-open; `add_document(key, body, vec)` 3-arg dense; `.pq` magic/header via the `ANT_PQ_STORE_*` constants (file-static in `pq_store.cpp` — hand-write literals in a store test if not header-visible, grepping the real magic string); `default_pq_m(dim)` = largest divisor ≤16 (pick `m` that divides `DIM`); confirm accessor names (`pq_configured`/`pq_m`/`pq_posture`/`pq_rerank_quant`/`disk_segment_has_pq`) by grep before use.

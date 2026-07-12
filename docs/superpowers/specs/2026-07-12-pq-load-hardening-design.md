# PQ Load Hardening Design

**Status:** approved 2026-07-12, ready for implementation planning. Issue #25.

**Goal:** Close two non-blocking hardening Minors from the PQ Phase-2 review, on BOTH the dense `.pq` and token `.mvpq` paths, so the load paths validate consistently: (1) revalidate the persisted PQ config after read (m divides the dimension; posture/quant/tier in range) and leave PQ unconfigured on mismatch; (2) bound the crafted-header token count before the `expected_size` multiply to remove the signed-overflow UB.

**Architecture (one sentence):** Two small, localized changes — an in-place revalidation added to the two `load_*pq_config` functions (with a one-line `open()` reorder so the dimension is available when the token config validates), and an upper-bound check on the header count in the two store `load()` functions before any size multiply — each mirroring patterns already present in the shipped code.

**Tech stack:** C++ engine (`atire/atire_segment_index*.cpp`, `source/pq_store.cpp`, `source/multivector_pq_store.cpp`). No new files, no interface changes.

**Scope:** engine-only, load paths only. Both issues are Minors (contained downstream: a bad config silently disables PQ → float fallback; a crafted header degrades the store to empty). No behavior change for any valid file.

---

## 1. Config revalidation (issue #25.1)

**Ordering fix (`open()` in `atire/atire_segment_index.cpp`, ~lines 388-390):** the persisted-config loads currently run `load_pq_config()` (388) → `load_multivector_pq_config()` (389) → `load_rerank_config()` (390). The token config's divisibility check needs `rerank_dimension_current`, which `load_rerank_config()` sets — so **move `load_rerank_config()` before `load_multivector_pq_config()`** (swap 389/390). `load_pq_config()` already has `vector_dimension_current` (set by `load_vector_config()` at line 382; a persisted `pq.config` implies a persisted `vector.config`). `load_rerank_config()` does not depend on the mvpq config, so the swap is safe.

**`load_pq_config()` (`atire_segment_index_vector.cpp`):** already validates magic/version, `m ∈ [1,65536]`, posture ∈ {0,1}, rerank_quant ∈ {0,1}, tier ∈ [0,2]. **Add** a divisibility guard: if `vector_dimension_current != 0 && vector_dimension_current % m != 0`, `fclose` and `return 0` **without** assigning `pq_m_current`/`pq_posture_current`/etc. — leaving PQ unconfigured, identical to the existing parse-failure path. (The `vector_dimension_current != 0` guard keeps the check inert in the impossible-but-defensive case where the dimension is unset.)

**`load_multivector_pq_config()` (`atire_segment_index_vector.cpp`):** currently reads m/posture/rerank_quant (+ tier for v2) with magic/version checks but no range or divisibility validation. **Add**, after the reads and before assigning the `mvpq_*_current` members: reject (fclose + return 1, the function's unconfigured path — leaves `mvpq_m_current` unset) when any of: `m < 1`; posture ∉ {PQ_POSTURE_REPLACE, PQ_POSTURE_RERANK}; rerank_quant ∉ {RERANK_QUANT_FLOAT, RERANK_QUANT_INT8}; tier ∉ {MV_TIER_FLOAT, MV_TIER_NONE}; or `rerank_dimension_current != 0 && rerank_dimension_current % m != 0`. This makes the "Defensive parse" comment accurate. Note: `load_multivector_pq_config` returns 1 on failure (its existing convention) whereas `load_pq_config` returns 0; each keeps its own convention — the meaningful signal in both is that the `*_m_current` field is left 0 (unconfigured).

## 2. Crafted-header size-overflow bound (issue #25.2)

**`ANT_multivector_pq_store::load` (`source/multivector_pq_store.cpp`, ~line 110-117):** `toks` (`read_i64(hdr+28)`) is only lower-bounded (`toks < 0` rejected). `expected_size = 52 + docs*4 + toks*mm + 256*dim*4` then multiplies `toks*mm`; an adversarial `toks` near `LLONG_MAX/mm` is signed-overflow UB that could wrap to match the real file size and slip past the exact-size gate. **Fix:** obtain the actual file size first (fseek END / ftell), then — before computing `expected_size` — reject when `mm > 0 && toks > (actual - 52) / mm` (a valid `.mvpq` always satisfies `toks*mm ≤ actual - 52`). `docs` and `dim` are already pinned to the real `expected_documents`/`expected_dimension` (line 111), so `docs*4` and `256*dim*4` cannot overflow for a real config; `toks*mm` is the only unbounded product. After the bound, the exact-size comparison proceeds as today.

**`ANT_pq_store::load` (`source/pq_store.cpp`, ~line 110-117):** already overflow-safe — `stored_documents ≤ 2^40` (line 94) and `stored_dimension ≤ 65536` (line 95) with `stored_m ≤ stored_dimension`, so `stored_documents * stored_m ≤ 2^56` and `codebook_floats*4 ≤ 2^26`, well below `LLONG_MAX`. **Add** a short comment documenting why no additional bound is needed (the existing caps), for parity with the token path — no logic change. (If a reviewer prefers an explicit belt-and-suspenders guard mirroring the token one, that is acceptable but not required; the caps already close the hole.)

## 3. Error handling

Every failure path degrades exactly as the surrounding code already does: a rejected config leaves PQ/token-PQ unconfigured (float path), and a rejected header leaves the store empty (`document_count()==0` → the segment falls back to the float `.vec`/`.mvec`). No new error surface, no exceptions, no aborts.

## 4. Testing

New `tests/test_pq_load_hardening.cpp`:
- **Config divisibility (dense + token):** write a valid index, then hand-rewrite `pq.config` / `multivector_pq.config` with an `m` that does NOT divide the dimension (and, separately, an out-of-range posture); reopen and assert PQ / token-PQ is **unconfigured** (`disk_segment_has_pq(0)==0` / `disk_segment_has_multivector_pq(0)==0` and/or `multivector_pq_configured()==0`), search still works via float fallback.
- **Valid config round-trip (regression):** a correctly written config still loads and reports configured — the guard does not reject good files.
- **Crafted header overflow (dense + token):** hand-write a `.pq` / `.mvpq` with a header count (`toks` / documents) set to a huge value near `LLONG_MAX/m`; `ANT_pq_store::load` / `ANT_multivector_pq_store::load` returns a degraded empty store (`document_count()==0` / `token_count()==0`), no crash, no oversized allocation.
- **ASan/UBSan** on the crafted-header tests (UBSan `signed-integer-overflow` must NOT fire in the load paths); the known out-of-scope `ANT_file::setvbuff` leak excluded.

## 5. Sequencing (TDD tasks)

1. **Config revalidation:** the `open()` reorder + `load_pq_config` divisibility guard + `load_multivector_pq_config` range+divisibility guards + the config tests (divisibility reject both paths, valid round-trip).
2. **Overflow bound:** the `ANT_multivector_pq_store::load` `toks` bound + the dense parity comment + the crafted-header tests + ASan/UBSan sweep.

## 6. Repo constraints

Whole repo `-fPIC`; **`rm -f obj/*.o lib/libantelope_engine.a` after header changes** (none expected here — all `.cpp` — but rebuild clean if a header is touched); after ASan a full clean rebuild before a normal link; `source/*.cpp`+`tests/*.cpp` auto-discovered; tests build to `bin/<name>` via `make <name>` (`CHECK()`); config setters POST-open; `pq.config` magic `ANTPQCF1` (v1/v2), `multivector_pq.config` magic `ANTMVPQC` (v1/v2, tier added in #24); `.pq` header `ANT_PQ_STORE_HEADER_SIZE`, `.mvpq` header 52 bytes (magic `ANTMVPQ1`).

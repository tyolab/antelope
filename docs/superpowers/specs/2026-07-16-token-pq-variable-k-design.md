# Token `.mvpq` Variable Code-Width (k≠256) Design (token epic, sub-project 3 of 4)

**Status:** approved 2026-07-16. Third of the **token `.mvpq` advanced-codec** epic, after the shipped **T1 token-OPQ** (e975fe6) and **T3 token-global-codebook** (d21a964). Mirrors the shipped dense **variable code-width #22.3** (b780871) for the ragged token pool. Last remaining: T4 token-single-resident (mirror Approach A, depends on T3).

**Goal:** let the token `.mvpq` store use a codebook of `k` = any power of two in `[2,256]` (default 256), bit-packing each token's `m` codes to `row_bytes = (m·bits + 7)/8` bytes (`bits = log2(k)`) instead of one byte per subvector, so `.mvpq` sidecars shrink at lower `k` in exchange for graceful recall loss. Opt-in, persisted, immutable-once. Composes with T1 token-OPQ (rotation R is `D²`, k-independent), T3 token-global-codebook (the shared codebook is trained/embedded at the chosen k), and the #24 resident tiers (orthogonal). Default off (k=256) ⇒ `.mvpq` v1/v2 and every path byte-identical to today.

**Architecture decision — mirror dense #22.3: add `.mvpq` v3, keep v1/v2 byte-identical at k=256.** The shared `ANT_pq_codec` already carries the full variable-k machinery from #22.3 — `bits_for_k`, `pack_codes`/`unpack_codes`, and a runtime `k` on `train`/`encode`/`adc_table`/`adc_score`/`reconstruct` — so **NO codec change is needed**. Raggedness is purely a store-layout concern; all T2 work is `.mvpq` store row-layout + engine `set_multivector_pq_k` config + writer/compaction wiring. The token `.mvpq` header already carries `k` as its 5th i64 (always 256 today), exactly as dense `.pq` did — so the version bump only changes the *codes* block (bit-packed) and the *codebook* block (sized by k).

**Rejected alternatives:** (B) always bit-pack / mutate the existing v2 in place — breaks back-compat and k=256 byte-identity; (C) shrink only the codebook and keep 1 unpacked byte per code — saves almost nothing because the per-token codes block dominates at token scale, defeating the goal.

**Tech stack:** C++ engine — `source/multivector_pq_store.{h,cpp}` (store v3 row layout + writer k param), `atire/atire_segment_index*` (config, `set_multivector_pq_k`, global-codebook-at-k, writer/compaction wiring), tests `tests/*.cpp`.

**Scope:** engine-only, token `.mvpq` only. No codec change. No binding change. Default off (k=256) ⇒ byte-identical to today.

---

## 1. Store — `.mvpq` v3 variable-k row layout (`source/multivector_pq_store.{h,cpp}`)

- **New token `.mvpq` v3** (variable-k, opq-capable): same 60-byte header as v2 (`magic ANTMVPQ1(8) + version u32 + dimension/documents/total_tokens/m/k (5×i64) + opq i64 at offset 52`). `k` already lives in the header. **Version selection on write:** `version = (k == 256) ? (opq ? V2 : V1) : V3`. When `k != 256`, always V3 (v3 always carries the opq i64 slot; the trailing `R` block is present iff opq=1, exactly like v2).
- **Blocks** (disk order unchanged: counts → codes → codebook → rotation):
  - `counts[documents]` int32 (unchanged).
  - `codes`: `total_tokens · row_bytes` bytes, `row_bytes = (m·bits + 7)/8`, each token-row bit-packed LSB-first via `ANT_pq_codec::pack_codes` (was `total_tokens · m` unpacked bytes; when k==256, `bits=8` ⇒ `row_bytes==m` ⇒ byte-identical).
  - `codebook`: `k · dimension` floats = `m · k · sub` (was `256 · dimension`).
  - `rotation`: `D²` floats, present iff opq (unchanged from T1).
- **Store members:** add `row_bytes` and `bits` (mirror dense `ANT_pq_store`). `expected_size = 60 + documents·4 + total_tokens·row_bytes + k·dimension·4 + (opq ? D²·4 : 0)`.
- **Load:** read `k` from the header; `bits = ANT_pq_codec::bits_for_k(k)` for v3 (v1/v2 require `k == 256` ⇒ `bits = 8`, else reject); compute `row_bytes`; keep the existing forgiving contract — validate `bits >= 0`, `m` divides `dimension`, `dimension ∈ (0,65536]`, `sum(counts) == total_tokens`, and an **exact** `expected_size` check **before any allocation**; bound `total_tokens` before the size multiply (existing #25 hardening); any mismatch ⇒ degraded-empty (`.mvec` fallback).
- **Scoring / reconstruct:** ADC-MaxSim `maxsim` builds the `num` ADC tables at `m·k` (was `m·256`); per doc-token it **unpacks** that token's `row_bytes` into an `m`-byte scratch (`ANT_pq_codec::unpack_codes`) then `adc_score` at k. `token_reconstruct` unpacks the row then `reconstruct` at k (this is the path #24 NONE-tier reuse depends on). `token_codes(t)` returns the packed row pointer (callers that compare codes across segments compare packed rows — valid because same k ⇒ same `row_bytes`).
- **Writer:** `create(path, dim, m, k, metric, opq)` gains the `k` param (mirror dense writer). `finish` (owned or external codebook) encodes each token to `m` byte-codes then `pack_codes` at `bits` into the row. `set_external_codebook` unchanged (the supplied codebook is already `k`-sized). Add `get_k()` accessor.

## 2. Engine config (`atire/atire_segment_index_vector.cpp` + `.h`)

- **Member** `long long mvpq_k_current` (default **256**; ctor init) + getter `long long multivector_pq_k(void) { return mvpq_k_current; }`.
- **`set_multivector_pq_k(long long k)`** — mirror `set_pq_k`: require open + `multivector_pq_configured()`; validate `ANT_pq_codec::bits_for_k(k) >= 0` (power of two in `[2,256]`); **immutable-once** (once set to a non-256 value it cannot change; setting the same value is idempotent); persisted; revert on save failure. Composes with `mvpq_m`/`posture`/`opq`/`global`/tier (orthogonal).
- **`multivector_pq.config` v4→v5:** append `k` i64 as the 7th value. Back-compat: v1(3 vals)/v2(4)/v3(5)/v4(6) load with `k = 256`; v5 reads + validates `bits_for_k(k) >= 0` (else leave token-PQ unconfigured, per #25 revalidation). Mirror the T3 `global` (v4) addition exactly — extend `vals[]` to 7, add a `version==5` read branch, assign `mvpq_k_current`; save writes `version=5` + 7 vals. No field-offset drift.

## 3. Composition wiring (`atire/atire_segment_index*`)

- **T3 global codebook:** `ensure_global_mvpq_codebook` and `rebuild_mvpq_global_codebook` train the shared codebook at `mvpq_k_current` (`ANT_pq_codec::train(rows, dim, m, k, n, codebook)` already takes k); `global_mvpq_codebook` is sized `k · dim` (= `m · k · sub`). The **`multivector_pq.codebook` sidecar** (magic `ANTMVGCB`) — today hardcodes `k = ANT_pq_codec::K` — is generalized to write the actual `k` in its header and size its codebook block `k · dim`; load validates `k == mvpq_k_current` and `bits_for_k(k) >= 0`, else forgiving-degrade to untrained (retrain next build). (R block unchanged — D², opq-keyed.)
- **T1 OPQ:** rotation R is `D²`, independent of k; `train_rotation` is k-agnostic. Composes with no change beyond the store already handling the R block per §1.
- **#24 resident tiers:** unchanged — the NONE-tier `token_reconstruct`-from-PQ routes through the store's unpack-at-k path (§1); FLOAT/NONE compose with any k.
- **Writer create sites pass `mvpq_k_current`:** `build_multivector_pq` backfill, the compaction `.mvpq` writer (`atire_segment_index_compaction.cpp`), and `rebuild_mvpq_global_codebook`'s Pass-2 re-encode writer. Fail-soft paths unchanged.

## 4. Testing (mirror dense #22.3)

- **k=256 byte-identity:** default (no `set_multivector_pq_k`) writes v1/v2 exactly as today; existing token suites (`test_mvpq_*`, `test_v6_*`, `test_pq_token_resident_tier`) stay green.
- **Small-k round-trip:** build under a small k (e.g. 16) — assert the `.mvpq` is v3, `row_bytes == (m·4+7)/8`, on-disk size matches `expected_size`; reopen; MaxSim search sane; `token_reconstruct` error bounded.
- **k + global (T3):** two segments under global mode at small k — the global codebook is trained/embedded at k; both `.mvpq` embed byte-equal `k·dim` codebooks; a shared probe token encodes to identical packed rows across segments.
- **k + OPQ (T1):** small k + `set_multivector_pq_opq(1)` — R block + k-packed codes coexist; R round-trips on reopen; cross-segment codes comparable (same R and k-codebook); MaxSim sane.
- **k + compaction / rebuild:** no-retrain compaction reuses the k-codebook (merged `.mvpq` is v3 at k); `rebuild_mvpq_global_codebook` retrains + re-encodes every segment at k; NONE-tier rebuild remains UAF-safe (T3's token_source/token_index refresh runs regardless of k).
- **Immutability + config v5 back-compat:** `set_multivector_pq_k` immutable-once; a `multivector_pq.config` without the k field loads as k=256; a persisted invalid k leaves token-PQ unconfigured (#25).
- **Forgiving load:** a truncated/mismatched v3 `.mvpq` or `multivector_pq.codebook` degrades cleanly (no over-read/over-alloc/crash). ASan/UBSan sweep (environment-blocked — report).

## 5. Sequencing (TDD tasks)

1. **Store v3 row layout** (`multivector_pq_store.{h,cpp}`): `row_bytes`/`bits` members, v3 header/version selection, `create(...,k,...)` + `finish` pack, load unpack + validate-before-alloc + exact-size, `maxsim`/`token_reconstruct` unpack-at-k, `get_k`; unit test (variable-k writer round-trips packed rows; k==256 byte-identical to v1/v2).
2. **Engine `set_multivector_pq_k` + config v5** (`atire/*`): member + getter + setter (immutable-once, validated, persisted) + `multivector_pq.config` v4→v5 back-compat + wire `build_multivector_pq` writer to `mvpq_k_current`. Tests: small-k build/reopen, immutability, config back-compat, default-off byte-identity.
3. **Composition** (`atire/*`): global-codebook-at-k (sidecar generalized) + OPQ-at-k + compaction/rebuild-at-k + tier compose. Tests: k+global cross-segment, k+OPQ, k+compaction no-retrain, k+rebuild (incl. NONE-tier), forgiving-load.

## 6. Repo constraints

Preserve the `.mvpq` deterministic-rebuild + forgiving-load contracts (variable-k extends them — same k+codebook ⇒ same packed codes). Header changes → `rm -f obj/*.o lib/libantelope_engine.a`; fresh worktree `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered → `bin/<name>` via `make <name>`; config setters POST-open; `multivector_pq.config` add-a-field back-compat mirrors the T3 `global` v4 change; `set_multivector_pq_k` mirrors `set_pq_k`; token variable-k is store+config+wiring only (codec untouched). Confirm signatures/line numbers by grep before editing.

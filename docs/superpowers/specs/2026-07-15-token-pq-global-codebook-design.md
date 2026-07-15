# Token `.mvpq` Global Codebook Design (token epic, sub-project 2 of 4)

**Status:** approved 2026-07-15. Second of the **token `.mvpq` advanced-codec** epic, after the shipped **T1 token-OPQ** (e975fe6). Mirrors the shipped dense **global codebook (#22.2, 5bf7025, Approach B)** for the ragged token pool. Later: T2 token-variable-k (mirror #22.3), T4 token-single-resident (mirror Approach A, depends on this).

**Goal:** train ONE frozen collection-wide token codebook for the whole `.mvpq` collection instead of a fresh per-segment codebook, so (a) token codes are directly comparable across segments and (b) **compaction needn't retrain**. Opt-in, persisted, deterministic. Composes with T1 token-OPQ (the rotation `R` also becomes global — one shared R) and the #24 resident tiers (orthogonal). Default off ⇒ per-segment path byte-identical.

**Architecture decision — Approach B (mirror dense #22.2):** each `.mvpq` embeds a *copy* of the one shared frozen codebook (+R). The `.mvpq` format, `load`, store, codec, and all query paths are UNCHANGED — only *who trains* moves from the writer to the engine. B delivers the two goals (comparable codes + no-retrain compaction) with **zero new lifetime hazard** — it does NOT introduce a cross-segment codebook borrow (that is **T4 token-single-resident**, the RAM-win follow-up, deferred exactly as dense deferred Approach A).

**Architecture (one sentence):** the engine owns a `global_mvpq_codebook` (+`global_mvpq_rotation` when OPQ) trained ONCE over the first build's flattened token pool, persisted to a shared `<dir>/multivector_pq.codebook` sidecar and loaded on open; every `.mvpq` writer (flush / `build_multivector_pq` / compaction) is handed that frozen codebook+R and **skips its own training** (both `train` and, under T1 OPQ, `train_rotation`), embedding the shared copy — so `.mvpq` format/load/store/codec/query are all UNCHANGED.

**Tech stack:** C++ engine — `atire/atire_segment_index*` (config, global-codebook train/persist/load, writer wiring, compaction, rebuild), `source/multivector_pq_store.{h,cpp}` (writer: accept an external codebook+R instead of training), tests `tests/*.cpp`.

**Scope:** engine-only, token `.mvpq` only. No binding changes. Default off ⇒ per-segment path byte-identical to today.

---

## 1. Shared codebook lifecycle (engine)

- **Members** (`ATIRE_segment_index`): `long mvpq_global_current` (0 off / 1 on; immutable once on), `float *global_mvpq_codebook` (m·256·(D/m), NULL until trained), `float *global_mvpq_rotation` (D·D when OPQ on, else NULL). Freed in the engine dtor. (`D = rerank_dimension_current`.)
- **`multivector_pq.codebook` sidecar** (`<dir>/multivector_pq.codebook`): magic `ANTMVGCB`, u32 version 1, i64 dimension/m/k, i64 opq flag, then (opq ? D·D float `R`) + m·256·(D/m) float codebook. Atomic temp+rename. Forgiving load: any mismatch (dim/m/k vs the index, size) ⇒ treat as untrained (retrain on next build). Validate-before-allocate (D≤65536 bounds D²). Mirrors dense `pq.codebook` (`ANTPQGCB`).
- **Train-once-frozen** (`ensure_global_mvpq_codebook(long which)`): the FIRST `.mvpq`-producing build under global mode with no existing `global_mvpq_codebook` — load segment `which`'s on-disk `.mvec` float pool (`ANT_multivector_store::load(mvec_name, rerank_dimension_current, docs)`); if degraded/empty (`token_count()==0`) fail-soft return nonzero; flatten the `total_tokens×D` pool into a `rows` buffer via `token_reconstruct(t, rows + t·D)` for `t ∈ [0, token_count())`; if `mvpq_opq_current`, `train_rotation` → `global_mvpq_rotation`, rotate `rows` in place; `train` the codebook over `rows` (k=256) → `global_mvpq_codebook`; persist `multivector_pq.codebook`; keep resident. Frozen thereafter — every later segment/compaction reuses it. On `open()`, `load_mvpq_codebook()` if present (global mode configured). Fail-soft on ANY failure ⇒ leave both buffers NULL, return nonzero (caller falls back to per-segment training — no hard build failure, no half-trained state).
- **Explicit `rebuild_mvpq_global_codebook()`:** retrain (+R) over ALL segments' tokens (gather first, before freeing the old codebook — fail-soft ordering), persist the new sidecar, then re-encode every segment's `.mvpq` against it. Opt-in, expensive, never automatic. Returns nonzero if global mode/token-PQ unconfigured or no tokens. Mirrors dense `rebuild_pq_global_codebook`.

## 2. Writer: use an external codebook (`source/multivector_pq_store.{h,cpp}`)

`ANT_multivector_pq_store_writer` gains `set_external_codebook(const float *codebook, const float *rotation)` (both borrowed for the duration of `finish`; `rotation` NULL ⇒ non-OPQ). When set, `finish()`:
- skips `train` AND (under T1 OPQ) `train_rotation` — uses the supplied `R`,
- rotates (if `R`) + encodes every token against the supplied codebook,
- **embeds a copy** of the supplied codebook (+R) into the `.mvpq` exactly as today (so `.mvpq` v1/v2 format and `ANT_multivector_pq_store::load` are UNCHANGED — the store still owns its embedded copy).

`finish()` gets the owned-vs-borrowed refactor (mirror dense #22.2): `owned_codebook`/`owned_rotation` (freed) vs `const float *codebook`/`rotation` (used — external or owned); when `ext_codebook != NULL` skip both trains, use externals, free only owned; the written `opq_flag`/version derive from the actual `rotation` pointer (external or owned). `create(...)` resets `ext_*` to NULL (a reused writer carries no stale externals). The writer NEVER frees the borrowed externals. Non-external mode (default) byte-identical to today — the ONLY change to the store is this seam; the codec is untouched.

## 3. Config (`atire/atire_segment_index*`)

- `long set_multivector_pq_global_codebook(long enable)` — opt-in, requires open + `multivector_pq_configured()`; idempotent same-value; immutable once enabled; persisted. Composes with `mvpq_posture`/`mvpq_opq`/tier (orthogonal). Persisted in `multivector_pq.config` (bump v3→**v4**, add a `global` i64; back-compat: v1/v2/v3 absent ⇒ 0 — mirror the T1 opq v3 addition).
- Getter `long multivector_pq_global_codebook(void) { return mvpq_global_current; }`.

## 4. Wiring the writers + compaction

- Every token-`.mvpq` writer `create`/`finish` site (`build_multivector_pq` backfill, compaction) checks `mvpq_global_current`: if on, `ensure_global_mvpq_codebook(which)` (train-once on the first call, else the resident one) and `set_external_codebook(global_mvpq_codebook, global_mvpq_rotation)` before `finish()` — fail-soft to per-segment training when the codebook is NULL.
- **Compaction** (`atire_segment_index_compaction.cpp`): currently retrains a fresh per-segment codebook over the merged token pool. Under global mode it reuses the resident `global_mvpq_codebook`+R (no retrain) — the merged output segment is the newly appended one; `ensure_global_mvpq_codebook((long)(segment_count-1))` (guarded fail-soft) + `set_external_codebook`.
- **Tiers (#24):** unchanged — tiers govern the resident float/NONE token source; codes reference the (now shared) codebook the same way. Global mode + FLOAT/NONE compose. `token_reconstruct`-from-PQ (NONE tier) reads the embedded codebook exactly as today.

## 5. Testing (mirror dense #22.2's `test_pq_global`)

- **No-retrain compaction:** build ≥2 segments under global mode, capture the `multivector_pq.codebook` bytes, compact, assert the sidecar bytes are byte-identical (no retrain) and the merged `.mvpq` embeds the same codebook; token search sane.
- **Cross-segment code comparability:** the same token added to two different segments (both under the frozen global codebook) encodes to the SAME m code bytes — assert via `token_codes` + a reconstruct match; contrast with per-segment mode where they may differ.
- **Train-once-frozen + persistence:** first build trains + writes `multivector_pq.codebook`; a second build reuses it (bytes unchanged); close + reopen loads it and searches correctly; `multivector_pq_global_codebook()` restored from `multivector_pq.config` v4.
- **`rebuild_mvpq_global_codebook`:** after adding more (differently-distributed) tokens, rebuild retrains + re-encodes every segment; the new sidecar differs from the first; all segments share the NEW codebook; recall stays sane.
- **OPQ (T1) composition:** global mode + `set_multivector_pq_opq(1)` — one global `R`+codebook trained once; cross-segment codes comparable (same R AND codebook); MaxSim recall preserved.
- **Back-compat / default off:** no `set_multivector_pq_global_codebook` ⇒ per-segment training byte-identical (existing token suites unchanged); a `multivector_pq.config` without the `global` field loads as off.
- **Forgiving load:** a corrupt/mismatched `multivector_pq.codebook` ⇒ treated as untrained (retrains on next build), no crash/over-read. ASan/UBSan sweep (environment-blocked — report).

## 6. Sequencing (TDD tasks)

1. **Writer external-codebook seam** (`multivector_pq_store.{h,cpp}`): `set_external_codebook` + `finish` skip-train(+train_rotation) path with the owned-vs-borrowed refactor; unit test (writer with a hand-supplied codebook+R produces codes against it, embeds it, loads back identically; non-external path byte-identical).
2. **Engine global codebook** (`atire/*`): members + `multivector_pq.codebook` sidecar (train-once/persist/load/forgiving) + `set_multivector_pq_global_codebook` config + `multivector_pq.config` v4 bump + wire `build_multivector_pq` writer to the shared codebook. Tests: train-once, persistence/reopen, cross-segment comparability, default-off byte-identity.
3. **Compaction + rebuild** (`atire/*`): compaction reuses the shared codebook (no retrain); `rebuild_mvpq_global_codebook()` retrain+re-encode-all. Tests: no-retrain compaction (byte-identical across compact), rebuild changes the codebook everywhere, OPQ-global composition, forgiving-load.

## 7. Repo constraints

Preserve the `.mvpq` deterministic-rebuild + forgiving-load contracts (global mode extends them — same frozen codebook ⇒ same codes). Header changes → `rm -f obj/*.o lib/libantelope_engine.a`; fresh worktree `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered → `bin/<name>` via `make <name>`; config setters POST-open; `multivector_pq.config` add-a-field back-compat mirrors the T1 opq v3 change; `set_multivector_pq_global_codebook` mirrors `set_pq_global_codebook`. `ANT_multivector_pq_store::load` COPIES the embedded codebook into its own buffer → no `.mvpq` aliases the engine's `global_mvpq_codebook` → no cross-segment borrow/UAF (that is why B is chosen over the T4 single-resident borrow). Confirm signatures/line numbers by grep before editing.

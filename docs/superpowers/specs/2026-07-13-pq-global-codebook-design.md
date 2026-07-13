# Dense PQ Global Codebook Design (#22, sub-project 2 of 3)

**Status:** proposed 2026-07-13, pending user review. Issue **#22** — advanced PQ codec options. This spec covers **sub-project 2: a collection-wide (global) codebook for the dense `.pq` store**, replacing the current per-segment codebook training. Sub-project 1 (OPQ, #22.1) shipped (647aa7c); sub-project 3 (k≠256) is separate.

**Goal:** Train ONE frozen codebook for the whole collection instead of one per segment, so (a) codes are directly comparable across segments and (b) **compaction needn't retrain** the codebook. Opt-in, persisted, deterministic. Composes with OPQ (#22.1 — the rotation `R` also becomes global) and the resident tiers (#19 — orthogonal).

**Architecture decision (chosen while user away — confirm at gate):** two shapes were considered — **A** a single shared codebook borrowed by every segment store (codes-only `.pq`, one resident copy, the RAM win) vs **B** every `.pq` embeds a *copy* of the shared frozen codebook (format/load/store unchanged, no borrow). **This spec implements B.** Rationale: B delivers the issue's two stated goals (comparable codes + no-retrain compaction) with **zero new lifetime hazard** — it does NOT introduce the codebase's first cross-segment codebook borrow (a UAF class it was bitten by via V6 `.tann`), which is the prudent call for an unattended decision. **A (dedupe to one resident codebook — a pure RAM optimization) is a clean follow-up** layered on B; opt into it at the review gate if you want the RAM win now.

**Architecture (one sentence):** the engine owns a `global_pq_codebook` (+ `global_pq_rotation` when OPQ) trained ONCE over the first build's present rows, persisted to a shared `<dir>/pq.codebook` sidecar and loaded on open; every `.pq` writer (flush / `build_pq` / compaction) is handed that frozen codebook+R and **skips its own training**, embedding the shared copy — so `.pq` format, `load`, store, codec, and query paths are all UNCHANGED.

**Tech stack:** C++ engine — `atire/atire_segment_index*` (config, global-codebook train/persist/load, writer wiring, compaction, rebuild), `source/pq_store.{h,cpp}` (writer: accept an external codebook+R instead of training), tests `tests/*.cpp`.

**Scope:** engine-only, dense `.pq` only (token `.mvpq` global codebook is a follow-up). No binding changes. Default off ⇒ per-segment path byte-identical to today.

---

## 1. Shared codebook lifecycle (engine)

- **Members** (`ATIRE_segment_index`): `long pq_global_current` (0 off / 1 on; immutable once on), `float *global_pq_codebook` (m·K·(D/m), NULL until trained), `float *global_pq_rotation` (D·D when OPQ on, else NULL). Freed in the engine dtor.
- **`pq.codebook` sidecar** (`<dir>/pq.codebook`): magic `ANTPQGCB`, u32 version 1, i64 dimension/m/k, i64 opq flag, then (opq ? D·D float `R`) + m·K·(D/m) float codebook. Atomic temp+rename. Forgiving load: any mismatch (dim/m/k vs the index, size) ⇒ treat as untrained (retrain on next build). Validate-before-allocate (D≤65536 already bounds D²).
- **Train-once-frozen:** the FIRST `.pq`-producing build under global mode (first flush / `build_pq`) with no existing `global_pq_codebook`: train over that build's present rows (with OPQ: `train_rotation` → `R`, then `train` the codebook in rotated space), persist `pq.codebook`, keep resident. Frozen thereafter — every later segment/compaction reuses it. On `open()`, load `pq.codebook` if present (global mode configured) into the resident buffers.
- **Explicit `rebuild_pq_global_codebook()`:** retrain the global codebook (+R) over ALL resident dense vectors across segments, persist the new `pq.codebook`, then re-encode every segment's `.pq` against it (rewrites each `.pq`). Opt-in, expensive, never automatic — the escape hatch for drift after the collection grows beyond the first batch. Returns nonzero if global mode/PQ unconfigured or no vectors.

## 2. Writer: use an external codebook (`source/pq_store.{h,cpp}`)

`ANT_pq_store_writer` gains a way to run `finish()` against a caller-supplied codebook+R instead of training its own — e.g. `set_external_codebook(const float *codebook, const float *rotation)` (both borrowed for the duration of `finish`; `rotation` NULL ⇒ non-OPQ). When set, `finish()`:
- skips `train` (and, under OPQ, skips `train_rotation` — uses the supplied `R`),
- rotates (if `R`) + encodes every row against the supplied codebook,
- **embeds a copy** of the supplied codebook (+R) into the `.pq` exactly as today (so `.pq` format/version and `ANT_pq_store::load` are UNCHANGED — the store still owns its embedded copy).

Non-external mode (the default) is byte-identical to today. This is the ONLY change to `pq_store.*`; the codec is untouched.

## 3. Config (`atire/atire_segment_index*`)

- `long set_pq_global_codebook(long enable)` — opt-in, requires open + `pq_configured()`; idempotent same-value; immutable once enabled; persisted. Composes with any `pq_posture`/`pq_opq`/tier (orthogonal). Persisted in `pq.config` (bump version, add a `global` i64; back-compat: absent ⇒ 0 — the loader already tolerates version growth, mirror the OPQ v3 addition).
- Getter `long pq_global_codebook(void) { return pq_global_current; }`.

## 4. Wiring the writers + compaction

- Every dense-`.pq` writer `create`/`finish` site (flush-time eager build, `build_pq` backfill, compaction retrain) checks `pq_global_current`: if on, ensure the global codebook exists (train-once on the first such call, else use the resident one) and `set_external_codebook(global_pq_codebook, global_pq_rotation)` before `finish()`.
- **Compaction:** currently retrains a fresh per-segment codebook over the merged dense vectors. Under global mode it instead reuses the resident `global_pq_codebook`+`R` (no retrain) and re-encodes the merged docs against it — the "needn't retrain" win. (Re-encoding from the merged float `.vec` is deterministic against the shared codebook; equivalent to copying codes, simpler to implement.)
- **Tiers (#19):** unchanged — tiers govern the resident float/int8 vectors; codes reference the (now shared) codebook the same way. Global mode + FLOAT/INT8/NONE all compose. INT8 `.pqr` and `reconstruct`-from-PQ read the embedded codebook exactly as today.

## 5. Testing

- **No-retrain compaction:** build ≥2 segments under global mode, capture the global codebook bytes, `maintain()`/compact, assert the merged segment's embedded codebook equals the pre-compaction global codebook (byte-identical — proves no retrain) and search results are sane.
- **Cross-segment code comparability:** the same input vector added to two different segments (both under the frozen global codebook) encodes to the SAME m code bytes — assert via `codes_for`/a reconstruct match; contrast with per-segment mode where they may differ.
- **Train-once-frozen + persistence:** first build trains + writes `pq.codebook`; a second flush reuses it (codebook bytes unchanged); close + reopen loads `pq.codebook` and continues to search correctly; `pq_global_codebook()` restored from `pq.config`.
- **`rebuild_pq_global_codebook`:** after adding more (differently-distributed) docs, rebuild retrains + re-encodes every segment; the new `pq.codebook` differs from the first, all segments now share the NEW codebook, and recall vs exact-float stays sane.
- **OPQ composition:** global mode + `set_pq_opq(1)` — one global `R`+codebook trained once; recall gain preserved; cross-segment codes comparable (same R AND codebook).
- **Back-compat / default off:** no `set_pq_global_codebook` ⇒ per-segment training path byte-identical to today (existing PQ suites unchanged); a `pq.config` without the `global` field loads as off.
- **Forgiving load:** a corrupt/mismatched `pq.codebook` ⇒ treated as untrained (retrains on next build), no crash/over-read. ASan/UBSan sweep (environment-blocked per prior sub-projects — report).

## 6. Sequencing (TDD tasks)

1. **Writer external-codebook seam** (`pq_store.*`): `set_external_codebook` + `finish` skip-train path; unit test (writer with a hand-supplied codebook produces codes against it, embeds it, loads back identically; non-external path byte-identical).
2. **Engine global codebook** (`atire/*`): members + `pq.codebook` sidecar (train-once/persist/load/forgiving) + `set_pq_global_codebook` config + `pq.config` bump + wire flush/`build_pq` writers to the shared codebook. Tests: train-once, persistence/reopen, cross-segment comparability, default-off byte-identity.
3. **Compaction + rebuild**: compaction reuses the shared codebook (no retrain); `rebuild_pq_global_codebook()` retrain+re-encode-all. Tests: no-retrain compaction (codebook byte-identical across compact), rebuild changes the codebook everywhere, OPQ-global composition, forgiving-load.

## 7. Repo constraints

Preserve the `.pq` deterministic-rebuild + forgiving-load contracts (global mode extends them — same frozen codebook ⇒ same codes). Header changes → `rm -f obj/*.o lib/libantelope_engine.a`; fresh worktree `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered → `bin/<name>` via `make <name>`; config setters POST-open; `pq.config` add-a-field back-compat mirrors the #22.1 OPQ v3 change. `pq_global_current` initialized in the ctor. Confirm signatures/line numbers by grep before editing.

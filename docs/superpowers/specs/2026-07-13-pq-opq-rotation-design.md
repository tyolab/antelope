# Dense PQ OPQ Rotation Design (#22, sub-project 1 of 3)

**Status:** proposed 2026-07-13, pending user review. Issue **#22** — advanced PQ codec options. This spec covers ONLY the first of three independent sub-projects: **OPQ rotation for the dense `.pq` store**. The other two knobs — global/collection-wide codebooks, and `k != 256` variable code-width — get their own specs later; neither is designed here.

**Decomposition note (chosen while user away, confirm at gate):** #22's three knobs touch different seams (OPQ = a rotation around the codec; global codebooks = codebook lifecycle/compaction; k≠256 = code-width packing). They are built as three separate sub-projects. OPQ is first: best recall-per-effort, cleanest boundary. This sub-project scopes OPQ to a **deterministic PCA/second-moment rotation** (no alternating-Procrustes/general-SVD loop) applied to the **dense `.pq`** store only (token `.mvpq` OPQ is a later follow-up).

**Goal:** Optionally learn an orthogonal rotation `R` (D×D) at PQ train time that realigns the vector space so the m subspace split incurs less quantization error → better recall at the same m/k. Opt-in, persisted, byte-deterministic (same platform), and metric-exact (rotation is orthogonal, so it preserves dot/L2/cosine — see below).

**Architecture (one sentence):** `ANT_pq_codec` gains pure rotation helpers (`train_rotation` via a deterministic symmetric Jacobi eigensolver + eigenvalue-balanced subspace allocation, `apply_rotation`, `apply_rotation_transpose`); `ANT_pq_store` owns the trained `R`, rotates the query once before each `adc_table` and un-rotates `reconstruct` output via `Rᵀ`, and persists `R` in a version-2 `.pq` sidecar; a new `set_pq_opq(enable)` config gates it.

**Tech stack:** C++ engine — `source/pq_codec.{h,cpp}` (pure), `source/pq_store.{h,cpp}` (+ writer), `atire/atire_segment_index*` (config + compaction retrain), tests `tests/*.cpp`.

**Scope:** engine-only, dense `.pq` only. No binding changes (a later follow-up can extend the #23 `pq` bag with an `opq` flag). Non-OPQ path stays byte-identical (opt-in, default off).

---

## 1. Why rotation is metric-exact here (no un-rotation for scoring)

`R` is **orthogonal** (`RᵀR = I`) and there is **no centering/translation**. Therefore `dot(Rq, Rx) = qᵀRᵀRx = qᵀx` and `‖Rq − Rx‖ = ‖q − x‖` — rotating BOTH the query and the documents preserves the true dot/cosine/L2 score exactly. OPQ's benefit is purely that quantizing `Rx` (instead of `x`) with the same m/k has lower error because `R` balances variance across subspaces. Consequence: ADC scoring is done entirely in rotated space and needs NO un-rotation. Centering is deliberately NOT used (it would break dot-product preservation), so the rotation is derived from the **uncentered second-moment matrix** `M = (1/n) Σ xxᵀ`, not the covariance.

## 2. Codec rotation helpers (`source/pq_codec.{h,cpp}`)

Add three pure static methods; `train`/`encode`/`adc_table`/`reconstruct` are UNCHANGED (they operate on already-rotated vectors — the store rotates at the boundary).

- `long train_rotation(const float *vectors, long long dimension, long long m, long long n, float *R)` — builds the D×D row-major rotation:
  1. Accumulate `M = Σ xxᵀ` (D×D symmetric, double precision).
  2. **Deterministic symmetric Jacobi eigensolver** on `M` → eigenvalues `λ[D]`, eigenvectors `U` (columns). Fixed cyclic sweep order, fixed max sweeps (e.g. 100) + off-diagonal threshold; ties/order deterministic.
  3. **Sign-canonicalize** each eigenvector (force the largest-magnitude component positive) → removes the ±sign ambiguity that would otherwise make `R` non-deterministic.
  4. **Eigenvalue-balanced subspace allocation:** distribute the D eigenvectors across the m subspaces (each gets D/m) so the per-subspace product of eigenvalues is balanced — greedy: sort eigenvalues desc, assign each to the subspace with the currently-smallest running product. This is what makes the split low-error (plain variance-sorted contiguous split would pile all high-variance dims into subspace 0).
  5. `R`'s rows = the allocated eigenvectors in subspace-contiguous order (so subspace `s` reads rotated dims `[s·sub, (s+1)·sub)`). `R = P·Uᵀ`, orthogonal. Returns 1 on `m∤dimension` or `n==0` (caller then skips OPQ / leaves `R` identity).
- `void apply_rotation(const float *vec, long long dimension, const float *R, float *out)` — `out = R·vec` (D→D).
- `void apply_rotation_transpose(const float *vec, long long dimension, const float *R, float *out)` — `out = Rᵀ·vec` (for `reconstruct` un-rotation).

Determinism caveat (documented): the rotation is byte-identical on rebuilds **on the same platform/compiler**; cross-platform float rounding in Jacobi may differ (acceptable — the non-OPQ codec's cross-platform byte-identity is a convenience, not a contract; same-platform rebuild + compaction reproducibility is preserved).

## 3. Store integration (`source/pq_store.{h,cpp}`)

- **Member:** `float *rotation;` (NULL when OPQ off — the common/back-compat case). Freed in dtor.
- **Sidecar v2:** bump `ANT_PQ_STORE_VERSION` 1→2. Header gains a `i64 opq` flag (0/1) after `k`. When `opq==1`, the file appends `dimension*dimension` floats (the row-major `R`) after the codebook. `expected_size` adds `opq ? dimension*dimension*4 : 0`. **Back-compat:** a v1 file (or v2 with `opq==0`) loads as non-OPQ (`rotation=NULL`); `load` validates the `R` block size before allocating (bounded: `dimension ≤ 65536` already capped → `dimension² ≤ 2³² ` guard). A corrupt/mis-sized `R` → degrade to empty store (existing forgiving-load contract).
- **Query paths (3 `adc_table` call sites — score, `prepare_query` #26, `scan_adc`):** when `rotation != NULL`, rotate the query once into a stack/heap scratch (`apply_rotation(query, dimension, rotation, rq)`) and pass `rq` to `adc_table`. For `prepare_query` (#26), rotate once when building the prepared table (the "build ADC table once per search" property is preserved — one extra D×D matvec per search, not per node).
- **`reconstruct`:** codec reconstruct yields a rotated-space approximation; when `rotation != NULL`, apply `apply_rotation_transpose` to return an ORIGINAL-space vector (the reconstruct contract). This keeps the #19 NONE-tier "reconstruct-from-PQ" rerank path correct under OPQ.
- **Writer `finish()`:** when OPQ enabled, `train_rotation` over the collected training vectors → `R`; rotate every vector by `R` (scratch) → train codebooks + encode on rotated vectors; write `R` to the sidecar. When OPQ off: unchanged (byte-identical).

## 4. Config + lifecycle (`atire/atire_segment_index*`)

- New `long set_pq_opq(long enable)` — opt-in, must be called after `set_pq_config` (PQ configured) and before the first flush; persisted in `pq.config` (version bump, back-compat: absent ⇒ 0); immutable once set (idempotent same value, nonzero on a different value or if PQ unconfigured). Mirrors the existing `set_pq_resident_tier` gate style.
- Plumb the flag into the per-segment writer (`build_pq`/eager/flush and compaction retrain all go through the same `ANT_pq_store_writer`), so a rebuilt/compacted segment re-trains `R`. Load restores the flag from `pq.config`; `.pq` self-describes `opq` in its header (the store is authoritative for what's on disk; the config flag drives NEW builds).
- Interactions: OPQ composes with both postures (replace scores ADC in rotated space; rerank shortlists via rotated ADC then rescores exact resident float — unaffected, float tier is original-space) and all resident tiers (#19 FLOAT/INT8/NONE — INT8 `.pqr` and reconstruct-from-PQ operate in the store, un-rotation handled in `reconstruct`). OPQ is orthogonal to `set_pq_config`'s m/posture/rerank_quant.

## 5. Testing

- **Recall gain (the headline):** synthetic **anisotropic** dataset (variance concentrated in a rotated subset of dims so an axis-aligned split is bad). Assert OPQ replace-posture recall@10 ≥ non-OPQ recall@10 on the same data/m/k (OPQ should strictly help here; use a margin that's robust to the generator seed). Both indexes exact-float ground-truth compared.
- **Metric-exactness:** with a hand-built orthogonal `R`, assert `apply_rotation` then `apply_rotation_transpose` round-trips to the input (‖·‖ within float eps), and `dot(Rq, Rx) == dot(q, x)` within eps — locks the "rotation preserves the metric" invariant.
- **Determinism:** two `build_pq()` runs (OPQ on) over identical data produce byte-identical `.pq` (including the `R` block) — same-platform.
- **Back-compat:** a v1 `.pq` (no OPQ) still loads and searches (non-OPQ path unchanged); an OPQ index and a non-OPQ index over the same data both return sane top-k. Default (no `set_pq_opq`) is byte-identical to today.
- **reconstruct round-trip:** under OPQ + NONE tier, reconstruct-from-PQ yields an original-space approximation whose error vs the true vector is comparable to non-OPQ (not rotated-space garbage) — guards the `Rᵀ` un-rotation.
- **Load hardening:** OPQ header flag set but truncated/mis-sized `R` block → forgiving-load degrades to empty (fallback), no over-read/over-alloc. Jacobi on a degenerate `M` (n=1, all-zero, repeated eigenvalues) returns a valid orthogonal `R` (or the caller falls back to identity) — no NaN/crash.
- ASan/UBSan sweep on the new paths (engine has no makefile sanitizer hook — build sanitizer manually or report environment-blocked, per prior sub-projects).

## 6. Sequencing (TDD tasks)

1. **Codec helpers:** `train_rotation` (Jacobi + sign-canonicalize + eigenvalue allocation), `apply_rotation`/`apply_rotation_transpose`; unit tests for orthogonality (`RᵀR≈I`), round-trip, dot-preservation, determinism, degenerate inputs. Pure, no store.
2. **Store integration:** `rotation` member + v2 sidecar (write/read/validate/back-compat) + query rotation at the 3 `adc_table` sites + `prepare_query` + `reconstruct` un-rotation + writer `finish` train/rotate/encode. Recall + reconstruct + determinism + load-hardening tests.
3. **Config + lifecycle:** `set_pq_opq` + `pq.config` persistence (back-compat) + build_pq/eager/compaction retrain wiring; reopen-persistence + compaction-retrain tests; final anisotropic recall-gain integration test.

## 7. Repo constraints

Deterministic-rebuild + forgiving-load are existing `.pq` contracts to preserve (OPQ extends both). Header changes → `rm -f obj/*.o lib/libantelope_engine.a` before rebuild; fresh worktree `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered → `bin/<name>` via `make <name>`; config setters POST-open; `.pq` header offsets are file-static in `pq_store.cpp` (extend `ANT_PQ_STORE_HEADER_SIZE` for the new `opq` i64). The eigensolver is self-contained (no external LA dependency). Confirm signatures/offsets by grep before editing.

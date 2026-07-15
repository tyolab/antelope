# Token `.mvpq` OPQ Rotation Design (token epic, sub-project 1 of 4)

**Status:** approved 2026-07-15. First of the **token `.mvpq` advanced-codec** epic — bringing the token-pool PQ store (`ANT_multivector_pq_store`, from Phase 2 #18 + PQ-backed token graph #24) up to parity with the dense `.pq` store. This sub-project mirrors the shipped dense **OPQ rotation (#22.1, 647aa7c)** for the ragged token pool. Later token sub-projects: T3 global codebook (mirror #22.2), T2 variable-k (mirror #22.3), T4 single-resident (mirror Approach A).

**Goal:** opt-in OPQ rotation for the token `.mvpq` pool — a learned orthogonal `D×D` rotation `R` applied before the subspace split, improving MaxSim recall at the same m/k, **metric-exactly** (an orthogonal rotation preserves dot/L2/cosine). Persisted, deterministic. Default off ⇒ byte-identical to today. Composes with the #24 PQ-backed token graph (`ANT_multivector_pq_source`) and the token resident tiers.

**Architecture (one sentence):** reuse the existing k-independent, metric-preserving `ANT_pq_codec::train_rotation`/`apply_rotation`/`apply_rotation_transpose` (shipped for dense #22.1) UNCHANGED; add an `R` block to the `.mvpq` sidecar (v2), rotate the pool + train the codebook in rotated space at write time, rotate the query before every ADC table build, and un-rotate only in `token_reconstruct` — so the codec, `ANT_pq_codec`, is untouched and only the store + engine config change.

**Tech stack:** C++ engine — `source/multivector_pq_store.{h,cpp}` (store: `.mvpq` v2, rotation member, query rotation, writer `train_rotation`), `atire/atire_segment_index*` (config `set_multivector_pq_opq`, `multivector_pq.config` bump, writer wiring), tests `tests/*.cpp`.

**Scope:** engine + `.mvpq` store only. No binding changes, no codec change. Default off ⇒ token per-segment path byte-identical. Token variable-k / global / single-resident are separate later sub-projects.

---

## 1. Codec — ZERO change (reuse dense #22.1 helpers)

`ANT_pq_codec::train_rotation(vectors, dimension, m, n, R)` (uncentered second-moment `M=Σxxᵀ`, NO centering → metric-preserving; deterministic cyclic Jacobi + sign-canonicalize + eigenvalue-balanced subspace allocation) and `apply_rotation`/`apply_rotation_transpose` (double-accumulated) are already shipped, k-independent, and raggedness-agnostic (they operate on a flat `n×D` row array). The token pool is already a flat `total_tokens×D` array in the writer's `buffer`, so `train_rotation(buffer, dimension, m, total_tokens, R)` applies directly. **KEY INSIGHT (same as #22.1):** `R` orthogonal ⇒ `dot(Rq,Rx)=dot(q,x)` ⇒ ADC scores computed in the rotated space are metric-EXACT with no un-rotation; only `token_reconstruct` (which must return an original-space vector) un-rotates via `Rᵀ`.

## 2. Store: `.mvpq` v2 + query rotation (`source/multivector_pq_store.{h,cpp}`)

- **Member:** `float *rotation;` (`D*D` row-major, NULL when OPQ off). Ctor inits NULL; dtor frees.
- **`.mvpq` v2 sidecar:** header gains an `opq` i64 field (0 = no rotation, 1 = R block present); a trailing `dimension*dimension` float `R` block is written after the codebook. Magic stays `ANTMVPQ1`; the u32 version field goes **1 → 2**. Header field order (v2): dim, docs, total_tokens, m, k, **opq**. Forgiving load: v1 (no opq field, no R) loads with `rotation=NULL`/opq=0 (back-compat); v2 reads opq and, when 1, the R block. Validate-before-allocate: `dimension ≤ 65536` already bounds `D²·4 ≤ 2³⁴`; the exact-file-size check accounts for the R block before any `new`. Any mismatch ⇒ degraded empty store (existing forgiving-load contract).
- **Query rotation at all THREE ADC sites** — rotate the query into R-space before `adc_table` (metric-exact, no un-rotation for scoring):
  - `token_score(t, query, metric)` — rotate `query` → `Rq`, build table on `Rq`.
  - `token_prepare_query(query)` — rotate once, build the reusable table on `Rq` (one `D`-matvec per search, not per token — mirrors the dense `prepare_query`).
  - `maxsim(docid, query_vecs, num_query_vecs)` — rotate EACH of the `num_query_vecs` query tokens before its per-query `adc_table` (the ragged MaxSim builds `num_query_vecs` tables).
  - When `rotation == NULL`, all three are byte-identical to today (no rotate).
- **`token_reconstruct(t, out)`:** if `rotation != NULL`, reconstruct in rotated space then `apply_rotation_transpose` (`Rᵀ`) into original space; else unchanged. (Used by any float-needing path / tier rescore.)

## 3. Writer: train + embed R (`ANT_multivector_pq_store_writer`)

- `create(path, dim, m, metric, opq)` — gains the `opq` flag (mirrors the dense `ANT_pq_store_writer::create` arity change).
- `finish()`: when `opq` and `total_tokens > 0`, `train_rotation` over the flattened `total_tokens×D` pool → rotate the whole pool in place → `train` the codebook and `encode` every token in rotated space; write the `opq` flag and the R block. Empty pool (`total_tokens == 0`) ⇒ `opq_flag = 0`, no R block (graceful — mirrors the #22.1 M1 empty-segment fix; derive the written flag from the actual trained `R` pointer, not the request). Non-OPQ path byte-identical to today apart from the v2/opq=0 header (v1 still loads — one-way format bump, same as dense #22.1).

## 4. Config (`atire/atire_segment_index*`)

- `long set_multivector_pq_opq(long enable)` — opt-in, requires open + `multivector_pq_configured()`; idempotent same-value; immutable once enabled; persisted. Mirrors `set_pq_opq` / `set_multivector_resident_tier`. Composes with `mvpq_posture`/tier (orthogonal).
- Getter `long multivector_pq_opq(void) { return mvpq_opq_current; }`; member `long mvpq_opq_current` (ctor init 0).
- **`multivector_pq.config`** (magic `ANTMVPQC`): bump version (currently 2 with the #24 tier field; → **3**), append an `opq` i64 after the tier value. Back-compat: v1 (3 vals: m/posture/rerank) and v2 (4 vals: +tier) ⇒ `opq=0`. Mirror the dense `pq.config` OPQ v3 addition.
- **Wire the two mvpq writer `create` sites** to pass `mvpq_opq_current`: `build_multivector_pq` (`atire_segment_index_vector.cpp:1904`) and compaction (`atire_segment_index_compaction.cpp:667`). (The `.mvec` float writer at compaction:303 is a different store — untouched.)

## 5. Composition with #24 (PQ-backed token graph + tiers)

- The `ANT_multivector_pq_source` (token graph over PQ codes) scores via `token_score_prepared`, which uses the rotated query table — so a token-HNSW built/searched over OPQ codes composes automatically (codes live in rotated space; scoring is metric-exact there).
- Token resident tiers (#24 `set_multivector_resident_tier {FLOAT,NONE}`): under NONE, `token_reconstruct` (un-rotating via `Rᵀ`) is the reconstruct-from-PQ path; under FLOAT the resident float `.mvec` is independent of R. Both compose.

## 6. Testing

- **Recall gain:** on anisotropic token data, OPQ MaxSim recall@k ≥ non-OPQ at the same m (the #22.1 fixture pattern — independent per-component draws so the axis-aligned split isn't a free exact fit). Locks the direction, not a fixed number.
- **Metric-exactness / round-trip:** a store written with OPQ, reloaded, returns `token_score`/`maxsim` consistent with a direct rotated-space ADC; `token_reconstruct` un-rotates to within codec quantization error of the original token.
- **`.mvpq` v2 persistence:** OPQ store writes v2 + R block; reopen restores `rotation`; a v1 (no-opq) file still loads (`rotation=NULL`). Forgiving load: a truncated/inconsistent R block ⇒ degraded empty store, no over-read.
- **Empty-pool graceful:** an OPQ-configured segment with zero tokens writes opq=0/no R, loads clean.
- **Config round-trip:** `set_multivector_pq_opq(1)` persists to `multivector_pq.config` v3; reopen restores `multivector_pq_opq()==1`; a v2 config (no opq) loads as 0; immutability + idempotent-same-value enforced.
- **Compose with #24:** OPQ + PQ-backed token graph (`multivector_pq` replace posture) search is sane; OPQ + NONE tier reconstruct path correct.
- **Default off byte-identity:** no `set_multivector_pq_opq` ⇒ existing token suites (`test_mvpq_store`, `test_pq_token_resident_tier`, `test_v6_*`) byte-identical.
- ASan/UBSan sweep (environment-blocked per prior sub-projects — report).

## 7. Sequencing (TDD tasks)

1. **Store `.mvpq` v2 + query rotation** (`multivector_pq_store.{h,cpp}`): rotation member, v2 load/store (forgiving), query rotation at the 3 ADC sites, `token_reconstruct` un-rotation, writer `create(...,opq)` + `train_rotation`/rotate-pool in `finish` (empty-pool graceful). Tests: v2 round-trip, v1 back-compat, metric-exact score, reconstruct un-rotation, empty-pool, forgiving load.
2. **Config + wiring** (`atire/*`): `mvpq_opq_current` + `set_multivector_pq_opq` + `multivector_pq.config` v3 + wire the two writer create sites. Tests: config round-trip/immutability, default-off byte-identity, build/compaction under OPQ.
3. **Composition + recall** (`tests/*`): OPQ + #24 token graph + tiers; recall-vs-non-OPQ direction on anisotropic data.

## 8. Repo constraints

Preserve the `.mvpq` deterministic-rebuild + forgiving-load contracts (OPQ extends them — same R+inputs ⇒ same codes). `.mvpq` v1→v2 and `multivector_pq.config` v2→v3 are add-a-field/one-way bumps mirroring dense #22.1 (old versions still load). Header changes → `rm -f obj/*.o lib/libantelope_engine.a`; fresh worktree `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered → `bin/<name>` via `make <name>`; config setters POST-open; `set_multivector_pq_opq` mirrors the `set_pq_opq` immutable-once pattern. Confirm signatures/line numbers by grep before editing.

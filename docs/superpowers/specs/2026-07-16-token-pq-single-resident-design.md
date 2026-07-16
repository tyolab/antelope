# Token `.mvpq` Single-Resident Global Codebook Design (token epic, sub-project 4 of 4 — LAST)

**Status:** approved 2026-07-16. Final sub-project of the **token `.mvpq` advanced-codec** epic, after the shipped **T1 token-OPQ** (e975fe6), **T3 token-global-codebook** (d21a964), and **T2 token-variable-k** (d78a6c7). Mirrors the shipped dense **single-resident global codebook — Approach A** (0cbc25f, the RAM-win follow-up to the #22.2 Approach-B global codebook). Closes the epic (brings `.mvpq` to full dense-`.pq` parity).

**Goal:** under global mode, dedupe the N per-segment `.mvpq` codebook copies to **one** RAM-resident `global_mvpq_codebook` (+ `global_mvpq_rotation` under OPQ) that every per-segment `ANT_multivector_pq_store` **borrows** instead of loading its own embedded copy — cutting resident token-codebook memory from N+1 copies to 1. The `.mvpq` on-disk format is **UNCHANGED** (each file still embeds its copy → self-describing, back-compatible); the borrow only skips reading that copy into RAM. Search results are byte-identical to Approach B (same codebook, just deduped) — a pure RAM win.

**Architecture decision — Approach A (mirror dense 0cbc25f), unconditional under global mode, no config toggle.** After T3 (Approach B), global mode means each `.mvpq` embeds a copy of the one frozen collection-wide codebook; correct, but N copies resident. Approach A points every segment store at the engine's single resident codebook. Because the borrow is search-identical with a pure RAM win, there is nothing to gate — exactly as dense made it the unconditional global-load behavior. **Rejected:** (B-toggle) a separate on/off flag — needless surface for a transparent win; (C) mmap / lazy-reconstruct — not the dense pattern, more complexity for no additional benefit.

**Architecture (one sentence):** the store's `load()` gains an optional borrowed-codebook (+rotation) seam guarded by `owns_codebook`/`owns_rotation` flags so a borrowing store points at the engine's resident buffers and its dtor never frees/derefs them (teardown-order-independent); the engine passes `global_mvpq_codebook`/`global_mvpq_rotation` at every `.mvpq` load site under global mode; and `rebuild_mvpq_global_codebook` adopts the dense Approach-A ordering (drop every borrowing store to NULL right after freeing the old resident codebook, using a `had_pq[]` eligibility snapshot captured before the drop, then Pass-2 reloads re-borrow the NEW resident).

**Tech stack:** C++ engine — `source/multivector_pq_store.{h,cpp}` (borrow seam + owns_* + dtor guard), `atire/atire_segment_index*` (load-site wiring + rebuild drop-all/had_pq), tests `tests/*.cpp`.

**Scope:** engine + `.mvpq` store only. No codec change. No `.mvpq` on-disk format change. No config change or version bump. No binding change. Global mode off ⇒ every path byte-identical to today.

---

## 1. Store borrow seam (`source/multivector_pq_store.{h,cpp}`)

- **Members:** add `long owns_codebook` and `long owns_rotation` (both default 1 in the private ctor). Mirror dense `ANT_pq_store` (`owns_codebook`/`owns_rotation` at pq_store.h:25-26).
- **`load()` signature** gains two trailing defaulted params: `static ANT_multivector_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric, const float *borrowed_codebook = NULL, const float *borrowed_rotation = NULL);`. Defaulting keeps every existing 4-arg caller (engine non-global sites, tests) compiling and behaving as today (owned path).
- **Borrow predicate** (mirror dense pq_store.cpp:194): `int borrow = (borrowed_codebook != NULL) && ((stored_opq == 1) == (borrowed_rotation != NULL));`. The engine only ever hands its resident codebook under global mode where the file's `k`/`dimension` match the resident buffer by construction (immutable-once k + frozen global codebook), so no byte-compare is needed — only the opq-consistency check, exactly as dense.
- **Borrow load path:** keep ALL existing validate-before-allocate + exact `expected_size` checks (the embedded codebook/rotation bytes are still on disk and counted in the size). When `borrow`: do NOT allocate or `fread` the codebook block or the rotation block into a new buffer — point `result->codebook = (float *)borrowed_codebook; result->owns_codebook = 0;` and `result->rotation = (stored_opq == 1) ? (float *)borrowed_rotation : NULL; result->owns_rotation = 0;` (mirror dense pq_store.cpp:242-246). When NOT borrowing (default / declined): today's owned path — allocate + read the embedded copy, `owns_* = 1`. Counts and codes are read identically in both cases.
- **Dtor guard:** free `codebook` only when `owns_codebook`, `rotation` only when `owns_rotation` (`counts`/`offsets`/`codes` always owned, freed as today). A borrowing store NEVER frees or dereferences the shared buffer → teardown-order-independent (the V6 `.tann` borrow-UAF class does not apply here — dtors never deref the borrowed buffer).
- **Accessor:** `long codebook_is_borrowed(void) { return owns_codebook == 0; }` (mirror dense pq_store.h:50).

## 2. Engine load-site wiring (`atire/atire_segment_index*`)

At every `.mvpq` store load site, pass the resident codebook when global mode is active and trained, else NULL (→ owned/degrade-to-B):
`const float *bcb = (mvpq_global_current && global_mvpq_codebook != NULL) ? global_mvpq_codebook : NULL; const float *brot = (bcb != NULL) ? global_mvpq_rotation : NULL;` then `ANT_multivector_pq_store::load(name, rerank_dimension_current, docs, ANT_pq_codec::METRIC_DOT, bcb, brot)`.
The three sites: `open()` (`atire_segment_index.cpp` ~1640), `rebuild_mvpq_global_codebook` Pass-2 reload (`atire_segment_index_vector.cpp` ~1732), and the `ensure`/`build_multivector_pq` reload after finish (`atire_segment_index_vector.cpp` ~2420). No behavior/format change when global mode is off (bcb == NULL everywhere).

## 3. Rebuild UAF safety (`rebuild_mvpq_global_codebook`) — the one non-mechanical change

Today (Approach B, T3) rebuild trains into locals then does a plain pointer swap because embedded copies never dangle. Under borrowing, freeing/replacing the resident codebook invalidates every borrowing store's `codebook` pointer. Adopt the dense Approach-A ordering (mirror dense `rebuild_pq_global_codebook`, atire_segment_index_vector.cpp:1009-1109):

1. Gather the full token pool across all segments (unchanged, before freeing anything).
2. Capture `char *had_pq = new char[segment_count]` — `had_pq[s] = (segments[s].multivector_pq != NULL && document_count matches && token_count > 0)` — **before** any drop (needed because the drop nulls the store pointer that the eligibility check reads).
3. Free the old resident `global_mvpq_codebook`/`global_mvpq_rotation`, and **immediately set `segments[s].multivector_pq = NULL` (delete) for every segment whose store `codebook_is_borrowed()`** — so nothing dereferences the freed buffer; those segments fail closed to the resident-float tier until Pass-2 reloads them.
4. Train + `save_mvpq_codebook` the NEW resident codebook (at `mvpq_k_current`; OPQ trains R too). On any failure here, the borrowing stores are already NULL → fail closed, `delete [] had_pq`, return nonzero (existing contract: caller must honor — the resident now holds a possibly-unpersisted codebook).
5. Pass-2: for each segment with `had_pq[s]`, re-encode its `.mvpq` with `set_external_codebook(global_mvpq_codebook, global_mvpq_rotation)`, then **reload it re-borrowing the NEW resident** (`load(..., global_mvpq_codebook, global_mvpq_rotation)` — must run AFTER the new resident is assigned) and run the existing T3 NONE-tier `token_source`/`token_index` refresh + `.tann` invalidation. `delete [] had_pq` at the end.

This preserves fail-closed semantics and the T3 UAF fix, and adds the Approach-A "no borrowing store dangles across the resident free" guarantee.

## 4. Composition

- **T2 variable-k:** the borrowed codebook is `k·dim` (engine already sizes `global_mvpq_codebook` at `mvpq_k_current`); the store points at it — no size assumption.
- **T1 OPQ:** the borrowed rotation is `D·D`; the borrow predicate enforces opq-consistency (`(stored_opq==1) == (borrowed_rotation!=NULL)`); a borrowing OPQ store rotates the query / un-rotates reconstruct through the borrowed R.
- **#24 resident tiers:** under NONE tier `token_source` wraps a borrowing store; reconstruct-from-PQ reads the borrowed codebook — safe because dtors never deref it and the resident codebook outlives the stores (engine dtor frees it after segment teardown, and even if not, borrowing dtors don't touch it → order-independent).
- **Global mode off / per-segment mode:** `bcb == NULL` → owned path → byte-identical to today.

## 5. Testing (mirror dense Approach A's `test_pq_single_resident`)

- **Borrow active:** build ≥2 segments under global mode; each loaded store `codebook_is_borrowed()` is true AND `get_codebook() == the engine's global_mvpq_codebook` pointer; token search results are byte-identical to the Approach-B (owned) baseline (same recall / same top-k).
- **Teardown-order independence (the crux):** (a) delete the engine (frees resident, then segment stores' dtors run) → no double-free/crash; (b) explicitly delete a borrowing store, then the engine → no crash. Both orders clean (owns_* guards).
- **Rebuild re-borrow:** after adding differently-distributed tokens, `rebuild_mvpq_global_codebook` frees+retrains the resident; borrowing stores are dropped then reloaded re-borrowing the NEW codebook (`get_codebook()` == new resident, != freed old); `had_pq` re-encode is NOT skipped (every eligible segment re-encoded); no UAF; search sane.
- **Degrade-to-B:** a half-borrow (opq flag mismatch between file and supplied rotation) or a NULL resident codebook → the store owns its embedded copy (`codebook_is_borrowed()` false), search still correct.
- **Composition:** borrow + k≠256 (`k·dim` borrowed codebook), borrow + OPQ (`D·D` borrowed rotation, query rotation correct), borrow + NONE tier (token_source over a borrowing store, reconstruct-from-PQ through the borrowed codebook, incl. the k+global+NONE-tier+rebuild path).
- **Forgiving/degrade:** ASan/UBSan sweep of the teardown + rebuild borrow paths (environment-blocked — report).

## 6. Sequencing (TDD tasks)

1. **Store borrow seam** (`multivector_pq_store.{h,cpp}`): `owns_codebook`/`owns_rotation` + `load` borrowed-params + borrow predicate + skip-embedded-read + dtor guard + `codebook_is_borrowed()`; unit test (a store loaded with a hand-supplied borrowed codebook points at it, is_borrowed true, reconstruct/maxsim identical to the owned load; declines on opq mismatch; dtor of a borrowing store frees nothing shared).
2. **Engine load-site wiring** (`atire/*`): pass `global_mvpq_codebook`/`global_mvpq_rotation` at the 3 load sites under global mode. Tests: borrow-active pointer identity + byte-identical search; teardown-order independence; degrade-to-B when global off.
3. **Rebuild drop-all + had_pq + composition** (`atire/*`): the Approach-A rebuild ordering; tests: rebuild re-borrow (no UAF, re-encode not skipped), and compose with k≠256 / OPQ / NONE-tier.

## 7. Repo constraints

Preserve the `.mvpq` on-disk format and deterministic-rebuild + forgiving-load contracts (the borrow is purely a RAM-load change; the file still embeds its copy). Header changes → `rm -f obj/*.o lib/libantelope_engine.a`; fresh worktree `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered → `bin/<name>` via `make <name>`; config setters POST-open. `load`'s new params are DEFAULTED so all existing callers/tests stay valid. Borrowing stores must never free/deref the shared buffer (owns_* dtor guard) → teardown-order-independent. Confirm signatures/line numbers by grep before editing.

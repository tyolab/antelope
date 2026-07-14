# Dense PQ Global Codebook — Single-Resident (Approach A) Design

**Status:** approved 2026-07-14. Follow-up on the shipped #22.2 dense-PQ global codebook (Approach B, 5bf7025). This realizes the **RAM win** #22.2 explicitly deferred.

**Background:** #22.2 made one frozen collection-wide codebook, but chose **Approach B** — every per-segment `.pq` *embeds a copy* of it — to avoid the codebase's first cross-segment codebook borrow. Consequence: under global mode the codebook is resident **N+1 times** (once in the engine's `global_pq_codebook`, plus a private copy inside each of the N segment stores). **Approach A** dedupes those N private copies to the single resident `global_pq_codebook` the engine already holds, by having each segment store *borrow* it.

**Goal:** under global mode, each `ANT_pq_store` borrows the engine's resident `global_pq_codebook` (+`global_pq_rotation` under OPQ) instead of owning a private copy — cutting resident codebook memory from N+1 to 1 copies. Opt-in is implicit (it triggers whenever global mode is on and a resident codebook is available); default (non-global) per-segment mode stays byte-identical; degrades safely to B when no resident codebook is available.

**Architecture (one sentence):** the `.pq` on-disk format is UNCHANGED (each file still embeds its codebook, staying self-describing and back-compatible), but `ANT_pq_store::load` gains an optional *borrowed* codebook+rotation; when supplied and header-consistent it points `store->codebook`/`store->rotation` at the engine's resident buffers, skips reading the embedded copy into RAM, and records `owns_codebook = owns_rotation = 0` so the destructor never frees them.

**Tech stack:** C++ engine — `source/pq_store.{h,cpp}` (borrow seam: load overload, `owns_*` flags, dtor guard, skip-embedded read), `atire/atire_segment_index*.cpp` (pass the resident codebook at every `pq_vectors` load site; rebuild ordering), tests `tests/*.cpp`.

**Scope:** engine-only, dense `.pq` only (token `.mvpq` single-resident = later follow-up). No binding changes, no `.pq`/`pq.codebook`/`pq.config` format change. Default off ⇒ per-segment path byte-identical to today.

---

## 1. The borrow seam (`source/pq_store.{h,cpp}`)

- **New store members:** `long owns_codebook; long owns_rotation;` (both default 1 — owned). Set to 0 only on the borrow path.
- **`load` overload / extension:** add an optional borrowed codebook + rotation, e.g.
  `static ANT_pq_store *load(const char *filename, long long expected_dimension, long long expected_documents, long metric, const float *borrowed_codebook, const float *borrowed_rotation);`
  (Keep the existing 4-arg signature as a thin wrapper passing `NULL, NULL` — every current caller stays valid and owns its codebook, byte-identical.)
- **Borrow decision (inside load, after header validation):** if `borrowed_codebook != NULL` AND the header is consistent with a borrow — i.e. the `.pq`'s `dimension`/`m`/`k`/`opq` match what the resident codebook was built for (dimension/m/k come from the header already; `opq` implies whether a rotation is expected, and `borrowed_rotation` must be non-NULL iff `opq==1`) — then **borrow**:
  - Do NOT allocate `codebook_buffer`; instead `result->codebook = (float *)borrowed_codebook; result->owns_codebook = 0;`.
  - If `opq==1`: `result->rotation = (float *)borrowed_rotation; result->owns_rotation = 0;` (borrowed rotation must be non-NULL — validated). If `opq==0`, rotation stays NULL/owned-N/A.
  - **Skip the embedded codebook bytes on disk:** the read sequence still reads presence and codes, but `fseek`s past the `codebook_floats` region (and, when borrowing rotation, past the trailing `rotation_floats` region) instead of `fread`-ing them into RAM. The exact-file-size validation is unchanged (it still accounts for the on-disk embedded codebook/rotation — the bytes exist on disk, we simply don't load them).
- **Fallback (borrow declined) = today's behavior:** if `borrowed_codebook == NULL` or the header is inconsistent (mismatched dim/m/k, or opq/rotation-nullness disagreement), load the embedded codebook (+rotation) into owned buffers exactly as now (`owns_* = 1`). So an unexpected/stale `.pq` degrades to B, never a mis-borrow.
- **Destructor:** free `codebook` only if `owns_codebook`, free `rotation` only if `owns_rotation`. (`presence`/`codes` are always owned — unchanged.)
- **Constructor** initializes `owns_codebook = owns_rotation = 1`.

**Consistency trust model:** global mode guarantees every segment `.pq` was encoded against the one frozen codebook, so the resident `global_pq_codebook` *is* the embedded copy. The borrow validates only cheap header fields (dim/m/k/opq) — it does NOT read the embedded bytes to byte-compare (that would defeat the RAM win). This is the same invariant B already relies on (B assumes every embedded copy equals the resident one). `reconstruct`/`score`/`scan_adc` are untouched — they read `codebook`/`rotation` through the same pointers, borrowed or owned.

## 2. Lifetime safety (the crux — first cross-segment borrow)

- **Teardown order is irrelevant.** A borrowing store's destructor only *conditionally skips* a `delete[]`; it never dereferences the borrowed codebook/rotation. So even if the engine frees `global_pq_codebook` before the segment stores, the stores' dtors are safe. This is exactly why the V6 `.tann`→`.mvec` UAF class does NOT apply here (there, a borrower dereferenced a freed buffer during teardown). The engine still frees `global_pq_codebook` in its destructor as today.
- **The one mid-life reallocation: `rebuild_pq_global_codebook()`.** It frees + retrains `global_pq_codebook`/`global_pq_rotation`, re-encodes every `.pq`, and refreshes each `segments[].pq_vectors` (reloads the store). Under A, that per-segment reload must **re-borrow the NEW resident buffers** — which happens automatically provided the new `global_pq_codebook`/`rotation` are installed as the engine's resident pointers BEFORE the per-segment refresh reloads run. This is an ordering requirement on existing code (rebuild already refreshes after retraining), asserted by a test, not new machinery. No borrowing store ever retains a pointer to a freed old codebook, because the refresh replaces the whole store.
- **Compaction / build_pq refresh:** these create/reload segment stores; under global mode each reload borrows the current resident codebook. Fine — no reallocation of `global_pq_codebook` occurs in these paths (compaction reuses the frozen codebook via the external-codebook writer; `ensure` only trains when NULL).
- **`ensure_global_pq_codebook`** trains once (when NULL) then is frozen; no reallocation after first train, so borrows taken after it are stable.

## 3. Wiring (`atire/atire_segment_index*.cpp`)

At every site that loads a segment's dense `pq_vectors` via `ANT_pq_store::load`, pass the resident codebook under global mode:
- **open path** (segment scan/load, ~`atire_segment_index.cpp:1570`): when `pq_global_current && global_pq_codebook != NULL`, call the borrow overload with `global_pq_codebook, global_pq_rotation`; else the 4-arg (owning) form.
- **`build_pq` refresh, compaction refresh, `rebuild_pq_global_codebook` refresh:** same conditional. `rebuild` must install the new resident codebook pointers before its refresh loop (see §2) — add an assertion/ordering guard.
- A single small private helper `load_segment_pq_vectors(segment, filename, docs)` that encapsulates "borrow under global mode, else own" keeps the four sites DRY and consistent.

Non-global mode: `global_pq_codebook` is NULL / `pq_global_current==0`, so every site takes the owning 4-arg path — byte-identical to today.

## 4. Testing

- **RAM dedup (the win):** build ≥3 segments under global mode; assert each segment store's `get_codebook()` pointer **equals** the engine's resident `global_pq_codebook` (same address ⇒ one resident copy, not N). Contrast: without global mode, each store owns a distinct codebook pointer.
- **Borrowed correctness:** searches/reconstruct/scan under borrowed codebook return identical results to Approach B (own-a-copy) on the same data — the borrow changes only ownership, not values. Cross-segment code comparability preserved.
- **OPQ compose:** global + OPQ — stores borrow `global_pq_rotation`; `reconstruct` un-rotates via the borrowed R correctly; results match B.
- **Variable-k compose:** global + `set_pq_k(16)` — borrowed codebook is `m·16·sub`; borrow indexes it correctly; results match B.
- **rebuild re-borrow:** after `rebuild_pq_global_codebook()` (which reallocates the resident codebook), every segment store's `get_codebook()` equals the NEW resident pointer (not a dangling old one), and search stays correct. This directly exercises the §2 ordering.
- **Fallback to B:** a segment `.pq` whose header is inconsistent with the resident codebook (e.g. simulate by loading with a mismatched expected dim/m, or with `borrowed_codebook==NULL`) loads its own embedded copy (`owns_codebook==1`) and still works — no crash, no mis-borrow.
- **Default off byte-identity:** no global mode ⇒ every store owns its codebook exactly as today; existing PQ suites unchanged.
- **Teardown safety:** destroy the engine (and drop a borrowing store) with the resident codebook freed — no double-free, no UAF. ASan/UBSan sweep (environment-blocked per prior sub-projects — report).

## 5. Repo constraints

Preserve `.pq`/`pq.codebook`/`pq.config` formats unchanged (A is RAM-only; every #22.2 file loads identically). Header changes → `rm -f obj/*.o lib/libantelope_engine.a`; fresh worktree `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered → `bin/<name>` via `make <name>`; the 4-arg `ANT_pq_store::load` wrapper keeps every existing caller valid. Confirm signatures/line numbers by grep before editing. The borrow is only ever taken from the engine-owned `global_pq_codebook`, whose lifetime spans all segment stores; store dtors never deref it (teardown-order-independent).

## 6. Sequencing (TDD tasks)

1. **Store borrow seam** (`pq_store.{h,cpp}`): `owns_*` flags + ctor/dtor guards + 5-arg `load` overload (borrow decision, skip-embedded read, fallback) + 4-arg wrapper. Unit tests: a hand-supplied borrowed codebook yields `owns_codebook==0` and `get_codebook()==supplied`, reconstruct/scan match an owned-copy store byte-for-byte; header mismatch falls back to owning; standalone (NULL) load unchanged.
2. **Engine wiring** (`atire/*`): `load_segment_pq_vectors` helper; borrow at open/build_pq/compaction refresh sites under global mode. Tests: RAM-dedup pointer-equality across segments, borrowed-search matches B, OPQ + variable-k compose, default-off owns-a-copy byte-identity.
3. **rebuild re-borrow + teardown** (`atire/*`): ensure rebuild installs new resident pointers before the per-segment refresh; tests for re-borrow after rebuild (new pointer everywhere, search correct) and teardown safety (no double-free/UAF with the resident codebook freed).

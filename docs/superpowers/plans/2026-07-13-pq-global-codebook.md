# Dense PQ Global Codebook Implementation Plan (#22 sub-project 2/3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Opt-in collection-wide (global) PQ codebook for the dense `.pq` store: train ONE frozen codebook (+ global rotation `R` when OPQ) once, reuse it for every segment build and compaction (no retrain), so codes are comparable across segments. Approach **B** — each `.pq` still embeds a copy of the shared codebook, so `.pq` format/load/store/codec are UNCHANGED; only *who trains* the codebook moves to the engine.

**Architecture:** The `ANT_pq_store_writer` gains an "external codebook" seam — `set_external_codebook(codebook, rotation)` makes `finish()` skip training and encode/embed against the supplied buffers. The engine owns a `global_pq_codebook` (+ `global_pq_rotation`) trained once from the first built segment's resident float vectors, persisted to `<dir>/pq.codebook`, loaded on open, and handed to every dense-`.pq` writer under global mode. Compaction then reuses it automatically (no retrain); `rebuild_pq_global_codebook()` is the explicit retrain+re-encode escape hatch.

**Tech Stack:** C++ engine — `source/pq_store.{h,cpp}`, `atire/atire_segment_index*`, tests `tests/*.cpp` (auto-discovered → `bin/<name>` via `make <name>`, `CHECK()`).

---

## Repo setup / gotchas (read first)

- Fresh worktree: `mkdir -p obj bin lib` + copy `external/**/*.a` from the main checkout.
- Header change (`pq_store.h`, `atire_segment_index.h`) → `rm -f obj/*.o lib/libantelope_engine.a` before rebuild.
- `make <name>` → `./bin/<name>`, exit 0 = pass. `source/*.cpp`+`tests/*.cpp` auto-discovered. ASan has no makefile hook → report environment-blocked.
- **Preserve:** default off (`pq_global_current==0`) ⇒ per-segment training path BYTE-IDENTICAL to today; forgiving-load + deterministic-rebuild `.pq` contracts.
- Composes with OPQ (#22.1, shipped): under global mode the rotation is ALSO global (one shared `R`).

## File Structure

- `source/pq_store.{h,cpp}` — writer `set_external_codebook` + `finish()` owned-vs-external refactor (Task 1).
- `atire/atire_segment_index.h` + `atire/atire_segment_index_vector.cpp` — global-codebook members, `pq.codebook` sidecar (train/persist/load), `set_pq_global_codebook` + `pq.config` v4, `build_pq` writer wiring, `rebuild_pq_global_codebook` (Tasks 2, 3).
- `atire/atire_segment_index_compaction.cpp` — compaction writer wiring (Task 3).
- `tests/test_pq_global.cpp` (new) — across all three tasks.

---

## Task 1: Writer external-codebook seam (`source/pq_store.{h,cpp}`)

**Files:** Modify `source/pq_store.h` (+2 members, +1 setter), `source/pq_store.cpp` (ctor init, `create` reset, `finish()` refactor); Test `tests/test_pq_global.cpp` (new).

- [ ] **Step 1: Write the failing test**

Create `tests/test_pq_global.cpp`:

```cpp
/*
	TEST_PQ_GLOBAL.CPP -- #22.2 global codebook. Task 1 locks the writer's
	external-codebook seam: finish() with a supplied codebook skips training,
	encodes against it, and embeds it (so load round-trips identically).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

// Train a standalone codebook over `n` D-dim rows (no OPQ), for use as an "external" codebook.
static float *train_codebook(const float *vecs, long long D, long long m, long long n)
{
	long long sub = D / m, floats = m * (long long)ANT_pq_codec::K * sub;
	float *cb = new float[floats];
	CHECK(ANT_pq_codec::train(vecs, D, m, n, cb) == 0);
	return cb;
}

static void test_external_codebook_encodes_and_embeds(void)
{
	const long long D = 8, m = 4, n = 50;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*7 + d*13) % 17) - 8);
	float *ext_cb = train_codebook(vecs, D, m, n);

	char path[] = "/tmp/ant_gcb_XXXXXX"; CHECK(mkstemp(path) >= 0);
	ANT_pq_store_writer w;
	CHECK(w.create(path, D, m, ANT_pq_codec::METRIC_L2, 0) == 0);
	w.set_external_codebook(ext_cb, NULL);              // supply codebook, no OPQ rotation
	for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
	CHECK(w.finish() == 0);

	ANT_pq_store *s = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2);
	CHECK(s != NULL && s->document_count() == n);
	// the embedded codebook must equal the supplied one (finish did not retrain)
	CHECK(memcmp(s->get_codebook(), ext_cb, (size_t)(m*(long long)ANT_pq_codec::K*(D/m))*sizeof(float)) == 0);
	// codes must equal a direct encode against the same codebook
	for (long long doc = 0; doc < n; doc++)
		{
		unsigned char expect[4];
		ANT_pq_codec::encode(vecs + doc*D, D, m, ext_cb, expect);
		CHECK(memcmp(s->codes_for(doc), expect, (size_t)m) == 0);
		}
	delete s; remove(path); delete [] ext_cb; delete [] vecs;
	printf("test_external_codebook_encodes_and_embeds OK\n");
}

static void test_non_external_path_unchanged(void)
{
	// A writer with NO external codebook trains its own — same file two runs => byte-identical (deterministic).
	const long long D = 6, m = 3, n = 40;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*5 + d*11) % 13) - 6);
	char a[] = "/tmp/ant_gna_XXXXXX", b[] = "/tmp/ant_gnb_XXXXXX";
	CHECK(mkstemp(a) >= 0); CHECK(mkstemp(b) >= 0);
	for (int pass = 0; pass < 2; pass++)
		{
		ANT_pq_store_writer w;
		CHECK(w.create(pass ? b : a, D, m, ANT_pq_codec::METRIC_L2, 0) == 0);
		for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
		CHECK(w.finish() == 0);
		}
	FILE *fa = fopen(a,"rb"), *fb = fopen(b,"rb"); CHECK(fa && fb);
	fseek(fa,0,SEEK_END); long la = ftell(fa); fseek(fb,0,SEEK_END); long lb = ftell(fb);
	CHECK(la == lb && la > 0); rewind(fa); rewind(fb);
	unsigned char *ba = new unsigned char[la], *bb = new unsigned char[lb];
	CHECK(fread(ba,1,la,fa)==(size_t)la && fread(bb,1,lb,fb)==(size_t)lb);
	CHECK(memcmp(ba, bb, la) == 0);
	fclose(fa); fclose(fb); delete[] ba; delete[] bb;
	remove(a); remove(b); delete [] vecs;
	printf("test_non_external_path_unchanged OK\n");
}

int main(void)
{
	test_external_codebook_encodes_and_embeds();
	test_non_external_path_unchanged();
	printf("ALL TESTS PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run — fails (no `set_external_codebook`)**

`rm -f obj/*.o lib/libantelope_engine.a && make test_pq_global && ./bin/test_pq_global` → FAIL to compile (`set_external_codebook` undeclared).

- [ ] **Step 3: Add the writer members + setter (`source/pq_store.h`)**

In `ANT_pq_store_writer` private members (line 52) add:
```cpp
	const float *ext_codebook; const float *ext_rotation;	// borrowed; when ext_codebook set, finish() skips training
```
In the public section (after `create`) add:
```cpp
	void set_external_codebook(const float *codebook, const float *rotation);	// use these instead of training (rotation NULL = non-OPQ)
```

- [ ] **Step 4: Implement setter + reset + `finish()` refactor (`source/pq_store.cpp`)**

In the writer ctor init `ext_codebook = NULL; ext_rotation = NULL;`. In `create()` reset them to NULL at the top (so a reused writer never carries stale externals). Add:
```cpp
void ANT_pq_store_writer::set_external_codebook(const float *codebook, const float *rotation)
{
ext_codebook = codebook;
ext_rotation = rotation;
}
```

Refactor `finish()` (the OPQ block ~486-523 and the frees) so an external codebook is used verbatim and NOT freed. Replace the `rotation`/`codebook` locals with an owned-vs-used split:

```cpp
// --- rotation: external (borrowed) OR trained (owned) OR none ---
float *owned_rotation = NULL;
const float *rotation = NULL;
if (ext_codebook != NULL)
	rotation = ext_rotation;					// borrowed (may be NULL for non-OPQ global)
else if (opq && present_count > 0)
	{
	owned_rotation = new float[dimension * dimension];
	if (ANT_pq_codec::train_rotation(present_rows, dimension, m, present_count, owned_rotation) != 0)
		{ delete [] owned_rotation; delete [] present_rows; return 1; }
	rotation = owned_rotation;
	}
if (rotation != NULL)							// rotate present_rows + buffer in place (unchanged logic)
	{
	float *tmp = new float[dimension];
	for (i = 0; i < present_count; i++)
		{ ANT_pq_codec::apply_rotation(present_rows + i*dimension, dimension, rotation, tmp); memcpy(present_rows + i*dimension, tmp, (size_t)dimension*sizeof(float)); }
	for (i = 0; i < documents; i++)
		{ ANT_pq_codec::apply_rotation(buffer + i*dimension, dimension, rotation, tmp); memcpy(buffer + i*dimension, tmp, (size_t)dimension*sizeof(float)); }
	delete [] tmp;
	}

// --- codebook: external (borrowed) OR trained (owned) ---
float *owned_codebook = NULL;
const float *codebook = NULL;
if (ext_codebook != NULL)
	codebook = ext_codebook;					// borrowed; do NOT free
else
	{
	owned_codebook = new float[codebook_floats > 0 ? codebook_floats : 1];
	if (ANT_pq_codec::train(present_rows, dimension, m, present_count, owned_codebook) != 0)
		{ delete [] owned_codebook; delete [] owned_rotation; delete [] present_rows; return 1; }
	codebook = owned_codebook;
	}
delete [] present_rows;

unsigned char *codes = new unsigned char[codes_bytes > 0 ? codes_bytes : 1];
for (i = 0; i < documents; i++)
	ANT_pq_codec::encode(buffer + i*dimension, dimension, m, codebook, codes + i*m);
```
Then in the file-write section: `opq_flag = (rotation != NULL) ? 1 : 0;`; write the codebook block from `codebook` (m·K·sub floats) and the R block from `rotation` when `rotation != NULL` — reading the SAME `codebook`/`rotation` pointers (external or owned). On EVERY exit path free ONLY the owned buffers + `codes`: replace each existing `delete [] codebook; delete [] rotation;` with `delete [] owned_codebook; delete [] owned_rotation;` (never free the borrowed externals). Verify every error/success path frees `codes`, `owned_codebook`, `owned_rotation` (and NOT `ext_*`).

NOTE (present_count==0 + external): valid — encodes absent rows (ignored), embeds the external codebook/R; `opq_flag` follows `ext_rotation != NULL`. Consistent with a global-mode empty segment.

- [ ] **Step 5: Rebuild and run — pass; existing PQ suites unchanged**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_global && ./bin/test_pq_global
for t in test_pq_store test_pq_opq test_pq_search test_pq_metrics test_pq_compaction test_pq_resident_tier test_pq_load_hardening test_pq_codec; do make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1 | sed "s/^/$t: /"; done
```
Expected: `test_pq_global` passes; all existing PQ suites unchanged (non-external path byte-identical — `test_pq_opq`'s determinism + `test_pq_store` prove it).

- [ ] **Step 6: Commit**

```bash
git add source/pq_store.h source/pq_store.cpp tests/test_pq_global.cpp
git commit -m "feat(pq): writer external-codebook seam (global codebook groundwork) (#22)"
```

---

## Task 2: Engine global codebook — train-once, persist, config, build_pq wiring (`atire/*`)

**Files:** Modify `atire/atire_segment_index.h` (members, getter, decls), `atire/atire_segment_index_vector.cpp` (sidecar, config v4, `set_pq_global_codebook`, `ensure_global_pq_codebook`, wire `build_pq`); Test `tests/test_pq_global.cpp`.

- [ ] **Step 1: Write the failing engine test**

Append engine cases to `tests/test_pq_global.cpp` (needs `#include "../atire/atire_segment_index.h"`), modeled on `tests/test_pq_metrics.cpp` (grep it for `set_vector_config` PRE-open, `open`, `set_pq_config` POST-open, `add_document(key,body,vec)` 3-arg, `flush`, `build_pq`, `search_vector`, `get_hit`). Cases:
- **train-once + persistence:** open, `set_pq_config(m, PQ_POSTURE_REPLACE, RERANK_QUANT_FLOAT)`, `set_pq_global_codebook(1)`, add docs across TWO flushes, `build_pq()` for both; assert `<dir>/pq.codebook` exists; close + reopen a fresh index (same config), assert `pq_global_codebook()==1` and `search_vector` still returns hits.
- **cross-segment comparability:** add the SAME vector `v` to segment A (flush) and segment B (second flush), `build_pq` both under global mode; load both `.pq` (via `disk_segment_*` or reopen) and assert `v` encodes to identical code bytes in both segments. (In per-segment mode they may differ — optional contrast.)
- **default-off byte-identity:** an index WITHOUT `set_pq_global_codebook` builds `.pq` exactly as today (existing suites already cover; a spot assert that `pq.codebook` is absent).

- [ ] **Step 2: Run — fails (`set_pq_global_codebook`/`pq_global_codebook` undeclared)**

- [ ] **Step 3: Members + getter + decls (`atire/atire_segment_index.h`)**

Add (beside `pq_opq_current`): `long pq_global_current; float *global_pq_codebook; float *global_pq_rotation;`. Getter `long pq_global_codebook(void) { return pq_global_current; }`. Declare `long set_pq_global_codebook(long enable);`, `long ensure_global_pq_codebook(long which);`, `long load_pq_codebook(void);`, `long save_pq_codebook(void);`, `long rebuild_pq_global_codebook(void);`. Initialize `pq_global_current = 0; global_pq_codebook = NULL; global_pq_rotation = NULL;` in the ctor; free the two buffers in the engine dtor.

- [ ] **Step 4: `pq.config` v4 — persist `global`**

In `save_pq_config` (`atire_segment_index_vector.cpp` ~507): `version = 4u`; after `opq`, write `long long global = pq_global_current;`. In `load_pq_config` (~471): accept `version == 4u` (extend the `(version != 1u && ...)` guard); after the v3 `opq` read, add `if (version == 4u) { read global, validate ∈{0,1}, else degrade }`; assign `pq_global_current = (long)global;` (default 0 for v1–v3).

- [ ] **Step 5: `pq.codebook` sidecar (train-once / persist / load / forgiving)**

Implement `save_pq_codebook` (atomic temp+rename): magic `ANTPQGCB`, u32 version 1, i64 dimension/m/k/opq, then (opq? `global_pq_rotation` D·D floats) + `global_pq_codebook` m·K·(D/m) floats. `load_pq_codebook`: read+validate (magic, version, dim==`vector_dimension_current`, m==`pq_m_current`, k==K, opq==`pq_opq_current`, exact file size; validate-before-allocate, D≤65536 bounds D²) → populate the two buffers; any mismatch ⇒ leave them NULL (untrained). Call `load_pq_codebook()` in `open()` after `load_pq_config()` when `pq_global_current`.

`ensure_global_pq_codebook(long which)`: if `global_pq_codebook != NULL` return 0; else extract segment `which`'s present float rows from `segments[which].vectors` (the resident float store; if a tiered/NONE segment lacks resident float, read the on-disk `.vec` — mirror how compaction's `pq_float_src` obtains floats), then: if `pq_opq_current`, `train_rotation`→`global_pq_rotation`, rotate rows; `ANT_pq_codec::train`→`global_pq_codebook`; `save_pq_codebook()`. Return nonzero on failure (caller falls back to per-segment training for safety, OR fails the build — pick fail-soft: if ensure fails, skip external and let the writer train per-segment; document it).

- [ ] **Step 6: `set_pq_global_codebook` + wire `build_pq`**

`set_pq_global_codebook(long enable)`: require open + `pq_configured()`; idempotent; immutable once on; persist via `save_pq_config()` (revert on failure). Mirror `set_pq_opq`'s guards.

At the `build_pq` writer site (`atire_segment_index_vector.cpp:1286`): when `pq_global_current`, call `ensure_global_pq_codebook(which)` then `w.set_external_codebook(global_pq_codebook, global_pq_rotation)` after `create` and before the append loop. (If ensure failed / returned NULL codebook, skip `set_external_codebook` — the writer trains per-segment, fail-soft.)

- [ ] **Step 7: Rebuild and run — engine tests pass; suites green**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_global && ./bin/test_pq_global
for t in test_pq_store test_pq_opq test_pq_search test_pq_metrics test_pq_config test_pq_compaction test_pq_resident_tier test_pq_load_hardening test_pq_backfill test_pq_codec; do make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1 | sed "s/^/$t: /"; done
```
Expected: global train-once/persistence/comparability pass; existing suites unchanged.

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index_vector.cpp atire/atire_segment_index.cpp tests/test_pq_global.cpp
git commit -m "feat(pq): engine global codebook — train-once, pq.codebook sidecar, config v4, build_pq wiring (#22)"
```
(Stage `atire_segment_index.cpp` too if the ctor init / dtor free lives there.)

---

## Task 3: Compaction no-retrain + rebuild (`atire/*`)

**Files:** Modify `atire/atire_segment_index_compaction.cpp` (writer wiring), `atire/atire_segment_index_vector.cpp` (`rebuild_pq_global_codebook`); Test `tests/test_pq_global.cpp`.

- [ ] **Step 1: Write the failing tests**

Append: **no-retrain compaction** — build ≥2 global-mode segments, capture `pq.codebook` bytes, force a compaction (`maintain()` / the test's compaction trigger — grep `test_pq_compaction.cpp` for how it compacts), then assert `pq.codebook` bytes are UNCHANGED and every merged `.pq`'s embedded codebook equals the global codebook (no retrain); search sane. **rebuild** — after adding more differently-distributed docs, `rebuild_pq_global_codebook()`; assert the new `pq.codebook` differs from the captured one, every segment now embeds the new codebook, and recall vs exact-float stays sane. **OPQ composition** — repeat train-once + comparability with `set_pq_opq(1)` before the first build (one global R+codebook; comparability holds).

- [ ] **Step 2: Run — fails (`rebuild_pq_global_codebook` undeclared / compaction retrains)**

- [ ] **Step 3: Wire compaction to the global codebook**

At the compaction writer site (`atire_segment_index_compaction.cpp:502`): when `pq_global_current`, `ensure_global_pq_codebook` (the codebook already exists post-open, so this is a no-op returning the resident one) then `w.set_external_codebook(global_pq_codebook, global_pq_rotation)` after `create`, before appends. This makes compaction reuse the frozen codebook — no retrain. (Fail-soft: if the global codebook is somehow NULL, fall through to per-segment training.)

- [ ] **Step 4: Implement `rebuild_pq_global_codebook`**

`rebuild_pq_global_codebook()`: require open + `pq_configured()` + `pq_global_current`; gather present float rows across ALL segments (resident float or on-disk `.vec`, as `ensure_*` does); free the current `global_pq_codebook`/`global_pq_rotation`; retrain (OPQ: `train_rotation`+rotate then `train`; else `train`); `save_pq_codebook()`; then re-encode every segment: for each, create a writer, `set_external_codebook(new global)`, append that segment's float rows, `finish()` overwriting its `.pq`, and refresh the in-memory `segments[which].pq_vectors` (mirror how `build_pq` refreshes the resident store). Return nonzero on any failure (leave prior state consistent where possible). Keep it straightforward — correctness over cleverness.

- [ ] **Step 5: Rebuild and run — all pass**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_global && ./bin/test_pq_global
for t in test_pq_store test_pq_opq test_pq_search test_pq_metrics test_pq_config test_pq_compaction test_pq_resident_tier test_pq_hnsw test_pq_hnsw_tiered test_pq_hnsw_prepared test_pq_load_hardening test_pq_backfill test_pq_codec; do make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1 | sed "s/^/$t: /"; done
```
Expected: no-retrain compaction + rebuild + OPQ composition pass; full PQ suite green.

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index_compaction.cpp atire/atire_segment_index_vector.cpp tests/test_pq_global.cpp
git commit -m "feat(pq): compaction reuses global codebook (no retrain) + rebuild_pq_global_codebook (#22)"
```

---

## Self-review notes

- **Spec coverage:** writer seam (Task 1) → spec §2; engine train-once/persist/config/build_pq (Task 2) → §1,§3,§4; compaction no-retrain + rebuild (Task 3) → §4,§1. OPQ composition (§ throughout) tested in Task 3; tiers orthogonal (no code — codes reference the embedded codebook as today).
- **Type/name consistency:** `set_external_codebook`, `ext_codebook`/`ext_rotation`, `owned_codebook`/`owned_rotation`, `pq_global_current`/`pq_global_codebook()`/`set_pq_global_codebook`, `global_pq_codebook`/`global_pq_rotation`, `ensure_global_pq_codebook`/`load_pq_codebook`/`save_pq_codebook`/`rebuild_pq_global_codebook`, `pq.config` v4, `pq.codebook` magic `ANTPQGCB` — consistent across tasks.
- **Byte-identity:** external path only engages when `pq_global_current`; default-off writer trains as today (Task 1 non-external determinism test locks it).
- **Leak safety:** `finish()` frees ONLY owned buffers; engine frees `global_pq_*` in dtor; borrowed externals never freed by the writer.
- **Fail-soft:** if `ensure_global_pq_codebook` fails, writers fall back to per-segment training (no hard build failure).
- **Line numbers indicative** — confirm by grep before editing.

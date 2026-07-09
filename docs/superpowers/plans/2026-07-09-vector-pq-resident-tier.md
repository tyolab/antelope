# Dense PQ Resident-Tier Policy (issue #19) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Realize the dense-PQ replace-posture memory win — a per-index `set_pq_resident_tier({FLOAT,INT8,NONE})` knob selects what stays resident beside the always-resident `.pq` codes (float `.vec` = today's default, a persistent per-segment int8 rerank sidecar `.pqr`, or nothing), so `INT8` drops the float from RAM and `RERANK_QUANT_INT8` stops aliasing float.

**Architecture:** A new immutable `pq_resident_tier_current` (persisted in `pq.config`, version bumped, absent field → FLOAT back-compat) drives the per-segment load decision in `append_segment`: `segments[].vectors` becomes the tier-typed rerank/fallback store (int8 `.pqr` / NULL / float `.vec`), while a degraded `.pq` always falls back to resident float. `build_pq()` and compaction additionally write/rebuild the int8 `.pqr` under INT8. The replace gatherer is unchanged; the rerank gatherer rescoring now runs through whatever `vectors` holds (float or int8), reconstructing from PQ codes when `.pqr` is absent.

**Tech Stack:** C++ (`source/`, `atire/`), reusing `ANT_pq_store` (`.pq`, ADC), the V4 int8 `ANT_vector_store_writer` (`QUANT_REPLACE`), and the per-segment sidecar + compaction + backfill + eager machinery. Builds directly on dense-PQ Phase 1 (`fe3d5a7`).

**Spec:** `docs/superpowers/specs/2026-07-09-vector-pq-resident-tier-design.md`

**Milestones:** config + persistence + accessor after Task 1; `.pqr` build under INT8 after Task 2; tier-driven segment load after Task 3; rerank through int8 + NONE replace-only after Task 4; compaction rebuild after Task 5; recall + byte-identical lock + sanitizer after Task 6. FLOAT is the default and stays byte-identical to Phase 1; this is a shippable opt-in memory win.

---

## Repo facts every task needs

- **Build:** `make all` then `make engine_lib`. **NO header dependency tracking** — after editing ANY `.h`, `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding or you link a stale/inconsistent archive (has caused runtime SEGVs). Tasks touching a header MUST clear objs.
- **Tests:** `tests/*.cpp` auto-discovered (a new `source/*.cpp` is auto-linked too — no makefile edit). Build+run one: `make <name> && ./bin/<name>`. Convention: `#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)`, a `main()` that runs the tests and prints a PASS line, exit 0 on success. Mirror `tests/test_pq_search.cpp`.
- **ASan/UBSan:** no make target; use `make all engine_lib <tests> CC='g++ -fsanitize=address,undefined -g'` (CC is a plain assignment, so this applies to compile+link and keeps the makefile's `-ldl -lpthread -lz …`), run with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1`. The pre-existing `ANT_file::setvbuff` leak + legacy-lexical misaligned-pointer UB are OUT OF SCOPE. After an ASan build the objects are instrumented → a full clean rebuild (`rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib`) is required before a normal (non-ASan) link, else `undefined reference to __asan_version_mismatch_check`.
- **Worktree already set up:** branch `feature/vector-pq-resident-tier` under `.worktrees/vector-pq-resident-tier`, `obj/bin/lib` present, prebuilt `external/**/*.a` copied. Config setters are POST-open (`directory != NULL`, vectors configured). Repo-wide `-fPIC`.
- **PQ config today (`atire/atire_segment_index_vector.cpp:457-554`):** `pq.config` = magic `"ANTPQCF1"` (8) + `version` u32 (`==1`) + `m`/`posture`/`rerank_quant` i64. `load_pq_config()` returns 0 and leaves members at defaults when absent. `set_pq_config(m,posture,rerank_quant)` is immutable-once-set, rejects V4-int8 coexistence. Members (`atire/atire_segment_index.h:119-121`): `pq_m_current` (0=off), `pq_posture_current`, `pq_rerank_quant_current`; ctor inits at `atire/atire_segment_index.cpp:90-93`. Enums `{PQ_POSTURE_REPLACE=0, PQ_POSTURE_RERANK=1}`, `{RERANK_QUANT_FLOAT=0, RERANK_QUANT_INT8=1}`.
- **Segment load today (`atire/atire_segment_index.cpp:1533-1568`):** when `vector_dimension_current != 0`, loads `.vectors` = `.qvec` int8 → `.vec` float fallback (always); `exact_vectors` only under `QUANTIZE_EXACT`; then `pq_vectors` from `.pq` (degraded → NULL). Teardown deletes `pq_vectors` at every site (`atire/atire_segment_index.cpp:150-151` dtor; `atire/atire_segment_index_compaction.cpp:607-609` shuffle).
- **`.pqr` is an int8 `ANT_vector_store`** written exactly like `.qvec` in `build_quantized()` (`atire/atire_segment_index_vector.cpp:1029-1032`): `ANT_vector_store_writer w; w.create(pqr_name, dim); w.set_quantization(ANT_vector_store_writer::QUANT_REPLACE); w.append(float_row_or_NULL)...; w.finish();` — but under a distinct `"pqr"` extension and **never `remove()` the float `.vec`** (unlike `build_quantized`, which drops it). Loaded via `ANT_vector_store::load(pqr_name, dim, docs)` → `is_quantized()==1`.

---

## Task 1: Resident-tier config — `set_pq_resident_tier` + `pq.config` v2 + back-compat + NONE⊥rerank + accessor

**Files:** Modify `atire/atire_segment_index.h`, `atire/atire_segment_index.cpp` (ctor init), `atire/atire_segment_index_vector.cpp` (config load/save/set); Test `tests/test_pq_resident_tier.cpp`.

- [ ] **Step 1: Header** (`atire/atire_segment_index.h`) — add the tier enum, member, setter/getter, and a test accessor. Place the enum next to the PQ posture enums; the member next to `pq_rerank_quant_current` (line ~121); the methods next to `set_pq_config` (line ~269) and `disk_segment_has_pq` (line ~369):

```cpp
enum { PQ_TIER_FLOAT = 0, PQ_TIER_INT8 = 1, PQ_TIER_NONE = 2 };
// member (near pq_rerank_quant_current):
long pq_resident_tier_current;      // PQ_TIER_FLOAT (default) / PQ_TIER_INT8 / PQ_TIER_NONE
// public (near set_pq_config):
long set_pq_resident_tier(long tier);   // 0 ok; nonzero: not open / PQ or vectors unconfigured / invalid tier / NONE+RERANK / already set to a DIFFERENT tier (immutable)
long pq_resident_tier(void) { return pq_resident_tier_current; }
// public (near disk_segment_has_pq): test accessor
long disk_segment_resident_tier(long long which);   // PQ_TIER_FLOAT/INT8/NONE from the loaded segments[which].vectors; -1 if which out of range
```

- [ ] **Step 2: Failing test `tests/test_pq_resident_tier.cpp`** — config semantics only (no `.pqr`/search yet):

```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../atire/atire_segment_index.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)

static const char *DIR = "/tmp/test_pq_resident_tier_idx";

static ATIRE_segment_index *fresh(long posture)
{
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);   // BEFORE open()
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_pq_config(4, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);   // POST open()
	return idx;
}

static void test_default_is_float(void)
{
	ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_REPLACE);
	CHECK(idx->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_FLOAT);
	delete idx;
}

static void test_set_and_immutable(void)
{
	ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_REPLACE);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);   // idempotent same
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_NONE) != 0);   // different -> reject
	CHECK(idx->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_INT8);
	delete idx;
}

static void test_none_rejects_rerank(void)
{
	ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_RERANK);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_NONE) != 0);   // NONE + rerank rejected
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);
	delete idx;
}

static void test_invalid_and_unconfigured(void)
{
	ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_REPLACE);
	CHECK(idx->set_pq_resident_tier(7) != 0);              // invalid tier
	delete idx;
	// PQ unconfigured: open + vectors but no set_pq_config
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx2 = new ATIRE_segment_index();
	CHECK(idx2->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);   // BEFORE open()
	CHECK(idx2->open(DIR) == 0);
	CHECK(idx2->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) != 0);   // PQ not configured
	delete idx2;
}

static void test_persist_and_backcompat(void)
{
	{
		ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_REPLACE);
		CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);
		delete idx;
	}
	// reopen -> tier restored from pq.config v2
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->pq_configured());
	CHECK(idx->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_INT8);
	delete idx;

	// Phase-1 pq.config (v1, no tier field) loads as FLOAT: rewrite the file as a v1 record.
	char path[2048]; snprintf(path, sizeof(path), "%s/pq.config", DIR);
	FILE *fp = fopen(path, "wb");
	unsigned long long magic; memcpy(&magic, "ANTPQCF1", 8);
	unsigned int version = 1u; long long m = 4, posture = 0, rq = 0;
	fwrite(&magic, sizeof(magic), 1, fp); fwrite(&version, sizeof(version), 1, fp);
	fwrite(&m, sizeof(m), 1, fp); fwrite(&posture, sizeof(posture), 1, fp); fwrite(&rq, sizeof(rq), 1, fp);
	fclose(fp);
	ATIRE_segment_index *idx2 = new ATIRE_segment_index();
	CHECK(idx2->open(DIR) == 0);
	CHECK(idx2->pq_configured());
	CHECK(idx2->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_FLOAT);   // absent field -> FLOAT
	delete idx2;
}

int main(void)
{
	test_default_is_float();
	test_set_and_immutable();
	test_none_rejects_rerank();
	test_invalid_and_unconfigured();
	test_persist_and_backcompat();
	printf("test_pq_resident_tier PASSED\n");
	return 0;
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `cd .worktrees/vector-pq-resident-tier && rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: compile error — `PQ_TIER_FLOAT` / `set_pq_resident_tier` not declared.

- [ ] **Step 4: Implement**

Ctor init (`atire/atire_segment_index.cpp`, next to line 92 `pq_rerank_quant_current = 0;`):
```cpp
pq_resident_tier_current = PQ_TIER_FLOAT;
```

`load_pq_config()` (`atire/atire_segment_index_vector.cpp:457`) — accept v1 (no tier → FLOAT) and v2 (read tier). Replace the version guard and the tail. The current body rejects `version != 1u`; change to accept `1u` or `2u`, and after reading `rerank_quant`, conditionally read the tier for v2:
```cpp
long long m, posture, rerank_quant, tier = PQ_TIER_FLOAT;
...
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != want
    || fread(&version, sizeof(version), 1, fp) != 1 || (version != 1u && version != 2u)
    || fread(&m, sizeof(m), 1, fp) != 1 || m < 1 || m > 65536
    || fread(&posture, sizeof(posture), 1, fp) != 1 || (posture != 0 && posture != 1)
    || fread(&rerank_quant, sizeof(rerank_quant), 1, fp) != 1 || (rerank_quant != 0 && rerank_quant != 1))
    { fclose(fp); return 0; }
if (version == 2u)
    {
    if (fread(&tier, sizeof(tier), 1, fp) != 1 || tier < 0 || tier > 2)
        { fclose(fp); return 0; }
    }
fclose(fp);
pq_m_current = m;
pq_posture_current = (long)posture;
pq_rerank_quant_current = (long)rerank_quant;
pq_resident_tier_current = (long)tier;
return 0;
```

`save_pq_config()` (`atire/atire_segment_index_vector.cpp:488`) — bump `version = 2u` and append the tier i64 after `rerank_quant`:
```cpp
unsigned int version = 2u;
...
long long tier = pq_resident_tier_current;
...
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
    || fwrite(&m, sizeof(m), 1, fp) != 1
    || fwrite(&posture, sizeof(posture), 1, fp) != 1
    || fwrite(&rerank_quant, sizeof(rerank_quant), 1, fp) != 1
    || fwrite(&tier, sizeof(tier), 1, fp) != 1)
    { fclose(fp); remove(temp); return 1; }
```

`set_pq_resident_tier()` — new function after `set_pq_config()` (`atire/atire_segment_index_vector.cpp:554`):
```cpp
long ATIRE_segment_index::set_pq_resident_tier(long tier)
{
if (directory == NULL || vector_dimension_current == 0)
    return 1;                       // must be open with vectors
if (!pq_configured())
    return 1;                       // PQ must be configured first
if (tier != PQ_TIER_FLOAT && tier != PQ_TIER_INT8 && tier != PQ_TIER_NONE)
    return 1;
if (tier == PQ_TIER_NONE && pq_posture_current == PQ_POSTURE_RERANK)
    return 1;                       // NONE is replace-only (no resident store to rescore)
if (pq_resident_tier_current != tier && pq_resident_tier_current != PQ_TIER_FLOAT)
    return 1;                       // immutable once moved off the default
if (pq_resident_tier_current == tier)
    return 0;                       // idempotent
long previous = pq_resident_tier_current;
pq_resident_tier_current = tier;
if (save_pq_config() != 0)
    { pq_resident_tier_current = previous; return 1; }
return 0;
}
```
(Immutability rule: only a transition **from** the default FLOAT is allowed; once a non-FLOAT tier is set, any different tier — including back to FLOAT — is rejected. The idempotent same-tier check makes a repeated INT8→INT8 a no-op success.)

`disk_segment_resident_tier()` — new test accessor next to `disk_segment_has_pq` (`atire/atire_segment_index_vector.cpp:1119`):
```cpp
long ATIRE_segment_index::disk_segment_resident_tier(long long which)
{
if (which < 0 || which >= segment_count)
    return -1;
if (segments[which].vectors == NULL)
    return PQ_TIER_NONE;
return segments[which].vectors->is_quantized() ? PQ_TIER_INT8 : PQ_TIER_FLOAT;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `cd .worktrees/vector-pq-resident-tier && rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: `test_pq_resident_tier PASSED`. Also `make test_pq_config && ./bin/test_pq_config` → still PASS (v2 config round-trips; existing PQ config semantics unchanged).

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_pq_resident_tier.cpp
git commit -m "feat(pq): resident-tier config (set_pq_resident_tier + pq.config v2 + back-compat)"
```

---

## Task 2: `.pqr` int8 rerank sidecar built by `build_pq()` under INT8 + eager

**Files:** Modify `atire/atire_segment_index_vector.cpp` (`build_pq`); Test `tests/test_pq_resident_tier.cpp` (extend).

- [ ] **Step 1: Write the failing test** — append to `tests/test_pq_resident_tier.cpp` a helper that indexes dense docs then asserts the `.pqr` file exists on disk after `build_pq()` under INT8, and does not exist under FLOAT. Add an accessor-free on-disk check (the resident-store assertion comes in Task 3):

```cpp
#include <sys/stat.h>
static int file_exists(const char *dir, long long generation, const char *ext)
{
	char path[2048]; struct stat st;
	// segment filenames are seg_<generation>.<ext> in the index dir
	snprintf(path, sizeof(path), "%s/seg_%lld.%s", dir, (long long)generation, ext);
	return stat(path, &st) == 0;
}

// Index ~24 dense docs (dim 16), flush, build_pq; return the flushed generation via an out-param.
static ATIRE_segment_index *build_indexed(long posture, long tier, long long *gen_out)
{
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(idx->set_pq_config(4, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_pq_resident_tier(tier) == 0);
	float v[16];
	for (int d = 0; d < 24; d++)
		{
		char name[64]; snprintf(name, sizeof(name), "doc%d", d);
		for (int j = 0; j < 16; j++) v[j] = (float)((d*7 + j*3) % 11) / 10.0f;
		CHECK(idx->add_document(name, (const unsigned char *)"body words here", 15, v) >= 0);
		}
	CHECK(idx->flush() == 0);
	*gen_out = idx->disk_segment_generation(0);   // atire/atire_segment_index.h:324 -> segments[0].generation
	CHECK(idx->build_pq() == 0);
	return idx;
}

static void test_pqr_built_under_int8(void)
{
	long long gen;
	ATIRE_segment_index *idx = build_indexed(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_INT8, &gen);
	CHECK(idx->disk_segment_has_pq(0) == 1);
	CHECK(file_exists(DIR, gen, "pqr"));      // int8 rerank sidecar written
	CHECK(file_exists(DIR, gen, "vec"));      // float .vec still on disk (never removed)
	delete idx;
}

static void test_pqr_absent_under_float(void)
{
	long long gen;
	ATIRE_segment_index *idx = build_indexed(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_FLOAT, &gen);
	CHECK(idx->disk_segment_has_pq(0) == 1);
	CHECK(!file_exists(DIR, gen, "pqr"));      // FLOAT tier writes no .pqr
	delete idx;
}
```
Add both to `main()`. (`disk_segment_generation(which)` is a real accessor, `atire/atire_segment_index.h:324`. Segment filenames are `seg_<generation>.<ext>` per `segment_filename`.)

- [ ] **Step 2: Run to verify it fails**

Run: `cd .worktrees/vector-pq-resident-tier && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: FAIL at `file_exists(DIR, gen, "pqr")` — `build_pq` does not yet write `.pqr`.

- [ ] **Step 3: Implement** — extend `build_pq()` (`atire/atire_segment_index_vector.cpp:1066`). After the `.pq` is written and `segments[which].pq_vectors` is refreshed (after the existing line 1106-1107 reload), and only when `pq_resident_tier_current == PQ_TIER_INT8`, write the int8 `.pqr` from the same float source. Add a `pqr_name` buffer alongside `vec_name`/`pq_name`, and inside the per-segment loop after the `.pq` swap:

```cpp
if (!failed && pq_resident_tier_current == PQ_TIER_INT8)
    {
    char pqr_name[4096];
    segment_filename(pqr_name, sizeof(pqr_name), generation, "pqr");
    ANT_vector_store *fsrc = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
    if (fsrc->document_count() == docs && docs > 0 && !fsrc->is_quantized())
        {
        ANT_vector_store_writer qw;
        long qfailed = qw.create(pqr_name, vector_dimension_current) != 0;
        if (!qfailed)
            qw.set_quantization(ANT_vector_store_writer::QUANT_REPLACE);
        float *qbuf = new float[vector_dimension_current];
        for (long long docid = 0; !qfailed && docid < docs; docid++)
            {
            if (fsrc->has(docid))
                { fsrc->reconstruct(docid, qbuf); qfailed = qw.append(qbuf) != 0; }
            else
                qfailed = qw.append(NULL) != 0;
            }
        delete [] qbuf;
        if (!qfailed)
            qfailed = qw.finish() != 0;       // writes int8 store to .pqr; float .vec is NOT removed
        if (qfailed)
            qw.abandon();
        }
    delete fsrc;
    }
```
(Best-effort: a failed `.pqr` leaves that segment's INT8 rerank to reconstruct-from-PQ in Task 4. `src` at the top of the loop was already `delete`d before this block runs — reload as `fsrc`. Under FLOAT/NONE this block is skipped, so no `.pqr` is written.) Eager already calls `build_pq()` at flush (`atire/atire_segment_index.cpp:1450`), so INT8 eager writes `.pqr` with no extra wiring.

- [ ] **Step 4: Run to verify it passes**

Run: `cd .worktrees/vector-pq-resident-tier && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: `test_pq_resident_tier PASSED`. `make test_pq_build && ./bin/test_pq_build` → still PASS (FLOAT-default build_pq unchanged).

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_pq_resident_tier.cpp
git commit -m "feat(pq): build_pq writes int8 .pqr rerank sidecar under INT8 tier"
```

---

## Task 3: Tier-driven segment load — populate `segments[].vectors` by tier

**Files:** Modify `atire/atire_segment_index.cpp` (`append_segment` load block); Test `tests/test_pq_resident_tier.cpp` (extend).

- [ ] **Step 1: Write the failing test** — reopen an INT8 / FLOAT / NONE index (built + `build_pq`'d, closed) and assert `disk_segment_resident_tier(0)` matches, i.e. the float is not resident under INT8/NONE:

```cpp
static void test_resident_tier_after_reopen(long tier, long posture)
{
	long long gen;
	{ ATIRE_segment_index *idx = build_indexed(posture, tier, &gen); delete idx; }   // close
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->pq_resident_tier() == tier);
	CHECK(idx->disk_segment_has_pq(0) == 1);
	CHECK(idx->disk_segment_resident_tier(0) == tier);   // FLOAT->float store, INT8->int8 .pqr, NONE->NULL
	delete idx;
}
// in main():
test_resident_tier_after_reopen(ATIRE_segment_index::PQ_TIER_FLOAT, ATIRE_segment_index::PQ_POSTURE_REPLACE);
test_resident_tier_after_reopen(ATIRE_segment_index::PQ_TIER_INT8,  ATIRE_segment_index::PQ_POSTURE_RERANK);
test_resident_tier_after_reopen(ATIRE_segment_index::PQ_TIER_NONE,  ATIRE_segment_index::PQ_POSTURE_REPLACE);
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd .worktrees/vector-pq-resident-tier && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: FAIL on the INT8/NONE cases — today the load block always loads float/int8 `.qvec`→`.vec` into `vectors` regardless of tier (`disk_segment_resident_tier` returns FLOAT for INT8/NONE).

- [ ] **Step 3: Implement** — restructure the load block in `append_segment` (`atire/atire_segment_index.cpp:1533-1568`) so `.pq` validity + tier drive `vectors`. Load `pq_vectors` **first**, then choose `vectors`. Replace the block from line 1533 (`if (vector_dimension_current != 0)`) through line 1568 with:

```cpp
segments[segment_count].vectors = NULL;
segments[segment_count].exact_vectors = NULL;
segments[segment_count].pq_vectors = NULL;

if (vector_dimension_current != 0)
	{
	long long docs = engine->get_document_count();

	/* PQ codes first: they decide what the resident rerank/fallback tier is. */
	int pq_valid = 0;
	if (pq_configured())
		{
		char pq_filename[1024];
		segment_filename(pq_filename, sizeof(pq_filename), generation, "pq");
		ANT_pq_store *pq = ANT_pq_store::load(pq_filename, vector_dimension_current, docs, vector_metric);
		if (pq->document_count() == docs && docs > 0)
			{ segments[segment_count].pq_vectors = pq; pq_valid = 1; }
		else
			delete pq;								/* degraded/absent .pq -> float fallback below */
		}

	if (pq_valid && pq_resident_tier_current == PQ_TIER_NONE)
		{
		segments[segment_count].vectors = NULL;		/* pure ADC: nothing resident beside codes */
		}
	else if (pq_valid && pq_resident_tier_current == PQ_TIER_INT8)
		{
		char pqr_filename[1024];
		segment_filename(pqr_filename, sizeof(pqr_filename), generation, "pqr");
		ANT_vector_store *iv = ANT_vector_store::load(pqr_filename, vector_dimension_current, docs);
		if (iv->document_count() == docs && docs > 0 && iv->is_quantized())
			segments[segment_count].vectors = iv;	/* resident int8 rerank tier */
		else
			{ delete iv; segments[segment_count].vectors = NULL; }	/* missing .pqr -> reconstruct-from-PQ at rerank (Task 4); do NOT load float */
		}
	else
		{
		/* FLOAT tier, or degraded/absent .pq (any tier) -> resident float .vec (with .qvec int8 as today). */
		char qvec_filename[1024];
		segment_filename(qvec_filename, sizeof(qvec_filename), generation, "qvec");
		ANT_vector_store *v = ANT_vector_store::load(qvec_filename, vector_dimension_current, docs);
		if (v->document_count() == 0)				/* no/degraded .qvec -> float .vec */
			{ delete v; v = ANT_vector_store::load(vec_filename, vector_dimension_current, docs); }
		segments[segment_count].vectors = v;

		if (quantization_current == QUANTIZE_EXACT)
			{
			ANT_vector_store *ev = ANT_vector_store::load(vec_filename, vector_dimension_current, docs);
			if (ev->document_count() == docs && ev->document_count() > 0 && !ev->is_quantized())
				segments[segment_count].exact_vectors = ev;
			else
				delete ev;
			}
		}
	}
```
Note: `vec_filename` is already computed at the top of `append_segment` (line 1498); `docs`/`generation`/`engine` are in scope. The `else`-branch preserves today's exact FLOAT behavior byte-for-byte (same `.qvec`→`.vec` fallback and `exact_vectors` logic), so a FLOAT-tier / PQ-unconfigured index loads identically. Teardown sites (dtor line 150-151, compaction 607-609) already `delete` `vectors`/`exact_vectors`/`pq_vectors` — no change needed.

- [ ] **Step 4: Run to verify it passes**

Run: `cd .worktrees/vector-pq-resident-tier && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: `test_pq_resident_tier PASSED`. `make test_pq_search test_segment_index && ./bin/test_pq_search && ./bin/test_segment_index` → still PASS (FLOAT/non-PQ load path unchanged).

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.cpp tests/test_pq_resident_tier.cpp
git commit -m "feat(pq): tier-driven segment load (int8 .pqr / NULL / float by resident tier)"
```

---

## Task 4: Rerank through the resident int8 tier + reconstruct-from-PQ fallback + NONE replace-only

**Files:** Modify `atire/atire_segment_index_vector.cpp` (`vector_candidates_pq_rerank`); Test `tests/test_pq_resident_tier.cpp` (extend).

- [ ] **Step 1: Write the failing test** — INT8-tier rerank returns a correct approximate top-k (and rescores through int8, not float, since no float is resident). Assert recall@10 vs an exact FLOAT-tier reference index over the same data ≥ a floor, and that the INT8 index has no resident float (`disk_segment_resident_tier(0)==PQ_TIER_INT8`). Also assert the `.pqr`-absent path still reranks (delete `.pqr`, reopen, search returns sane top-k). Reuse `build_indexed`; add a shared recall helper that plants 3 near-query docs:

```cpp
// Returns recall@10 of idx->search_vector(q,10) against the planted-nearest set `planted` (size np).
static double recall_at_10(ATIRE_segment_index *idx, const float *q, const long *planted, int np)
{
	long long n = idx->search_vector(q, 10);   // search_vector returns the result count
	int hit = 0;
	for (int i = 0; i < np; i++)
		{
		char want[64]; snprintf(want, sizeof(want), "doc%ld", planted[i]);
		for (long long h = 0; h < n && h < 10; h++)
			// hits carry filenames "docN"; planted[] holds the doc index N
			if (strcmp(idx->get_hit(h)->filename, want) == 0) { hit++; break; }
		}
	return (double)hit / np;
}
```
(`get_hit(i)` returns `ATIRE_segment_index::hit *` with `{filename,generation,docid,score}`, `atire/atire_segment_index.h:372`; `search_vector` returns the count.) The test builds an INT8 rerank index and a FLOAT rerank reference index over the *same* 24 docs + a query near docs {3,10,17}, and checks `recall_at_10(int8) >= 0.66` and that the int8 recall is `<=` the float-tier recall (int8 rescore ≤ float precision). Then a `.pqr`-deleted reopen still returns 10 results with the planted #1 present.

- [ ] **Step 2: Run to verify it fails** — before the implementation, an INT8-tier rerank hits `segments[which].vectors == NULL` when `.pqr` is missing OR rescoring silently uses whatever `vectors` is; the reconstruct-from-PQ fallback and the NULL-`vectors` skip need the new logic. Run and observe the recall/absent-`.pqr` assertions fail.

Run: `cd .worktrees/vector-pq-resident-tier && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: FAIL on the `.pqr`-deleted reopen case (segment skipped because `vectors == NULL` at the current guard `atire/atire_segment_index_vector.cpp:1760`).

- [ ] **Step 3: Implement** — rework `vector_candidates_pq_rerank` (`atire/atire_segment_index_vector.cpp:1740-1790`) so a valid `.pq` drives the shortlist+rescore even when `vectors == NULL`, rescoring through `vectors` (float OR int8) when present and reconstructing from PQ codes otherwise. Replace the per-segment body (the loop starting at line 1758):

```cpp
for (which = 0; which < segment_count; which++)
	{
	ANT_pq_store *pq = segments[which].pq_vectors;
	long long docs = segments[which].engine->get_document_count();
	int pq_ok = (pq != NULL && pq->document_count() == docs && docs > 0);
	if (!pq_ok && segments[which].vectors == NULL)
		continue;								/* no PQ codes and no resident store: nothing to scan */
	unsigned char *fbits = evaluate_filter_for_segment(which, filter);
	if (pq_ok)
		{
		ANT_vector_store *src = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
		long long count = 0;
		pq->scan_adc(query, vector_metric, segments[which].tombstones, segments[which].generation, shortlist, &count, pool_size, fbits);
		float *recon = NULL;
		for (p = 0; p < count; p++)
			{
			long long docid = shortlist[p].docid;
			double score;
			if (src != NULL && src->has(docid))
				score = src->score(docid, query, vector_metric);		/* resident float or int8 rescore */
			else
				{
				if (recon == NULL) recon = new float[vector_dimension_current];
				pq->reconstruct(docid, recon);							/* .pqr absent -> ADC-precision reconstruct */
				score = ANT_vector_store::kernel(recon, query, vector_dimension_current, vector_metric);
				}
			ANT_vector_candidate_insert(best, &best_count, top_k, score, segments[which].generation, docid);
			}
		delete [] recon;
		}
	else
		segments[which].vectors->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
	delete [] fbits;
	}
```
`ANT_vector_store::kernel(a, b, dimension, metric)` (static, `source/vector_store.h:82`) is the metric scorer used by `score()` — DOT/COSINE = dot (both already normalized under COSINE since the shortlist query was normalized at line 1749-1756), L2 = negated squared distance (higher=better), matching ADC's sign convention. NONE-tier never reaches rerank (rejected at config time), so no special case is needed here; the replace gatherer `vector_candidates_pq` is unchanged and already serves NONE via ADC (it scans `pq_vectors` when valid and touches `vectors` only when non-NULL — `atire/atire_segment_index_vector.cpp:1700-1713`).

Also **update the stale doc comment** above `vector_candidates_pq_rerank` (`atire/atire_segment_index_vector.cpp:1723-1738`): it currently states `segments[].vectors is float in PQ mode` and that `RERANK_QUANT_INT8` "does not change behavior in Phase 1." Replace that paragraph to reflect #19 — the resident rerank tier is now the tier-typed `segments[].vectors` (float under FLOAT, int8 `.pqr` under INT8), and rescore reconstructs from the `.pq` codes when the resident store is absent.

- [ ] **Step 4: Run to verify it passes**

Run: `cd .worktrees/vector-pq-resident-tier && rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: `test_pq_resident_tier PASSED`. `make test_pq_search && ./bin/test_pq_search` → still PASS (FLOAT rerank uses `vectors`=float via the same `src->score` path — unchanged behavior).

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_pq_resident_tier.cpp
git commit -m "feat(pq): rerank rescores through resident int8 tier + reconstruct-from-PQ fallback"
```

---

## Task 5: Compaction rebuilds `.pqr` + tier-aware resident refresh + teardown-free

**Files:** Modify `atire/atire_segment_index_compaction.cpp`; Test `tests/test_pq_resident_tier.cpp` (extend).

- [ ] **Step 1: Write the failing test** — INT8-tier index, docs across 2 flushes, `build_pq()`, capture `search_vector(q,10)` baseline, `compact(gens,2)`, assert the merged index answers with the planted #1 present, `disk_segment_has_pq(0)==1`, and `disk_segment_resident_tier(0)==PQ_TIER_INT8` (the merged segment's `.pqr` was rebuilt and is resident, not float). Reuse the multi-flush + `compact` idiom from `tests/test_pq_compaction.cpp` (open, set config+tier, add docs, flush, add docs, flush, `build_pq`, collect the two generations, `compact`).

- [ ] **Step 2: Run to verify it fails**

Run: `cd .worktrees/vector-pq-resident-tier && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: FAIL at `disk_segment_resident_tier(0)==PQ_TIER_INT8` — compaction rebuilds `.pq` but not `.pqr`; the merged segment (registered before the `.pq` rebuild) has `vectors==NULL` (INT8 + valid `.pq` + not-yet-built `.pqr`), so the tier reads NONE and rerank falls to reconstruct.

- [ ] **Step 3: Implement** — in `atire/atire_segment_index_compaction.cpp`, immediately after the existing `.pq` rebuild + refresh block (ends line 477, right after `output_segment->pq_vectors = ANT_pq_store::load(...)`), add a tier-aware `.pqr` rebuild + `vectors` refresh. This must run BEFORE Step 6's shuffle (same constraint as the `.pq` block):

```cpp
/*
	Resident-tier (#19): under INT8, rebuild the merged .pqr int8 sidecar over
	the merged float .vec and refresh output_segment->vectors so THIS session's
	rerank rescores through the compacted int8 tier.  Under FLOAT the merged
	.vec was already loaded into ->vectors by append_segment; under NONE ->vectors
	is NULL.  The float .vec is never removed.  Best-effort: a failed .pqr leaves
	->vectors NULL (rerank reconstructs from the rebuilt .pq).
*/
if (pq_configured() && pq_resident_tier_current == PQ_TIER_INT8
	&& output_segment->pq_vectors != NULL && output_segment->pq_vectors->document_count() > 0)
	{
	char out_vec2[4096], out_pqr[4096];
	segment_filename(out_vec2, sizeof(out_vec2), output_generation, vext);
	segment_filename(out_pqr, sizeof(out_pqr), output_generation, "pqr");
	long long out_docs = output_segment->engine->get_document_count();
	ANT_vector_store *fv = ANT_vector_store::load(out_vec2, vector_dimension_current, out_docs);
	if (fv->document_count() == out_docs && out_docs > 0 && !fv->is_quantized())
		{
		ANT_vector_store_writer qw;
		long qfailed = qw.create(out_pqr, vector_dimension_current) != 0;
		if (!qfailed)
			qw.set_quantization(ANT_vector_store_writer::QUANT_REPLACE);
		float *qbuf = new float[vector_dimension_current];
		for (long long d = 0; !qfailed && d < out_docs; d++)
			{
			if (fv->has(d))
				{ fv->reconstruct(d, qbuf); qfailed = qw.append(qbuf) != 0; }
			else
				qfailed = qw.append(NULL) != 0;
			}
		delete [] qbuf;
		if (!qfailed)
			qfailed = qw.finish() != 0;
		if (qfailed)
			qw.abandon();
		}
	delete fv;
	delete output_segment->vectors;
	ANT_vector_store *iv = ANT_vector_store::load(out_pqr, vector_dimension_current, out_docs);
	if (iv->document_count() == out_docs && out_docs > 0 && iv->is_quantized())
		output_segment->vectors = iv;			/* refreshed resident int8 tier */
	else
		{ delete iv; output_segment->vectors = NULL; }	/* .pqr failed -> reconstruct-from-PQ at rerank */
	}
```
(`vext` is already `"vec"` in PQ mode — line 452. `output_segment->vectors` is freed on the Step-6 shuffle teardown at line 607 as `vectors`; the delete+reload here refreshes it in place, and the final teardown frees whatever it points at — no leak, no borrowed dependent. Under FLOAT/NONE this block is skipped, so their `vectors` (float / NULL, set by append_segment) is untouched.)

- [ ] **Step 4: Run to verify it passes**

Run: `cd .worktrees/vector-pq-resident-tier && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: `test_pq_resident_tier PASSED`. `make test_pq_compaction && ./bin/test_pq_compaction` → still PASS (FLOAT-default compaction unchanged).

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_compaction.cpp tests/test_pq_resident_tier.cpp
git commit -m "feat(pq): compaction rebuilds int8 .pqr + tier-aware resident refresh"
```

---

## Task 6: Recall sanity + FLOAT-default byte-identical lock + ASan/UBSan

**Files:** Test `tests/test_pq_resident_tier.cpp` (extend, or a new `tests/test_pq_tier_recall.cpp`).

- [ ] **Step 1: Write the recall + byte-identical test** — 200 docs, dim 32, random unit vectors + 3 planted near-query docs. Build three indexes over identical data at the DEFAULT m (`set_pq_config(0, PQ_POSTURE_RERANK, RERANK_QUANT_FLOAT)`): FLOAT-tier rerank, INT8-tier rerank, and a NONE-tier replace (`PQ_POSTURE_REPLACE`). `build_pq()` each. Assert:
  - `recall_at_10(int8_rerank) >= recall_at_10(replace_adc)` (int8 rescore ≥ raw ADC),
  - `recall_at_10(int8_rerank) <= recall_at_10(float_rerank) + 1e-9` (float is the precision ceiling),
  - `recall_at_10(int8_rerank) >= 0.9`,
  - all 3 planted docs recalled by the float-tier rerank.
  - **Byte-identical lock:** a FLOAT-tier PQ rerank index and a Phase-1-style PQ rerank index (same `set_pq_config`, no `set_pq_resident_tier` call → default FLOAT) over identical data return identical top-10 `(filename, docid, score)` for a fixed query. (Since FLOAT is the default and the load `else`-branch is byte-for-byte the old path, these must match exactly.)

- [ ] **Step 2: Run → observe recall**

Run: `cd .worktrees/vector-pq-resident-tier && make test_pq_resident_tier && ./bin/test_pq_resident_tier`
Expected: PASS. If `int8_rerank < 0.9` at the default m, that is a genuine finding — raise the default-m via `set_pq_config(m,…)` with a finer m in the recall test and record the observed recall in the commit message (the default-m rule itself is Phase-1's and out of scope to change here).

- [ ] **Step 3: ASan/UBSan sweep**

```bash
cd .worktrees/vector-pq-resident-tier
rm -f obj/*.o lib/libantelope_engine.a
make all engine_lib test_pq_resident_tier test_pq_search test_pq_build test_pq_compaction CC='g++ -fsanitize=address,undefined -g'
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_resident_tier
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_search
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_build
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_compaction
```
Expected: no ASan/UBSan ERROR on the new paths (`.pqr` build/load, int8-tier rescore, reconstruct-from-PQ, compaction `.pqr` rebuild + `vectors` refresh + teardown). `detect_leaks=1` on the resident-tier + compaction tests confirms no leaked `.pqr`/`vectors`/`pq_vectors` store. The known `ANT_file::setvbuff` leak + legacy-lexical misalignment are OUT OF SCOPE. Then restore the normal build:
```bash
rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && ./bin/test_segment_index
```

- [ ] **Step 4: Commit**

```bash
git add tests/test_pq_resident_tier.cpp
git commit -m "test(pq): resident-tier recall sanity + FLOAT byte-identical lock + ASan/UBSan clean"
```

---

## Final review + finish

After Task 6: dispatch a holistic review over the whole diff (`git diff master...HEAD`) focusing on:
- **`.pqr` lifecycle:** written only under INT8 (build_pq + compaction), never removes the float `.vec`, loaded tier-aware, every `ANT_vector_store` freed on each error branch and at teardown.
- **The tier-driven load restructure:** FLOAT / PQ-unconfigured branch is byte-for-byte the old path (regression lock); INT8-missing-`.pqr` → `vectors==NULL` (NOT float) so the RAM win holds; NONE → `vectors==NULL`.
- **Rerank correctness:** rescore through int8 when resident, reconstruct-from-PQ when absent, NONE never reaches rerank; the `recon` buffer freed on every path.
- **Compaction:** `.pqr` rebuilt + `vectors` refreshed AFTER the `.pq` finalize, before the Step-6 shuffle; teardown frees the refreshed `vectors` (no leak, no borrowed-store UAF — the gatherers read `vectors`/`pq_vectors` per-query, not retained).
- **Config:** `pq.config` v2 round-trips; v1 (absent tier) → FLOAT; NONE⊥rerank rejected at config time; tier immutable once moved off FLOAT; the version guard accepts both v1 and v2.

Fix Critical/Important with regression tests. Then finishing-a-development-branch — verify the full suite green on a clean build (`rm -f obj/*.o lib/libantelope_engine.a && make all && make engine_lib && <run tests>`) before merging locally to master.

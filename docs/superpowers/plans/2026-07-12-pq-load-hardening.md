# PQ Load Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden the PQ load paths — revalidate the persisted config (m divides the dimension; posture/quant/tier in range) and bound the crafted-header token count before the `expected_size` multiply — on both the dense `.pq` and token `.mvpq` paths.

**Architecture:** Two localized changes: (1) an in-place revalidation added to `load_pq_config`/`load_multivector_pq_config` plus a one-line `open()` reorder so the dimension is loaded before the token config validates; (2) an upper-bound check on the header token count in `ANT_multivector_pq_store::load` before any size multiply (the dense store is already bounded — comment only).

**Tech Stack:** C++ engine (`atire/atire_segment_index.cpp`, `atire/atire_segment_index_vector.cpp`, `source/multivector_pq_store.cpp`, `source/pq_store.cpp`). No new files, no interface/header changes.

**Spec:** `docs/superpowers/specs/2026-07-12-pq-load-hardening-design.md`.

**Repo constraints:** whole repo `-fPIC`; **no header dependency tracking** — but this plan touches only `.cpp`, so a normal `make` suffices (if you do touch a header, `rm -f obj/*.o lib/libantelope_engine.a` first); after an ASan sweep, a full clean rebuild before a normal link; `source/*.cpp`+`tests/*.cpp` auto-discovered; tests build to `bin/<name>` via `make <name>` (`CHECK()` macro, exit 0 on pass); config setters are POST-open (`open(dir)` first). Verify accessor names by grep before use: `pq_configured()` / `multivector_pq_configured()` (both are `<field>_m_current != 0`), `disk_segment_has_pq`/`disk_segment_has_multivector_pq`. On-disk formats: `pq.config` = `unsigned long long magic("ANTPQCF1")` + `u32 version(1|2)` + `i64 m` + `i64 posture` + `i64 rerank_quant` (+ `i64 tier` if v2); `multivector_pq.config` = `char[8] "ANTMVPQC"` + `u32 version(1|2)` + `i64 m,posture,rerank_quant` (+ `i64 tier` if v2); `.mvpq` header = 52 bytes: `char[8] "ANTMVPQ1"` + `u32 version(1)` + `i64 dim,documents,total_tokens,m,k` (k==256).

---

## File Structure

- `atire/atire_segment_index.cpp` — `open()`: swap the order of `load_rerank_config()` and `load_multivector_pq_config()`.
- `atire/atire_segment_index_vector.cpp` — `load_pq_config()` (+divisibility), `load_multivector_pq_config()` (+range +divisibility).
- `source/multivector_pq_store.cpp` — `load()`: bound `toks` before `expected_size`.
- `source/pq_store.cpp` — `load()`: parity comment only.
- `tests/test_pq_load_hardening.cpp` (new) — config-reject + valid round-trip + crafted-header tests.

---

## Task 1: Config revalidation (divisibility + range) + open() reorder

**Files:**
- Modify: `atire/atire_segment_index.cpp` (open() reorder, ~lines 388-390)
- Modify: `atire/atire_segment_index_vector.cpp` (`load_pq_config` ~457-485, `load_multivector_pq_config` ~629-662)
- Test: `tests/test_pq_load_hardening.cpp` (new)

- [ ] **Step 1: Write the failing config-revalidation tests**

Create `tests/test_pq_load_hardening.cpp`. First grep to confirm the config-accessor names and the exact `set_*` signatures you'll call: `grep -n "pq_configured\|multivector_pq_configured\|set_pq_config\|set_vector_config\|set_rerank_config\|set_multivector_pq_config\|add_document(" atire/atire_segment_index.h`. Then:

```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/pq_codec.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
static const char *DIR = "/tmp/test_pq_load_hardening_idx";

static void reset_dir(void){ char c[2048]; snprintf(c,sizeof(c),"rm -rf %s && mkdir -p %s",DIR,DIR); system(c); }

/* Overwrite pq.config with a chosen (possibly invalid) v2 record. */
static void write_pq_config(long long m, long long posture, long long rq, long long tier)
{
	char path[4096]; snprintf(path,sizeof(path),"%s/pq.config",DIR);
	FILE *f = fopen(path,"wb"); CHECK(f != NULL);
	unsigned long long magic; memcpy(&magic,"ANTPQCF1",8);
	unsigned int version = 2;
	CHECK(fwrite(&magic,8,1,f)==1 && fwrite(&version,4,1,f)==1
		&& fwrite(&m,8,1,f)==1 && fwrite(&posture,8,1,f)==1 && fwrite(&rq,8,1,f)==1 && fwrite(&tier,8,1,f)==1);
	fclose(f);
}

/* Overwrite multivector_pq.config with a chosen (possibly invalid) v2 record. */
static void write_mvpq_config(long long m, long long posture, long long rq, long long tier)
{
	char path[4096]; snprintf(path,sizeof(path),"%s/multivector_pq.config",DIR);
	FILE *f = fopen(path,"wb"); CHECK(f != NULL);
	unsigned int version = 2;
	long long vals[4] = { m, posture, rq, tier };
	CHECK(fwrite("ANTMVPQC",1,8,f)==8 && fwrite(&version,4,1,f)==1 && fwrite(vals,8,4,f)==4);
	fclose(f);
}

/* dense: a config whose m does not divide the vector dimension leaves PQ unconfigured on reopen */
static void test_dense_config_bad_m_rejected(void)
{
	reset_dir();
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);	/* PRE-open */
	CHECK(ix->open(DIR) == 0);
	float v[16]; for(int j=0;j<16;j++) v[j]=(float)((j*3)%7-3)/3.0f;
	CHECK(ix->add_document("d0","body",v) >= 0);
	CHECK(ix->flush() == 0);					/* writes vector.config on disk */
	delete ix;
	write_pq_config(3, 0, 0, 0);					/* 16 % 3 != 0 -> must be rejected */
	ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->pq_configured() == 0);				/* bad-m config left PQ unconfigured */
	delete ix;
	printf("test_dense_config_bad_m_rejected PASSED\n");
}

/* dense: out-of-range posture rejected */
static void test_dense_config_bad_posture_rejected(void)
{
	reset_dir();
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	float v[16]; for(int j=0;j<16;j++) v[j]=(float)((j*3)%7-3)/3.0f;
	CHECK(ix->add_document("d0","body",v) >= 0);
	CHECK(ix->flush() == 0);
	delete ix;
	write_pq_config(4, 7, 0, 0);					/* posture 7 out of {0,1} */
	ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->pq_configured() == 0);
	delete ix;
	printf("test_dense_config_bad_posture_rejected PASSED\n");
}

/* token: a config whose m does not divide the rerank dimension leaves token-PQ unconfigured on reopen */
static void test_token_config_bad_m_rejected(void)
{
	reset_dir();
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* POST-open; writes rerank config */
	float rows[2*16]; for(int r=0;r<2;r++){ double n=0; for(int j=0;j<16;j++){ rows[r*16+j]=(float)((r*5+j*3)%7-3)/3.0f; n+=rows[r*16+j]*rows[r*16+j]; } n=sqrt(n)+1e-9; for(int j=0;j<16;j++) rows[r*16+j]/=(float)n; }
	CHECK(ix->add_document("d0","body",NULL,rows,2) >= 0);
	CHECK(ix->flush() == 0);
	delete ix;
	write_mvpq_config(3, 0, 0, 0);					/* 16 % 3 != 0 -> rejected */
	ix = new ATIRE_segment_index();
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->multivector_pq_configured() == 0);			/* bad-m token config left token-PQ unconfigured */
	delete ix;
	printf("test_token_config_bad_m_rejected PASSED\n");
}

/* regression: a VALID config still loads and reports configured (guard doesn't reject good files) */
static void test_valid_config_still_loads(void)
{
	reset_dir();
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	float v[16]; for(int j=0;j<16;j++) v[j]=(float)((j*3)%7-3)/3.0f;
	CHECK(ix->add_document("d0","body",v) >= 0);
	CHECK(ix->flush() == 0);
	delete ix;
	ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
	CHECK(ix->open(DIR) == 0);
	CHECK(ix->pq_configured() == 1);				/* valid m=4 | dim=16 loads fine */
	delete ix;
	printf("test_valid_config_still_loads PASSED\n");
}

int main(void)
{
	test_dense_config_bad_m_rejected();
	test_dense_config_bad_posture_rejected();
	test_token_config_bad_m_rejected();
	test_valid_config_still_loads();
	printf("ALL test_pq_load_hardening PASSED\n");
	return 0;
}
```

Note: confirm `set_pq_config`, `PQ_POSTURE_REPLACE`, `VECTOR_METRIC_COSINE`, and the dense/token `*_configured()` accessor spellings with the grep above; adjust literally if any differ. The 3-arg `add_document(key,body,vec)` is dense; the 5-arg `add_document(key,body,NULL,multivector,num)` is token.

- [ ] **Step 2: Run to verify the config tests FAIL**

Run: `make test_pq_load_hardening && ./bin/test_pq_load_hardening`
Expected: FAIL — `test_dense_config_bad_m_rejected` (and the token one) fail at `pq_configured() == 0` / `multivector_pq_configured() == 0` because the current loaders accept a non-dividing `m`. (`test_dense_config_bad_posture_rejected` already passes — dense config already range-checks posture — and `test_valid_config_still_loads` already passes; that's fine, they lock no-regression.)

- [ ] **Step 3: Reorder the config loads in `open()`**

In `atire/atire_segment_index.cpp`, the current order (~388-390) is:
```cpp
load_pq_config();
load_multivector_pq_config();
load_rerank_config();
```
Change to (rerank before mvpq, so `rerank_dimension_current` is set when the token config validates):
```cpp
load_pq_config();
load_rerank_config();
load_multivector_pq_config();
```

- [ ] **Step 4: Add the divisibility guard to `load_pq_config`**

In `atire/atire_segment_index_vector.cpp`, `load_pq_config()` currently reads and validates magic/version/m/posture/rerank_quant/tier, then assigns `pq_m_current` etc. Add a divisibility guard immediately BEFORE the assignments (after the `fclose(fp);` that follows the successful reads), so a non-dividing `m` leaves PQ unconfigured:
```cpp
fclose(fp);
if (vector_dimension_current != 0 && vector_dimension_current % m != 0)
	return 0;					/* m must divide the vector dimension; leave PQ unconfigured */
pq_m_current = m;
pq_posture_current = (long)posture;
pq_rerank_quant_current = (long)rerank_quant;
pq_resident_tier_current = (long)tier;
return 0;
```
(The function already returns 0 on parse failure without assigning `pq_m_current`; this new guard uses the same "return without assigning = unconfigured" contract.)

- [ ] **Step 5: Add range + divisibility guards to `load_multivector_pq_config`**

In `atire/atire_segment_index_vector.cpp`, `load_multivector_pq_config()` currently does `if (!ok) return 1;` then assigns `mvpq_m_current` etc. from `vals[]`. Insert validation between them:
```cpp
if (!ok)
	return 1;

long long m = vals[0], posture = vals[1], rq = vals[2], tier = vals[3];
if (m < 1
	|| (posture != PQ_POSTURE_REPLACE && posture != PQ_POSTURE_RERANK)
	|| (rq != RERANK_QUANT_FLOAT && rq != RERANK_QUANT_INT8)
	|| (tier != MV_TIER_FLOAT && tier != MV_TIER_NONE)
	|| (rerank_dimension_current != 0 && rerank_dimension_current % m != 0))
	return 1;					/* invalid persisted config; leave token-PQ unconfigured */

mvpq_m_current = m;
mvpq_posture_current = (long)posture;
mvpq_rerank_quant_current = (long)rq;
mvpq_resident_tier_current = (long)tier;
return 0;
```
(Confirm the enum spellings `PQ_POSTURE_REPLACE/RERANK`, `RERANK_QUANT_FLOAT/INT8`, `MV_TIER_FLOAT/NONE` exist in `atire_segment_index.h` — they do, from #18/#24 — and are visible here.)

- [ ] **Step 6: Run the config tests — all PASS**

Run: `make test_pq_load_hardening && ./bin/test_pq_load_hardening`
Expected: `test_dense_config_bad_m_rejected`, `test_dense_config_bad_posture_rejected`, `test_token_config_bad_m_rejected`, `test_valid_config_still_loads` all PASS.

- [ ] **Step 7: Regression — the existing PQ/token-PQ config + recall suites still pass**

Run:
```bash
make test_mvpq_config && ./bin/test_mvpq_config
make test_mvpq_recall && ./bin/test_mvpq_recall
make test_pq_resident_tier && ./bin/test_pq_resident_tier
```
Expected: all PASS (valid configs unaffected; the reorder is behavior-neutral for well-formed indexes). If `test_pq_resident_tier` isn't the right dense-config test name, list candidates with `ls tests/ | grep -E 'pq|resident'` and run the dense PQ config/round-trip one.

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_pq_load_hardening.cpp
git commit -m "fix(#25): revalidate persisted PQ config (m|dim + range) on load; reorder rerank/mvpq config loads"
```

---

## Task 2: Crafted-header size-overflow bound

**Files:**
- Modify: `source/multivector_pq_store.cpp` (`load`, ~lines 105-118)
- Modify: `source/pq_store.cpp` (`load`, parity comment ~lines 104-117)
- Test: `tests/test_pq_load_hardening.cpp` (add crafted-header tests)

- [ ] **Step 1: Write the failing crafted-header test**

Append to `tests/test_pq_load_hardening.cpp` (add the include `#include "../source/multivector_pq_store.h"` at top, and `#include "../source/pq_store.h"`), and add the calls to `main`:

```cpp
#include <limits.h>

/* A .mvpq header claiming a huge total_tokens must degrade to an empty store, not overflow/alloc. */
static void test_mvpq_crafted_toks_rejected(void)
{
	const char *path = "/tmp/test_pq_hardening_crafted.mvpq";
	FILE *f = fopen(path,"wb"); CHECK(f != NULL);
	long long dim=16, docs=1, toks = LLONG_MAX/2, m=8, k=256;	/* toks*m overflows if unbounded */
	unsigned int version = 1;
	CHECK(fwrite("ANTMVPQ1",1,8,f)==8 && fwrite(&version,4,1,f)==1
		&& fwrite(&dim,8,1,f)==1 && fwrite(&docs,8,1,f)==1 && fwrite(&toks,8,1,f)==1
		&& fwrite(&m,8,1,f)==1 && fwrite(&k,8,1,f)==1);
	/* deliberately DO NOT write the (impossibly huge) body */
	fclose(f);
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, dim, docs, ANT_pq_codec::METRIC_DOT);
	CHECK(s->token_count() == 0);					/* degraded empty: bound rejected the header */
	delete s; remove(path);
	printf("test_mvpq_crafted_toks_rejected PASSED\n");
}

/* Dense .pq with an out-of-cap documents count is already rejected (the 2^40 cap); confirm it degrades. */
static void test_pq_crafted_documents_rejected(void)
{
	const char *path = "/tmp/test_pq_hardening_crafted.pq";
	FILE *f = fopen(path,"wb"); CHECK(f != NULL);
	/* header layout: magic[8], u32 version, i64 dimension, i64 documents, i64 m, i64 k */
	unsigned long long magic; memcpy(&magic, ANT_PQ_STORE_MAGIC, 8);
	unsigned int version = ANT_PQ_STORE_VERSION;
	long long dim=16, documents = (1LL<<41), m=4, k=256;		/* > 2^40 cap -> rejected */
	CHECK(fwrite(&magic,8,1,f)==1 && fwrite(&version,4,1,f)==1
		&& fwrite(&dim,8,1,f)==1 && fwrite(&documents,8,1,f)==1 && fwrite(&m,8,1,f)==1 && fwrite(&k,8,1,f)==1);
	fclose(f);
	ANT_pq_store *s = ANT_pq_store::load(path, dim, documents, ANT_pq_codec::METRIC_DOT);
	CHECK(s->document_count() == 0);				/* degraded empty */
	delete s; remove(path);
	printf("test_pq_crafted_documents_rejected PASSED\n");
}
```
Add to `main` before the final print:
```cpp
	test_mvpq_crafted_toks_rejected();
	test_pq_crafted_documents_rejected();
```
Confirm `ANT_PQ_STORE_MAGIC` / `ANT_PQ_STORE_VERSION` names by grep in `source/pq_store.cpp` (they are `#define`s there); if they're file-static rather than header-visible, instead write the 8 magic bytes literally (grep the actual magic string) and the integer version literally.

- [ ] **Step 2: Run to verify the mvpq crafted test FAILS (or trips UBSan)**

Run: `make test_pq_load_hardening && ./bin/test_pq_load_hardening`
Expected: `test_mvpq_crafted_toks_rejected` FAILS — without the bound, `toks*mm` overflows (UB; may wrap to a value that mismatches the size and *coincidentally* still degrade, or may compute a bogus `expected_size`). To make the failure deterministic, first build this one test under UBSan and confirm the signed-overflow fires:
`make CC='g++ -fsanitize=address,undefined -g' test_pq_load_hardening && ./bin/test_pq_load_hardening` → expect a `runtime error: signed integer overflow` in `multivector_pq_store.cpp` at the `toks*mm` line. (`test_pq_crafted_documents_rejected` already passes — the dense 2^40 cap is pre-existing.) Then `rm -f obj/*.o lib/libantelope_engine.a` before the normal build below.

- [ ] **Step 3: Add the `toks` bound to `ANT_multivector_pq_store::load`**

In `source/multivector_pq_store.cpp`, the current sequence is (dim/docs/toks/mm/kk validated for lower bounds), then:
```cpp
long long expected_size = 52 + docs*4 + toks*mm + 256*dim*4;
if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return s; }
long long actual = ftell(in);
if (actual != expected_size) { fclose(in); return s; }
```
Reorder so the actual size is known first, then bound `toks` before the multiply:
```cpp
if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return s; }
long long actual = ftell(in);
/* toks is the only header count without an upper bound; cap it against the real file size so
   toks*mm below cannot signed-overflow (docs/dim are already pinned to expected_* above). */
if (actual < 52 || (mm > 0 && toks > (actual - 52) / mm)) { fclose(in); return s; }
long long expected_size = 52 + docs*4 + toks*mm + 256*dim*4;
if (actual != expected_size) { fclose(in); return s; }
if (fseek(in, 52, SEEK_SET) != 0) { fclose(in); return s; }
```
(`mm > 0` holds from the earlier `mm < 1` reject, but the guard keeps the division safe regardless.)

- [ ] **Step 4: Add the parity comment to `ANT_pq_store::load`**

In `source/pq_store.cpp`, just above the `expected_size` computation (~line 114), add a comment documenting why no extra bound is needed:
```cpp
/*
	#25: unlike the token .mvpq path, no extra overflow bound is needed here --
	stored_documents is capped at 2^40 and stored_dimension at 65536 (validated
	above), so stored_documents*stored_m <= 2^56 and codebook_floats*4 <= 2^26,
	both well below LLONG_MAX; the products cannot signed-overflow.
*/
long long presence_bytes = (stored_documents + 7) / 8;
```

- [ ] **Step 5: Run the crafted-header tests — all PASS**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_load_hardening && ./bin/test_pq_load_hardening`
Expected: `test_mvpq_crafted_toks_rejected PASSED`, `test_pq_crafted_documents_rejected PASSED`, and all Task-1 subtests still PASS, `ALL test_pq_load_hardening PASSED`.

- [ ] **Step 6: ASan/UBSan sweep**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a bin/test_pq_load_hardening
make CC='g++ -fsanitize=address,undefined -g' test_pq_load_hardening
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./bin/test_pq_load_hardening; echo "EXIT=$?"
```
Expected: `ALL test_pq_load_hardening PASSED`, EXIT=0, and NO `signed-integer-overflow` in the load paths (the previously-firing `toks*mm` overflow is gone). The known out-of-scope `ANT_file::setvbuff` leak is the only acceptable leak (this test opens a segment index in Task-1's cases); if it dominates, re-run with `detect_leaks=0` to confirm the PASSED line.

- [ ] **Step 7: Clean rebuild + final normal run**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a bin/test_pq_load_hardening
make test_pq_load_hardening && ./bin/test_pq_load_hardening; echo "EXIT=$?"
```
Expected: `ALL test_pq_load_hardening PASSED`, EXIT=0.

- [ ] **Step 8: Commit**

```bash
git add source/multivector_pq_store.cpp source/pq_store.cpp tests/test_pq_load_hardening.cpp
git commit -m "fix(#25): bound crafted .mvpq total_tokens before expected_size multiply (overflow UB); document dense .pq caps"
```

---

## Self-Review

**1. Spec coverage:** §1 config revalidation (reorder + `load_pq_config` divisibility + `load_multivector_pq_config` range+divisibility) → Task 1 Steps 3-5. §2 overflow bound (mvpq `toks` bound + dense parity comment) → Task 2 Steps 3-4. §4 testing (config reject dense+token, valid round-trip, crafted-header dense+token, ASan/UBSan) → Task 1 Steps 1/6/7 + Task 2 Steps 1/5/6. §3 error handling (degrade to unconfigured/empty) → the guards all use the existing return-without-assign / return-empty-store contracts. ✓

**2. Placeholder scan:** no TBD/"handle edge cases"/"similar to". Every code step shows the exact before→after. The only conditionals are grep-confirm-the-accessor-name notes, which give the exact grep and fallback. ✓

**3. Type/signature consistency:** `pq_configured()`/`multivector_pq_configured()` used consistently; `load_pq_config` returns 0 (unconfigured = `pq_m_current` unset), `load_multivector_pq_config` returns 1 (unconfigured = `mvpq_m_current` unset) — each matches its existing convention, and the tests assert via the `*_configured()` accessors (not the return value), so the convention difference is immaterial. Config on-disk layouts in the test writers match the loader reads (dense: magic u64 + u32 ver + i64 m/posture/rq/tier; token: char[8] + u32 ver + i64×4). `.mvpq` header field order in the crafted test (dim,docs,toks,m,k) matches `load`'s `read_i64(hdr+12/20/28/36/44)`. ✓

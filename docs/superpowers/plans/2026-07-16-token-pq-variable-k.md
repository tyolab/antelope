# Token `.mvpq` Variable Code-Width (k≠256) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the token `.mvpq` store use a codebook of `k` = any power of two in `[2,256]` (default 256), bit-packing each token's `m` codes to `row_bytes = (m·bits+7)/8` bytes so `.mvpq` shrinks at lower k, in exchange for graceful recall loss.

**Architecture:** Direct mirror of the shipped dense variable-code-width feature (#22.3, `source/pq_store.cpp` + `set_pq_k`/`pq.config` v5). The shared `ANT_pq_codec` already has the full variable-k machinery (`bits_for_k`, `pack_codes`/`unpack_codes`, runtime `k`), so NO codec change is needed. Add token `.mvpq` **v3** (variable-k bit-packed rows; v1/v2 kept byte-identical at k=256), an engine `set_multivector_pq_k` (immutable-once) + `multivector_pq.config` v5, and generalize the T3 global codebook + writer/compaction wiring off the hardcoded `ANT_pq_codec::K` to `mvpq_k_current`.

**Tech Stack:** C++ engine. Files: `source/multivector_pq_store.{h,cpp}` (store v3 + writer k param), `atire/atire_segment_index.{h,cpp}` (config member/getter), `atire/atire_segment_index_vector.cpp` (setter, config v5, global-codebook-at-k, build wiring), `atire/atire_segment_index_compaction.cpp` (compaction writer k), tests `tests/*.cpp`.

**Key reference (crib exact shapes):**
- Dense store `source/pq_store.cpp`: `row_bytes`/`bits`, `version = (k==256)?V2:V3`, `bits_for_k`/`pack_codes`/`unpack_codes` sites, validate-before-alloc + exact-size, header layout comment at top.
- Dense `set_pq_k` at `atire/atire_segment_index_vector.cpp:724`; dense `pq.config` v5 in `load_pq_config`/`save_pq_config` (`atire/atire_segment_index_vector.cpp:471-526`).
- Current token store `source/multivector_pq_store.{h,cpp}` (v1/v2, header already carries a `k` i64 always 256; everything currently hardcodes `ANT_pq_codec::K`).
- T3 mirror just shipped: `multivector_pq.config` v4 (`global`) in `load_multivector_pq_config`/`save_multivector_pq_config`; `multivector_pq.codebook` sidecar (`ANTMVGCB`) in `save_mvpq_codebook`/`load_mvpq_codebook`; `ensure_global_mvpq_codebook`/`rebuild_mvpq_global_codebook`.

**Codec API (already present, do NOT change — `source/pq_codec.h`):**
- `static long bits_for_k(long long k)` — `log2(k)` for a power of two in `[2,256]`; `-1` otherwise.
- `static void pack_codes(const unsigned char *codes, long long m, long long bits, unsigned char *packed)` — `m` byte-codes (`<2^bits`) → `(m·bits+7)/8` bytes, LSB-first.
- `static void unpack_codes(const unsigned char *packed, long long m, long long bits, unsigned char *codes)` — inverse.
- `train/encode/adc_table/adc_score/reconstruct` all already take a runtime `long long k`.

**Global constraints (every task):**
- Commit ONLY the files named in that task's `git add`. NEVER `git add -A`. NEVER stage build artifacts (`.o`, `.a`, `.so`, `.node`) or data sidecars (`.mvpq`, `.mvec`, `.mvpq`/`multivector_pq.*` config/codebook, `.tann`, `.pq*`) or the untracked `docs/business-strategy-2026-07-07.md`. After committing, confirm `git status --short` shows that doc still `??`.
- After editing any `.h`: `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding (no header dep tracking).
- Config setters are POST-open (need `directory != NULL`). Tests must `open(dir)` before any `set_*`.
- Tests auto-discovered: `make <name>` builds `bin/<name>`; run `./bin/<name>`. `CHECK(cond)` aborts on failure.
- Do NOT push, do NOT touch remotes. ASan/UBSan environment-blocked — report, don't attempt.

---

## Task 1: Store `.mvpq` v3 bit-packed row layout + writer `k` param

Make `ANT_multivector_pq_store` carry a runtime `k` and `row_bytes`, read/write `.mvpq` v3 (variable-k, bit-packed rows), and unpack per-row at every scoring/reconstruct site. Keep v1/v2 (k=256) byte-identical. Add `get_k()`. Give the writer a `k` param.

**Build-ordering note (important):** adding a REQUIRED `k` arg mid-signature to `ANT_multivector_pq_store_writer::create` breaks all three engine `.create` call sites (`build_multivector_pq`, compaction, `rebuild_mvpq_global_codebook`) at compile time — and `libantelope_engine.a` must compile for *any* test to link, including this task's. So Task 1 ALSO declares the `mvpq_k_current` engine member (default 256, ctor init) and updates all three call sites to pass it. Task 2 then adds only the getter/setter/config-persistence over that already-existing member. This keeps every task's build green.

**Files:**
- Modify: `source/multivector_pq_store.h`
- Modify: `source/multivector_pq_store.cpp`
- Modify: `atire/atire_segment_index.h` (declare `mvpq_k_current`)
- Modify: `atire/atire_segment_index.cpp` (ctor init `mvpq_k_current = 256;`)
- Modify: `atire/atire_segment_index_vector.cpp` (pass `mvpq_k_current` at the `build_multivector_pq` + `rebuild_mvpq_global_codebook` `.create` sites)
- Modify: `atire/atire_segment_index_compaction.cpp` (pass `mvpq_k_current` at the compaction `.create` site)
- Test: `tests/test_mvpq_variable_k.cpp` (create)

- [ ] **Step 1: Write the failing test** — `tests/test_mvpq_variable_k.cpp`

```cpp
/*
	TEST_MVPQ_VARIABLE_K.CPP -- token epic 3/4 Task 1: .mvpq v3 variable code-width.
	A writer created at k<256 produces a v3 sidecar with bit-packed rows
	(row_bytes = (m*bits+7)/8); it round-trips (reconstruct/maxsim sane) and a
	k==256 writer stays byte-identical to the pre-feature v1/v2 layout.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define DIM 8
#define MM 4			/* sub = 2 */

static void fill(long long seed, float *v)
{
	double n = 0;
	for (int j = 0; j < DIM; j++) { v[j] = (float)(((seed*7 + j*3) % 13) - 6) / 6.0f; n += v[j]*v[j]; }
	n = sqrt(n) + 1e-9;
	for (int j = 0; j < DIM; j++) v[j] /= (float)n;
}

/* build a .mvpq at the given k over NDOC docs (2 tokens each); return path via out */
static void build(const char *path, long long k, long long ndoc)
{
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, DIM, MM, k, ANT_pq_codec::METRIC_DOT, 0) == 0);
	for (long long d = 0; d < ndoc; d++)
		{
		float rows[2*DIM];
		fill(d*3 + 0, rows); fill(d*3 + 1, rows + DIM);
		CHECK(w.append(rows, 2) == 0);
		}
	CHECK(w.finish() == 0);
}

static void test_variable_k_roundtrip(void)
{
	char path[] = "/tmp/ant_mvk_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	const long long k = 16, ndoc = 20;
	build(path, k, ndoc);

	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, DIM, ndoc, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == 2*ndoc);
	CHECK(s->get_k() == k);
	/* reconstruct error is bounded (k=16 over 8 dims is lossy but finite) */
	float probe[2*DIM]; fill(0, probe); fill(1, probe + DIM);
	float rec[DIM]; s->token_reconstruct(0, rec);
	double err = 0; for (int j = 0; j < DIM; j++) { double d = rec[j] - probe[j]; err += d*d; }
	CHECK(err < 1.0);							/* not garbage */
	/* maxsim of doc 0 against its own two tokens is positive and finite */
	double ms = s->maxsim(0, probe, 2);
	CHECK(ms > 0.0 && ms < 100.0);
	remove(path);
	printf("test_variable_k_roundtrip OK\n");
}

/* k==256 writer must be byte-identical to the legacy v1 layout: version==1,
   row_bytes==m, codebook 256*dim floats. Assert the on-disk header + size. */
static void test_k256_byte_identical(void)
{
	char path[] = "/tmp/ant_mvk256_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	const long long ndoc = 12;
	build(path, 256, ndoc);

	FILE *fp = fopen(path, "rb"); CHECK(fp != NULL);
	char magic[8]; unsigned int version; long long dim, docs, toks, m, k;
	CHECK(fread(magic, 1, 8, fp) == 8 && memcmp(magic, "ANTMVPQ1", 8) == 0);
	CHECK(fread(&version, 4, 1, fp) == 1 && version == 1u);		/* k==256, no opq -> v1 */
	CHECK(fread(&dim, 8, 1, fp) == 1 && dim == DIM);
	CHECK(fread(&docs, 8, 1, fp) == 1 && docs == ndoc);
	CHECK(fread(&toks, 8, 1, fp) == 1 && toks == 2*ndoc);
	CHECK(fread(&m, 8, 1, fp) == 1 && m == MM);
	CHECK(fread(&k, 8, 1, fp) == 1 && k == 256);
	fseek(fp, 0, SEEK_END);
	long long actual = ftell(fp);
	/* v1 header 52 + counts + codes(toks*m, row_bytes==m) + codebook(256*dim) */
	long long expect = 52 + docs*4 + toks*m + 256*dim*4;
	CHECK(actual == expect);
	fclose(fp);
	remove(path);
	printf("test_k256_byte_identical OK\n");
}

int main(void)
{
	test_variable_k_roundtrip();
	test_k256_byte_identical();
	printf("ALL test_mvpq_variable_k PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_mvpq_variable_k && ./bin/test_mvpq_variable_k`
Expected: FAIL to compile/link — `create(...)` has no 5-arg (k) overload and `get_k()` does not exist. (This proves the test targets the new surface.)

- [ ] **Step 3: Store header — add `k` + `row_bytes` members and `get_k()`**

In `source/multivector_pq_store.h`, in the `private:` block of `ANT_multivector_pq_store` (near the existing `long long dimension, documents, total_tokens, m;`), add:

```cpp
	long long k;			// codebook size (power of two in [2,256]); 256 for v1/v2
	long long row_bytes;	// packed bytes per token row = (m*bits_for_k(k)+7)/8 (== m when k==256)
```

Change the `codes` comment to reflect packing and add the `get_k` accessor next to `get_m`:

```cpp
	unsigned char *codes;	// total_tokens*row_bytes packed rows, NULL when empty
```
```cpp
	long long get_k(void) { return k; }
```

Update `token_codes` to stride by `row_bytes`:

```cpp
	const unsigned char *token_codes(long long t) { return token_has(t) ? codes + t*row_bytes : 0; }
```

In the writer class, change the `create` signature to take `k`:

```cpp
	long create(const char *path, long long dim, long long m, long long k, long metric, long opq = 0);
```
and add a private `long long k;` member next to `long long dimension, m;`.

- [ ] **Step 4: Store ctor + reconstruct/maxsim/token_score — unpack per row at k**

In `source/multivector_pq_store.cpp`:

Init the new members in the private ctor (line ~12) — add `k(256), row_bytes(0)` to the initializer list.

`token_reconstruct` (lines ~34-45): replace the two `reconstruct(codes + t*m, dimension, m, ANT_pq_codec::K, codebook, ...)` calls with an unpack-then-reconstruct at the store's `k`. Rewrite the body:

```cpp
void ANT_multivector_pq_store::token_reconstruct(long long t, float *out)
{
if (!token_has(t) || codebook == 0)
	{ for (long long j = 0; j < dimension; j++) out[j] = 0.0f; return; }
long long bits = ANT_pq_codec::bits_for_k(k);
unsigned char cb[256];									/* m <= dimension <= 65536? no: m<=dimension, but m code bytes; 256 is ample since m<=dimension and dimension bounded, guard below */
unsigned char *codebuf = (m <= 256) ? cb : new unsigned char[m];
ANT_pq_codec::unpack_codes(codes + t*row_bytes, m, bits, codebuf);
if (rotation != 0)
	{
	float *tmp = new float[dimension];
	ANT_pq_codec::reconstruct(codebuf, dimension, m, k, codebook, tmp);
	ANT_pq_codec::apply_rotation_transpose(tmp, rotation, dimension, out);	/* un-rotate: x = R^T (reconstructed) */
	delete [] tmp;
	}
else
	ANT_pq_codec::reconstruct(codebuf, dimension, m, k, codebook, out);
if (m > 256) delete [] codebuf;
}
```

NOTE: confirm by grep the EXACT current `token_reconstruct` body (it already handles the OPQ `rotation != 0` un-rotation via `apply_rotation_transpose` from T1 — preserve that logic exactly; only the code source changes from `codes + t*m` unpacked-identity to `unpack_codes(codes + t*row_bytes, m, bits, codebuf)`, and `ANT_pq_codec::K` → `k`). If the current body differs, keep its structure and apply only those two substitutions.

`token_score` (~line 61-66), `token_score_prepared` (~75-89), and `maxsim` (~100-115): at each `adc_score(codes + t*m, m, ANT_pq_codec::K, table)` call, unpack first. Add a small helper at file scope near the top of the `.cpp`:

```cpp
/* unpack token t's packed row into an m-byte scratch (caller supplies buf of size >= m) */
static inline void mvpq_unpack_row(const unsigned char *codes, long long t, long long row_bytes, long long m, long long bits, unsigned char *buf)
{ ANT_pq_codec::unpack_codes(codes + t*row_bytes, m, bits, buf); }
```

Then in each scorer:
- Replace `m * ANT_pq_codec::K` table sizing with `m * k`, and `adc_table(..., ANT_pq_codec::K, ...)` with `adc_table(..., k, ...)`.
- Before each `adc_score`, unpack: `unsigned char rb[256]; unsigned char *rbuf = (m<=256)?rb:new unsigned char[m]; long long bits = ANT_pq_codec::bits_for_k(k);` then `mvpq_unpack_row(codes, t, row_bytes, m, bits, rbuf);` and call `ANT_pq_codec::adc_score(rbuf, m, k, table)`; free `rbuf` if heap. In `maxsim`'s inner loop over tokens, allocate the scratch ONCE outside the token loop (not per token).

(Grep the exact current bodies and apply these substitutions faithfully; the `rq` OPQ query-rotation from T1 stays unchanged — only the code-lookup and k change.)

- [ ] **Step 5: Store `load` — v3 header, `bits`/`row_bytes`, sized codebook, validate-before-alloc**

In `load` (lines ~132-171): accept version 3, read real `k`, derive `bits`/`row_bytes`, and size codes by `row_bytes` and codebook by `k*dim`. Replace the version/size/alloc block:

```cpp
unsigned int version; memcpy(&version, hdr+8, 4);
if (version != 1 && version != 2 && version != 3) { fclose(in); return s; }
/* hdr fields dim/docs/toks/m/k already parsed as today (dim, docs, toks, mm, kk) */
long long bits;
if (version == 3)
	bits = ANT_pq_codec::bits_for_k(kk);				/* any power of two in [2,256] */
else
	bits = (kk == ANT_pq_codec::K) ? 8 : -1;			/* v1/v2: k must be 256 */
if (dim != expected_dimension || docs != expected_documents || bits < 0 || mm < 1 || dim % mm != 0 || dim > 65536) { fclose(in); return s; }
long long header_size = 52;
long long rot_floats = 0;
if (version == 2 || version == 3)
	{
	unsigned char ohdr[8];
	if (fread(ohdr, 1, 8, in) != 8) { fclose(in); return s; }
	long long opq; memcpy(&opq, ohdr, 8);
	header_size = 60;
	if (opq == 1) rot_floats = dim*dim;
	else if (opq != 0) { fclose(in); return s; }
	}
long long row_bytes = (mm*bits + 7) / 8;
/* bound toks before the size multiply (existing #25 hardening): */
if (row_bytes > 0 && toks > (/*actual-ish upper bound*/ (long long)1 << 40)) { fclose(in); return s; }
long long cb_floats = kk * dim;						/* was 256*dim */
long long expected_size = header_size + docs*4 + toks*row_bytes + cb_floats*4 + rot_floats*4;
if (actual != expected_size) { fclose(in); return s; }
```

Then the allocations: `codes = new unsigned char[toks*row_bytes > 0 ? toks*row_bytes : 1];` (was `toks*mm`), read `toks*row_bytes` bytes; `codebook = new float[cb_floats]` and read `cb_floats` floats (was `256*dim`). Set `s->k = kk; s->row_bytes = row_bytes;` alongside the existing member assignments (`s->m = mm;` etc). Keep the exact forgiving-degrade structure (any `fread` short read / size mismatch → return the empty `s`). Preserve the existing #25 `toks` overflow guard shape — grep the current guard and adapt it to `row_bytes` (replace `mm` with `row_bytes` in the `(actual-52)/mm` reject).

- [ ] **Step 6: Writer `create` + `finish` — store k, pack rows, size codebook, version selection**

In `source/multivector_pq_store.cpp` writer `create` (~line 200+): accept `k` and stash it (`this->k = k;`). Add k validation: `if (ANT_pq_codec::bits_for_k(k) < 0) return 1;`.

In `finish` (~lines 234-296):
- `long long bits = ANT_pq_codec::bits_for_k(k); long long row_bytes = (m*bits + 7)/8;`
- codebook train/borrow: size `cb_floats = k * dimension` (was `256*dimension`); `train(buffer, dimension, m, k, total_tokens, owned_codebook)` (was `ANT_pq_codec::K`).
- encode + pack: allocate `codes = new unsigned char[total_tokens*row_bytes > 0 ? total_tokens*row_bytes : 1];`. Per token: `unsigned char tmp[256]; unsigned char *tbuf=(m<=256)?tmp:new unsigned char[m]; ANT_pq_codec::encode(buffer + t*dimension, dimension, m, k, codebook, tbuf); ANT_pq_codec::pack_codes(tbuf, m, bits, codes + t*row_bytes);` (free tbuf if heap).
- header write: `unsigned int version = (k == 256) ? (opq_flag ? 2u : 1u) : 3u;` and write the REAL `k` (remove the `long long k = 256;` hardcode — use the member). Write codes as `total_tokens*row_bytes` bytes and codebook as `cb_floats` floats. Keep the atomic `.tmp` + rename and the opq/rotation trailing-block logic unchanged.

- [ ] **Step 7: Engine member + fix all `.create` call sites (keeps the build green)**

Because the writer signature now requires `k`, every engine call site must pass it. Introduce the member here (its setter/config come in Task 2):

In `atire/atire_segment_index.h`, next to the T3 token-PQ members (`mvpq_m_current`, `mvpq_opq_current`, `mvpq_global_current`), add:
```cpp
	long long mvpq_k_current;			// token PQ codebook size (power of two in [2,256]); 256 = full byte
```

In `atire/atire_segment_index.cpp` ctor, where those members are initialized, add:
```cpp
mvpq_k_current = 256;
```

Grep every token `.mvpq` writer create — `grep -rn "ANT_multivector_pq_store_writer" atire/ | ...` then find each `.create(` — and insert `mvpq_k_current` as the new 4th arg (after `mvpq_m_current`, before the metric). There are three: `build_multivector_pq` and `rebuild_mvpq_global_codebook` in `atire/atire_segment_index_vector.cpp`, and the compaction site (~667) in `atire/atire_segment_index_compaction.cpp`. Each becomes:
```cpp
w.create(mvpq_name, rerank_dimension_current, mvpq_m_current, mvpq_k_current, ANT_pq_codec::METRIC_DOT, mvpq_opq_current)
```
(the exact receiver/var names differ per site — change only the arg list).

- [ ] **Step 8: Rebuild and run the test + the store regression**

Run:
```
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_variable_k && ./bin/test_mvpq_variable_k
make test_mvpq_store && ./bin/test_mvpq_store
make test_mvpq_opq && ./bin/test_mvpq_opq
make test_mvpq_global && ./bin/test_mvpq_global
```
Expected: `ALL test_mvpq_variable_k PASSED`; `test_mvpq_store PASSED`; `ALL test_mvpq_opq PASSED`; `ALL test_mvpq_global PASSED` (k=256 default ⇒ every existing token path byte-identical).

- [ ] **Step 9: Commit**

```bash
git add source/multivector_pq_store.h source/multivector_pq_store.cpp \
        atire/atire_segment_index.h atire/atire_segment_index.cpp \
        atire/atire_segment_index_vector.cpp atire/atire_segment_index_compaction.cpp \
        tests/test_mvpq_variable_k.cpp
git commit -m "feat(mvpq): .mvpq v3 variable code-width store + writer k param + call-site wiring (token epic 3/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git status --short | grep -q business-strategy && echo "strategy doc still untracked OK"
```

---

## Task 2: Engine `set_multivector_pq_k` (immutable-once) + `multivector_pq.config` v5 + build wiring

The `mvpq_k_current` member, its ctor init, and all three writer `.create` call sites were added in Task 1 (build-ordering). Task 2 adds only the public getter, the `set_multivector_pq_k` setter, and `multivector_pq.config` v5 persistence over that existing member.

**Files:**
- Modify: `atire/atire_segment_index.h` (getter + setter decl)
- Modify: `atire/atire_segment_index_vector.cpp` (setter def, config v5 load/save)
- Test: `tests/test_mvpq_variable_k_config.cpp` (create)

- [ ] **Step 1: Write the failing test** — `tests/test_mvpq_variable_k_config.cpp`

```cpp
/*
	TEST_MVPQ_VARIABLE_K_CONFIG.CPP -- token epic 3/4 Task 2: engine set_multivector_pq_k.
	set_multivector_pq_k validates power-of-two, is immutable-once, persists in
	multivector_pq.config v5 (survives reopen), and a config without the k field
	loads as k=256. A build under k=16 writes a v3 .mvpq.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define RD 8
#define MM 4

static char *mkdir_tmp(const char *tmpl)
{ char b[64]; strcpy(b, tmpl); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r, d); return r; }

static void fill(long long seed, float *v)
{ double n=0; for (int j=0;j<RD;j++){v[j]=(float)(((seed*7+j*3)%13)-6)/6.0f;n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<RD;j++)v[j]/=(float)n; }

static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{
	for (long long i = lo; i < hi; i++)
		{ float rows[3*RD]; for (int r=0;r<3;r++) fill(i*5+r, rows+r*RD);
		  char key[32]; snprintf(key,sizeof(key),"d-%lld",i);
		  CHECK(ix->add_document(key, "body words", NULL, rows, 3) >= 0); }
}

static void test_setter_validate_and_immutable(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkc1_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->multivector_pq_k() == 256);				/* default */
	CHECK(ix->set_multivector_pq_k(17) != 0);			/* not a power of two -> reject */
	CHECK(ix->set_multivector_pq_k(512) != 0);			/* out of [2,256] -> reject */
	CHECK(ix->set_multivector_pq_k(16) == 0);			/* ok */
	CHECK(ix->multivector_pq_k() == 16);
	CHECK(ix->set_multivector_pq_k(16) == 0);			/* idempotent */
	CHECK(ix->set_multivector_pq_k(32) != 0);			/* immutable once changed */
	CHECK(ix->multivector_pq_k() == 16);
	delete ix; delete [] dir;
	printf("test_setter_validate_and_immutable OK\n");
}

static void test_persist_and_build_v3(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkc2_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	add_docs(ix, 0, 12);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
	long long gen = ix->disk_segment_generation(0);
	delete ix;

	/* the .mvpq is v3 (k=16) */
	char mp[4096]; snprintf(mp, sizeof(mp), "%s/seg_%06lld.mvpq", dir, gen);
	FILE *fp = fopen(mp, "rb"); CHECK(fp != NULL);
	unsigned int version; fseek(fp, 8, SEEK_SET); CHECK(fread(&version,4,1,fp)==1); CHECK(version == 3u); fclose(fp);

	/* reopen restores k=16 from config v5 */
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	CHECK(re->multivector_pq_k() == 16);
	CHECK(re->build_token_index() == 0);
	float q[2*RD]; fill(1, q); fill(2, q+RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);
	delete re; delete [] dir;
	printf("test_persist_and_build_v3 OK\n");
}

int main(void)
{
	test_setter_validate_and_immutable();
	test_persist_and_build_v3();
	printf("ALL test_mvpq_variable_k_config PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_mvpq_variable_k_config && ./bin/test_mvpq_variable_k_config`
Expected: FAIL to compile — `multivector_pq_k()` / `set_multivector_pq_k(...)` do not exist.

- [ ] **Step 3: Header — getter + setter decl** (the `mvpq_k_current` member already exists from Task 1)

In `atire/atire_segment_index.h`, next to the `multivector_pq_global_codebook()` getter add:
```cpp
	long long multivector_pq_k(void) { return mvpq_k_current; }
```
Next to the `set_multivector_pq_global_codebook` decl add:
```cpp
	long set_multivector_pq_k(long long k);
```

- [ ] **Step 4: Setter — mirror `set_pq_k`**

In `atire/atire_segment_index_vector.cpp`, near `set_multivector_pq_global_codebook`, add (mirror `set_pq_k` at line ~724):

```cpp
/*
	ATIRE_SEGMENT_INDEX::SET_MULTIVECTOR_PQ_K()
	-------------------------------------------
	Token PQ code-width: k = power of two in [2,256] (256 = one byte per code,
	byte-identical to pre-#T2 .mvpq v1/v2). Immutable once changed from 256:
	the same value is idempotent success; any different value is rejected.
	Persists in multivector_pq.config v5. Composes with opq/global/tier; writer
	create() sites pass mvpq_k_current so backfilled/compacted segments encode at k.
*/
long ATIRE_segment_index::set_multivector_pq_k(long long k)
{
if (directory == NULL)
	return 1;						// must be open
if (!multivector_pq_configured())
	return 1;						// token PQ must be configured first
if (ANT_pq_codec::bits_for_k(k) < 0)
	return 1;						// not a power of two in [2,256]
if (mvpq_k_current == k)
	return 0;						// idempotent
if (mvpq_k_current != 256)
	return 1;						// immutable once changed from the default
mvpq_k_current = k;
if (save_multivector_pq_config() != 0)
	{ mvpq_k_current = 256; return 1; }
return 0;
}
```

- [ ] **Step 5: `multivector_pq.config` v4→v5 — mirror the T3 `global` addition**

In `load_multivector_pq_config` (the T3 code with `long long vals[6]` and version branches 1/2/3/4): extend to `vals[7]`, add a `version==5` branch, and default `k=256` for older versions. Concretely — grep the current function and apply:
- change `long long vals[6];` → `long long vals[7];`
- in each existing back-compat branch, append `vals[6] = 256;` (v1/v2/v3/v4 have no k):
  - v1: reads 3 → also set `vals[3]=MV_TIER_FLOAT; vals[4]=0; vals[5]=0; vals[6]=256;`
  - v2: reads 4 → `vals[4]=0; vals[5]=0; vals[6]=256;`
  - v3: reads 5 → `vals[5]=0; vals[6]=256;`
  - v4: reads 6 → `vals[6]=256;`
- add: `else if (version == 5u) ok = (fread(vals, 8, 7, in) == 7);`
- after the read, add to the destructuring line: `long long k = vals[6];`
- validate + assign: `if (ANT_pq_codec::bits_for_k(k) < 0) { /* leave token-PQ unconfigured, per #25 — mirror the existing m/posture/tier revalidation bail */ }` then `mvpq_k_current = k;` alongside the other `mvpq_*_current = ...` assignments. (Grep how the T3 code bails on an invalid persisted value — e.g. the `m` divisibility / tier-range check that returns leaving PQ unconfigured — and add the `bits_for_k(k) < 0` case to the SAME guard.)

In `save_multivector_pq_config`: bump `unsigned int version = 5u;` and change `long long vals[6] = { ... mvpq_global_current }` → `long long vals[7] = { mvpq_m_current, mvpq_posture_current, mvpq_rerank_quant_current, mvpq_resident_tier_current, mvpq_opq_current, mvpq_global_current, mvpq_k_current };` and `fwrite(vals, 8, 7, ...)`.

- [ ] **Step 6: Verify the `build_multivector_pq` writer already encodes at k**

The `.create` call sites were wired to `mvpq_k_current` in Task 1, so no code change here — just confirm by grep that `build_multivector_pq`'s `w.create(...)` passes `mvpq_k_current`. The Task-2 test (`test_persist_and_build_v3`) proves it end-to-end: with `set_multivector_pq_k(16)` the on-disk `.mvpq` is v3.

- [ ] **Step 7: Rebuild and run**

Run:
```
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_variable_k_config && ./bin/test_mvpq_variable_k_config
make test_mvpq_variable_k && ./bin/test_mvpq_variable_k
make test_mvpq_opq_config && ./bin/test_mvpq_opq_config
make test_v6_compaction && ./bin/test_v6_compaction
```
Expected: `ALL test_mvpq_variable_k_config PASSED`; Task-1 test still green; `ALL test_mvpq_opq_config PASSED` (config back-compat); `ALL TESTS PASSED` (compaction unaffected — k=256 default).

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index_vector.cpp tests/test_mvpq_variable_k_config.cpp
git commit -m "feat(mvpq): set_multivector_pq_k immutable-once + multivector_pq.config v5 (token epic 3/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git status --short | grep -q business-strategy && echo "strategy doc still untracked OK"
```

---

## Task 3: Composition — global codebook at k, OPQ, compaction/rebuild, tiers, forgiving-load

Make the T3 global codebook + `multivector_pq.codebook` sidecar train/persist/validate at `mvpq_k_current` (today hardcodes `ANT_pq_codec::K`), confirm OPQ + compaction + rebuild + NONE-tier all compose at k, and cover forgiving-load of a truncated v3/sidecar.

**Files:**
- Modify: `atire/atire_segment_index_vector.cpp` (`ensure_global_mvpq_codebook`, `rebuild_mvpq_global_codebook`, `save_mvpq_codebook`, `load_mvpq_codebook`)
- Test: `tests/test_mvpq_variable_k_compose.cpp` (create)

(All writer `.create` call sites — build/compaction/rebuild — already pass `mvpq_k_current` from Task 1; Task 3 only makes the *global codebook* train/persist/validate at k.)

- [ ] **Step 1: Write the failing test** — `tests/test_mvpq_variable_k_compose.cpp`

```cpp
/*
	TEST_MVPQ_VARIABLE_K_COMPOSE.CPP -- token epic 3/4 Task 3: k composes with
	global codebook, OPQ, compaction, rebuild, and forgiving-load.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define RD 8
#define MM 4

static char *mkdir_tmp(const char *tmpl)
{ char b[64]; strcpy(b, tmpl); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r, d); return r; }
static void fill(long long seed, float *v)
{ double n=0; for (int j=0;j<RD;j++){v[j]=(float)(((seed*7+j*3)%13)-6)/6.0f;n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<RD;j++)v[j]/=(float)n; }
static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{ for (long long i=lo;i<hi;i++){ float rows[3*RD]; for(int r=0;r<3;r++) fill(i*5+r, rows+r*RD);
  char key[32]; snprintf(key,sizeof(key),"d-%lld",i); CHECK(ix->add_document(key,"body",NULL,rows,3)>=0); } }
static void add_probe(ATIRE_segment_index *ix, const char *key, const float *p)
{ CHECK(ix->add_document(key,"probe",NULL,p,1) >= 0); }

/* k + global: two segments under global mode at k=16 embed byte-equal k*dim
   codebooks and a shared probe token encodes to identical packed rows. */
static void test_k_global_cross_segment(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkg_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	float probe[RD]; fill(999, probe);
	add_probe(ix, "pa", probe); add_docs(ix, 1, 12);
	CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_probe(ix, "pb", probe); add_docs(ix, 13, 24);
	CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_count() == 2);
	long long ga = ix->disk_segment_generation(0), gb = ix->disk_segment_generation(1);
	delete ix;

	char pa[4096], pb[4096];
	snprintf(pa,sizeof(pa),"%s/seg_%06lld.mvpq",dir,ga);
	snprintf(pb,sizeof(pb),"%s/seg_%06lld.mvpq",dir,gb);
	ANT_multivector_pq_store *sa = ANT_multivector_pq_store::load(pa, RD, 12, ANT_pq_codec::METRIC_DOT);
	ANT_multivector_pq_store *sb = ANT_multivector_pq_store::load(pb, RD, 12, ANT_pq_codec::METRIC_DOT);
	CHECK(sa && sb && sa->get_k() == 16 && sb->get_k() == 16);
	/* embedded codebooks (k*dim floats) byte-equal */
	size_t cb = (size_t)(16 * RD) * sizeof(float);
	CHECK(memcmp(sa->get_codebook(), sb->get_codebook(), cb) == 0);
	/* shared probe (token 0 in both) -> identical packed rows; row_bytes = (m*4+7)/8 = 2 */
	long long bits = ANT_pq_codec::bits_for_k(16);
	size_t rb = (size_t)((MM*bits + 7)/8);
	CHECK(memcmp(sa->token_codes(0), sb->token_codes(0), rb) == 0);
	delete sa; delete sb; delete [] dir;
	printf("test_k_global_cross_segment OK\n");
}

/* k + OPQ: build under k=16 + opq; reopen; MaxSim search sane; .mvpq is v3 with R block. */
static void test_k_opq(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvko_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_opq(1) == 0);
	add_docs(ix, 0, 16);
	CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	long long gen = ix->disk_segment_generation(0);
	delete ix;
	/* v3 + opq: header version 3, opq flag 1, size includes R block (D*D) */
	char mp[4096]; snprintf(mp,sizeof(mp),"%s/seg_%06lld.mvpq",dir,gen);
	FILE *fp = fopen(mp,"rb"); CHECK(fp);
	unsigned int version; fseek(fp,8,SEEK_SET); CHECK(fread(&version,4,1,fp)==1); CHECK(version==3u);
	long long opq; fseek(fp,52,SEEK_SET); CHECK(fread(&opq,8,1,fp)==1); CHECK(opq==1);
	fclose(fp);
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	CHECK(re->multivector_pq_k() == 16);
	CHECK(re->build_token_index() == 0);
	float q[2*RD]; fill(1,q); fill(2,q+RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);
	delete re; delete [] dir;
	printf("test_k_opq OK\n");
}

/* k + compaction no-retrain + rebuild at k. */
static void test_k_compaction_and_rebuild(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkc_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 12); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 12, 24); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);

	/* compact: merged .mvpq is v3 at k=16, reuses the global codebook (no retrain) */
	CHECK(ix->compact() == 0);
	CHECK(ix->disk_segment_count() == 1);
	long long gen = ix->disk_segment_generation(0);
	char mp[4096]; snprintf(mp,sizeof(mp),"%s/seg_%06lld.mvpq",dir,gen);
	FILE *fp = fopen(mp,"rb"); CHECK(fp);
	unsigned int version; fseek(fp,8,SEEK_SET); CHECK(fread(&version,4,1,fp)==1); CHECK(version==3u); fclose(fp);

	/* rebuild at k: retrains + re-encodes; still v3 at k=16, search sane */
	add_docs(ix, 24, 36); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->rebuild_mvpq_global_codebook() == 0);
	CHECK(ix->build_token_index() == 0);
	float q[2*RD]; fill(3,q); fill(7,q+RD);
	CHECK(ix->search_multivector(q, 2, 10) > 0);
	delete ix; delete [] dir;
	printf("test_k_compaction_and_rebuild OK\n");
}

/* forgiving load: truncate a v3 .mvpq -> degraded-empty (token_count()==0), no crash. */
static void test_k_forgiving_load(void)
{
	char path[] = "/tmp/ant_mvkf_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, RD, MM, 16, ANT_pq_codec::METRIC_DOT, 0) == 0);
	for (int d = 0; d < 10; d++) { float rows[2*RD]; fill(d,rows); fill(d+1,rows+RD); CHECK(w.append(rows,2)==0); }
	CHECK(w.finish() == 0);
	CHECK(truncate(path, 40) == 0);						/* chop mid-header/body */
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, RD, 10, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == 0);			/* degraded, not a crash */
	delete s; remove(path);
	printf("test_k_forgiving_load OK\n");
}

int main(void)
{
	test_k_global_cross_segment();
	test_k_opq();
	test_k_compaction_and_rebuild();
	test_k_forgiving_load();
	printf("ALL test_mvpq_variable_k_compose PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_mvpq_variable_k_compose && ./bin/test_mvpq_variable_k_compose`
Expected: FAIL — the `multivector_pq.codebook` sidecar + `ensure/rebuild` still train/size the global codebook at `ANT_pq_codec::K` (256), so `test_k_global_cross_segment` fails (embedded codebook is `256*dim`, not `16*dim`, or the sidecar size/validate mismatches → global falls back to per-segment and codes/codebooks differ). The writer itself already encodes at k (Task 1); this task fixes the *global* codebook path.

- [ ] **Step 3: Global codebook — train/size at `mvpq_k_current`**

In `atire/atire_segment_index_vector.cpp`:

`ensure_global_mvpq_codebook`: the T3 code trains `global_mvpq_codebook` at `ANT_pq_codec::K` and sizes it `m*256*sub`. Change:
- allocation size from `mvpq_m_current * ANT_pq_codec::K * sub` (= `256*rerank_dimension_current`) to `mvpq_k_current * rerank_dimension_current` (= `m*k*sub`);
- `ANT_pq_codec::train(rows, rerank_dimension_current, mvpq_m_current, ANT_pq_codec::K, ntok, global_mvpq_codebook)` → replace `ANT_pq_codec::K` with `mvpq_k_current`.

`rebuild_mvpq_global_codebook`: same two substitutions on its train + `new_codebook` sizing (`k*dim`).

(Grep both functions for `ANT_pq_codec::K` and `256` — replace with `mvpq_k_current` for the codebook train/size ONLY. The OPQ `train_rotation` (D²) is k-independent — leave it.)

- [ ] **Step 4: `multivector_pq.codebook` sidecar — carry + validate k**

`save_mvpq_codebook` (magic `ANTMVGCB`): it currently writes a `k` header field as `ANT_pq_codec::K` and a codebook block `256*dim`. Change the written `k` to `mvpq_k_current` and the codebook block size to `mvpq_k_current * rerank_dimension_current`. (Grep for where it writes the `k`/`dimension`/`m`/`opq` header and the `fwrite(codebook, ...)`.)

`load_mvpq_codebook`: validate the stored `k == mvpq_k_current` (in ADDITION to the existing `dimension`/`m`/`opq` checks) and `ANT_pq_codec::bits_for_k(k) >= 0`; size the read codebook block `mvpq_k_current * rerank_dimension_current` (was `256*dim`). Keep the exact validate-before-allocate + exact-remaining-size discipline; any mismatch ⇒ forgiving-degrade to NULL (untrained). (Grep the current `256`/`ANT_pq_codec::K` in this function and substitute.)

- [ ] **Step 5: Verify all `.create` sites pass k** (wired in Task 1 — verification only)

Confirm by grep (`ANT_multivector_pq_store_writer` + `.create(` across `atire/atire_segment_index_compaction.cpp` and `atire/atire_segment_index_vector.cpp`) that EVERY token `.mvpq` writer `create` passes `mvpq_k_current` as the 4th arg — the compaction site (~667), the `rebuild_mvpq_global_codebook` Pass-2 writer, and `build_multivector_pq`. All three were wired in Task 1; this is a no-op check. The `test_k_compaction_and_rebuild` case proves the merged/rebuilt `.mvpq` is v3 at k.

- [ ] **Step 6: Rebuild and run the compose test**

Run:
```
rm -f obj/*.o lib/libantelope_engine.a
make test_mvpq_variable_k_compose && ./bin/test_mvpq_variable_k_compose
```
Expected: `ALL test_mvpq_variable_k_compose PASSED`.

- [ ] **Step 7: Full token/engine regression**

Run each and confirm PASS:
```
for t in test_mvpq_variable_k test_mvpq_variable_k_config test_mvpq_variable_k_compose \
         test_mvpq_global test_mvpq_external_codebook test_mvpq_opq test_mvpq_opq_config \
         test_mvpq_store test_pq_token_resident_tier test_v6_search_multivector \
         test_v6_compaction test_segment_index; do
  make $t >/dev/null 2>&1 && ./bin/$t >/tmp/rk_$t.log 2>&1 && echo "PASS $t" || echo "FAIL $t"; done
```
Expected: every line `PASS`.

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_mvpq_variable_k_compose.cpp
git commit -m "feat(mvpq): global codebook trained/persisted at variable k + compose tests (token epic 3/4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git status --short | grep -q business-strategy && echo "strategy doc still untracked OK"
```

---

## Notes for the implementer

- **`m <= 256` scratch guard:** `m` divides `dimension` and `dimension <= 65536`, so `m` can exceed 256 in theory. The `unsigned char rb[256]` stack scratches above are only valid when `m <= 256`; each snippet already falls back to `new unsigned char[m]` when `m > 256`. Keep that guard — do not assume `m <= 256`.
- **Byte-identity is the contract:** at `k == 256`, `bits == 8`, `row_bytes == m`, `version` is v1/v2, codebook is `256*dim` — every byte of a `.mvpq` and every read/score path must be identical to pre-feature. The Task-1 `test_k256_byte_identical` and the untouched `test_mvpq_store`/`test_mvpq_opq`/`test_v6_*` suites are the guard; if any regresses, a `k` substitution leaked into the k=256 path.
- **Do NOT touch `source/pq_codec.{h,cpp}`** — the codec is complete. If you think you need a codec change, you've misread; re-check `bits_for_k`/`pack_codes`/`unpack_codes`.
- **Grep before every edit:** the T1/T3 code has evolved these functions; the exact line numbers here are approximate. Confirm the current body, then apply the substitution described.
```

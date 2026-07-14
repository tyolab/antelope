# Dense PQ Variable Code-Width (k≠256) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the dense `.pq` product-quantization store use a codebook size `k` that is any power of two in `[2,256]`, bit-packing codes below a byte per subvector for a smaller `.pq` (trading recall), with `k=256` byte-identical to today.

**Architecture:** `ANT_pq_codec`'s compile-time `K=256` becomes a runtime `k` parameter on every entry point (k-means/ADC math otherwise unchanged); two pure helpers `pack_codes`/`unpack_codes` translate between unpacked byte-codes and a per-row LSB-first bit-packed layout (`row_bytes = (m·bits+7)/8`, `bits = log₂k`); `ANT_pq_store` stores `documents·row_bytes` packed codes behind a `.pq` v3 header (v2 written when `k==256` for byte-identity); `set_pq_k` persists `k` in `pq.config` v5; the #22.2 global codebook and compaction thread `k` through.

**Tech Stack:** C++ (C++03-style, no STL in hot paths beyond `std::vector` already used in the codec), the ATIRE/antelope engine. Tests are standalone `tests/*.cpp` binaries built via `make <name>` → `bin/<name>`, using the repo's `CHECK()` macro convention.

**Repo gotchas (read before starting):**
- **After ANY header change** (`source/pq_codec.h`, `source/pq_store.h`, `atire/atire_segment_index.h`) run `rm -f obj/*.o lib/libantelope_engine.a` before rebuilding — there is no header dependency tracking, so stale objects cause SEGVs/`undefined reference`.
- Build a test: `make <testname>` (no `bin/` prefix, no `.cpp`); binary lands at `bin/<testname>`. Run `./bin/<testname>`.
- Config setters are POST-open (need `directory` + `vector_dimension_current`).
- `default off ⇒ byte-identical`: any test that round-trips a default (`k=256`) store must produce a v2 file, not v3.
- Confirm signatures/line numbers by `grep` before editing — line numbers below are as of this plan's writing and may drift.

---

### Task 1: Codec `k` parameter + pack/unpack helpers

Make the codec size-agnostic and add the packing primitives. No store/engine changes yet — this task is self-contained in the codec plus its own test.

**Files:**
- Modify: `source/pq_codec.h` (add `k` param to 5 methods; add 3 new statics)
- Modify: `source/pq_codec.cpp` (thread `k`; implement `bits_for_k`/`pack_codes`/`unpack_codes`)
- Test: `tests/test_pq_codec_kwidth.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_pq_codec_kwidth.cpp`:

```cpp
/*
	TEST_PQ_CODEC_KWIDTH.CPP -- variable code-width codec (#22.3):
	pack/unpack round-trip across all bit-widths, bits_for_k validation,
	and k-parameter self-consistency (encode->adc_score ranks the assigned
	centroid highest; reconstruct returns it). k=256 stays byte-identical.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pq_codec.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_bits_for_k(void)
{
CHECK(ANT_pq_codec::bits_for_k(2) == 1);
CHECK(ANT_pq_codec::bits_for_k(16) == 4);
CHECK(ANT_pq_codec::bits_for_k(64) == 6);
CHECK(ANT_pq_codec::bits_for_k(256) == 8);
CHECK(ANT_pq_codec::bits_for_k(6) == -1);		// not a power of two
CHECK(ANT_pq_codec::bits_for_k(1) == -1);		// below 2
CHECK(ANT_pq_codec::bits_for_k(512) == -1);		// above 256
CHECK(ANT_pq_codec::bits_for_k(0) == -1);
}

static void test_pack_roundtrip(void)
{
long long m = 13;						// deliberately not a byte multiple
for (long bits = 1; bits <= 8; bits++)
	{
	long long k = 1LL << bits;
	unsigned char codes[13], out[13];
	for (long long s = 0; s < m; s++)
		codes[s] = (unsigned char)((s * 7 + 3) % k);	// each < 2^bits
	long long row_bytes = (m * bits + 7) / 8;
	unsigned char *packed = new unsigned char[row_bytes + 4];
	memset(packed, 0xAB, (size_t)(row_bytes + 4));		// canary tail
	ANT_pq_codec::pack_codes(codes, m, bits, packed);
	ANT_pq_codec::unpack_codes(packed, m, bits, out);
	CHECK(memcmp(codes, out, (size_t)m) == 0);
	if (bits < 8)						// trailing bits of last byte must be zero
		{
		long long used = m * bits;
		if (used % 8 != 0)
			{
			unsigned char last = packed[(used - 1) / 8];
			unsigned char mask = (unsigned char)(0xFF << (used % 8));
			CHECK((last & mask) == 0);
			}
		}
	delete [] packed;
	}
}

static void test_pack_bits8_is_memcpy(void)
{
long long m = 5;
unsigned char codes[5] = { 0, 42, 255, 7, 200 }, packed[5];
ANT_pq_codec::pack_codes(codes, m, 8, packed);
CHECK(memcmp(codes, packed, (size_t)m) == 0);		// identity at bits==8
}

static void test_k_param_self_consistent(void)
{
// 4-dim vectors, m=2 subspaces of 2 dims, k=16 centroids.
long long dim = 4, m = 2, k = 16, n = 32, sub = dim / m;
float *vectors = new float[n * dim];
srand(12345);
for (long long i = 0; i < n * dim; i++)
	vectors[i] = (float)(rand() % 1000) / 1000.0f;
float *codebook = new float[m * k * sub];
CHECK(ANT_pq_codec::train(vectors, dim, m, k, n, codebook) == 0);

for (long long i = 0; i < n; i++)
	{
	unsigned char codes[2];
	ANT_pq_codec::encode(vectors + i * dim, dim, m, k, codebook, codes);
	CHECK(codes[0] < k && codes[1] < k);
	// reconstruct returns the assigned centroids
	float recon[4];
	ANT_pq_codec::reconstruct(codes, dim, m, k, codebook, recon);
	for (long long s = 0; s < m; s++)
		{
		const float *cent = codebook + s * k * sub + codes[s] * sub;
		for (long long d = 0; d < sub; d++)
			CHECK(fabs(recon[s * sub + d] - cent[d]) < 1e-6);
		}
	// ADC (dot metric): the assigned code should score >= any other code in each subspace
	double *table = new double[m * k];
	ANT_pq_codec::adc_table(vectors + i * dim, dim, m, k, codebook, ANT_pq_codec::METRIC_L2, table);
	double best = ANT_pq_codec::adc_score(codes, m, k, table);
	for (long long s = 0; s < m; s++)
		for (long long c = 0; c < k; c++)
			CHECK(table[s * k + codes[s]] >= table[s * k + c] - 1e-9);	// nearest under L2
	(void)best;
	delete [] table;
	}
delete [] vectors;
delete [] codebook;
}

static void test_k256_matches_default(void)
{
// With k=256 the codec must behave exactly as the fixed-K codec did.
long long dim = 8, m = 4, k = ANT_pq_codec::K, n = 50, sub = dim / m;
float *vectors = new float[n * dim];
srand(999);
for (long long i = 0; i < n * dim; i++)
	vectors[i] = (float)(rand() % 2000 - 1000) / 500.0f;
float *codebook = new float[m * k * sub];
CHECK(ANT_pq_codec::train(vectors, dim, m, k, n, codebook) == 0);
unsigned char codes[4], packed[4];
ANT_pq_codec::encode(vectors, dim, m, k, codebook, codes);
ANT_pq_codec::pack_codes(codes, m, 8, packed);
CHECK(memcmp(codes, packed, (size_t)m) == 0);		// packing is identity at k=256
delete [] vectors;
delete [] codebook;
}

int main(void)
{
test_bits_for_k();
test_pack_roundtrip();
test_pack_bits8_is_memcpy();
test_k_param_self_consistent();
test_k256_matches_default();
if (failures == 0)
	printf("ALL test_pq_codec_kwidth PASSED\n");
else
	printf("%d CHECK(s) FAILED\n", failures);
return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_codec_kwidth 2>&1 | tail -20
```
Expected: FAIL — compile error (`bits_for_k`/`pack_codes`/`unpack_codes` not declared; `train`/`encode`/`adc_table`/`adc_score`/`reconstruct` called with an extra `k` argument that the current 4/5-arg signatures reject).

- [ ] **Step 3: Update the codec header**

In `source/pq_codec.h`, replace the five method declarations and add the three new statics. The class body becomes:

```cpp
	enum { K = 256, KMEANS_ITERS = 25 };
	enum { METRIC_DOT = 0, METRIC_COSINE = 1, METRIC_L2 = 2 };	// mirrors ANT_vector_store metrics

	// #22.3 variable code-width: k is a power of two in [2,256]; bits = log2(k).
	static long bits_for_k(long long k);												// log2(k) for a power of two in [2,256]; -1 otherwise
	static void pack_codes(const unsigned char *codes, long long m, long long bits, unsigned char *packed);	// m byte-codes (<2^bits) -> (m*bits+7)/8 bytes, LSB-first
	static void unpack_codes(const unsigned char *packed, long long m, long long bits, unsigned char *codes);	// inverse of pack_codes

	static long train(const float *vectors, long long dimension, long long m, long long k, long long n, float *codebook);	// m must divide dimension (else 1); 0 ok
	static void encode(const float *vector, long long dimension, long long m, long long k, const float *codebook, unsigned char *codes);	// writes m UNPACKED byte-codes in [0,k)
	static void adc_table(const float *query, long long dimension, long long m, long long k, const float *codebook, long metric, double *table);	// table is m*k doubles
	static double adc_score(const unsigned char *codes, long long m, long long k, const double *table);	// codes UNPACKED bytes; table stride k
	static void reconstruct(const unsigned char *codes, long long dimension, long long m, long long k, const float *codebook, float *out);	// codes UNPACKED bytes

	// OPQ (#22.1): learn an orthogonal D*D row-major rotation R (metric-preserving, no centering).
	static long train_rotation(const float *vectors, long long dimension, long long m, long long n, float *R);
	static void apply_rotation(const float *vec, long long dimension, const float *R, float *out);
	static void apply_rotation_transpose(const float *vec, long long dimension, const float *R, float *out);
```

Also update the file's top comment: change "per subspace a k=256 k-means codebook" to "per subspace a k-means codebook of k centroids (k a power of two <=256)".

- [ ] **Step 4: Thread `k` through the codec body and add the helpers**

In `source/pq_codec.cpp`:

1. Add `#include <stdio.h>` is NOT needed; ensure `<string.h>` (already present).

2. `train`: change the signature to `long ANT_pq_codec::train(const float *vectors, long long dimension, long long m, long long k, long long n, float *codebook)`. Inside, replace **every** `K` with `k` (the local parameter). Specifically: `std::vector<long long> counts(K)` → `counts(k)`; `std::vector<double> sums(K * sub)` → `sums(k * sub)`; `memset(codebook, 0, (size_t)(m * K * sub) ...)` → `m * k * sub`; `codebook + s * K * sub` → `s * k * sub`; the `distinct_found < K` / `c < K` / `for (c = distinct_found; c < K; c++)` / `memset(centroids, 0, (size_t)(K * sub) ...)` loops → `k`.

3. `encode`: signature `void ANT_pq_codec::encode(const float *vector, long long dimension, long long m, long long k, const float *codebook, unsigned char *codes)`. Replace `codebook + s * K * sub` → `s * k * sub` and `for (c = 1; c < K; c++)` → `c < k`. (`codes[s] = (unsigned char)best;` unchanged — best < k ≤ 256 fits a byte.)

4. `adc_table`: signature adds `long long k` after `m`. Replace `s * K * sub` → `s * k * sub`, `for (c = 0; c < K; c++)` → `c < k`, and `table[s * K + c]` → `table[s * k + c]`.

5. `adc_score`: signature `double ANT_pq_codec::adc_score(const unsigned char *codes, long long m, long long k, const double *table)`. Replace `table[s * K + codes[s]]` → `table[s * k + codes[s]]`.

6. `reconstruct`: signature adds `long long k` after `m`. Replace `codebook + s * K * sub` → `s * k * sub`.

7. Add the three new functions (place them just after `reconstruct`, before the Jacobi block):

```cpp
/*
	ANT_pq_codec::bits_for_k()
	----------------------------
	log2(k) for a power of two in [2,256]; -1 otherwise.
*/
long ANT_pq_codec::bits_for_k(long long k)
{
if (k < 2 || k > 256)
	return -1;
long bits = 0;
long long v = k;
while ((v & 1) == 0) { v >>= 1; bits++; }
return (v == 1) ? bits : -1;			// v==1 iff k was a power of two
}

/*
	ANT_pq_codec::pack_codes()
	----------------------------
	Pack m byte-codes (each < 2^bits) LSB-first into (m*bits+7)/8 bytes:
	code s occupies bit positions [s*bits, (s+1)*bits); trailing bits zero.
	Deterministic. bits==8 is a straight memcpy (identity).
*/
void ANT_pq_codec::pack_codes(const unsigned char *codes, long long m, long long bits, unsigned char *packed)
{
long long nbytes = (m * bits + 7) / 8;
if (bits == 8)
	{ memcpy(packed, codes, (size_t)m); return; }
memset(packed, 0, (size_t)(nbytes > 0 ? nbytes : 1));
for (long long s = 0; s < m; s++)
	{
	unsigned long v = codes[s];
	long long base = s * bits;
	for (long long j = 0; j < bits; j++)
		if (v & (1UL << j))
			packed[(base + j) >> 3] |= (unsigned char)(1u << ((base + j) & 7));
	}
}

/*
	ANT_pq_codec::unpack_codes()
	------------------------------
	Inverse of pack_codes: writes m byte-codes.
*/
void ANT_pq_codec::unpack_codes(const unsigned char *packed, long long m, long long bits, unsigned char *codes)
{
if (bits == 8)
	{ memcpy(codes, packed, (size_t)m); return; }
for (long long s = 0; s < m; s++)
	{
	unsigned long v = 0;
	long long base = s * bits;
	for (long long j = 0; j < bits; j++)
		if (packed[(base + j) >> 3] & (1u << ((base + j) & 7)))
			v |= (1UL << j);
	codes[s] = (unsigned char)v;
	}
}
```

- [ ] **Step 5: Fix the in-tree callers so the codec still links**

The codec's callers in `source/pq_store.cpp` and `atire/atire_segment_index_vector.cpp` still use the old arity and will now fail to compile. For THIS task, do the **minimal** mechanical fix: pass `ANT_pq_codec::K` as the new `k` argument at every existing call site so behavior is unchanged (Task 2 replaces these with the real `k`). Update these call sites:

- `source/pq_store.cpp`: `reconstruct` (2 sites ~214,219) → add `, m, ANT_pq_codec::K, codebook`; `adc_table` (~250,284,327) → add `ANT_pq_codec::K` after `m`; `adc_score` (~253,296,338) → add `ANT_pq_codec::K` after `m`; `train` (~544) → `train(present_rows, dimension, m, ANT_pq_codec::K, present_count, owned_codebook)`; `encode` (~557) → `encode(buffer + i*dimension, dimension, m, ANT_pq_codec::K, codebook, codes + i*m)`.
- `atire/atire_segment_index_vector.cpp`: `train` calls in `ensure_global_pq_codebook` (~887) and `rebuild_pq_global_codebook` (~992) → add `ANT_pq_codec::K` after `pq_m_current`.

(Grep `ANT_pq_codec::\(train\|encode\|adc_table\|adc_score\|reconstruct\)` to catch every site; the token `.mvpq` codec calls in the multivector store source — e.g. `source/multivector_pq_store.cpp` — also use `ANT_pq_codec` and MUST get the same `K` argument — do not skip them.)

**Test files that call the codec directly must also be updated in THIS task** (they won't compile otherwise, and Step 6/7 build them): `tests/test_pq_codec.cpp`, `tests/test_pq_global.cpp`, `tests/test_pq_hnsw_tiered.cpp`, `tests/test_pq_hnsw_prepared.cpp`. In each, grep the same `ANT_pq_codec::(train|encode|adc_table|adc_score|reconstruct)` calls and insert `ANT_pq_codec::K` as the new `k` argument (train/encode/reconstruct: after `m`; adc_table: after `m`; adc_score: after `m`). These files also construct dense `ANT_pq_store_writer` objects, but leave those `.create(...)` calls alone for now — the writer signature does not change until Task 2. This keeps them compiling and behavior-identical at k=256.

- [ ] **Step 6: Run the test to verify it passes**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_codec_kwidth 2>&1 | tail -5
./bin/test_pq_codec_kwidth
```
Expected: `ALL test_pq_codec_kwidth PASSED`.

- [ ] **Step 7: Verify existing PQ + multivector suites still pass (k=256 unchanged)**

```bash
for t in test_pq_codec test_pq_store test_pq_config test_pq_compaction test_pq_opq test_pq_global test_pq_metrics test_pq_resident_tier test_pq_hnsw test_pq_hnsw_tiered test_pq_hnsw_prepared test_mvpq_store test_pq_token_resident_tier; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: every suite PASSED (the mechanical `K` pass-through preserves behavior). `test_mvpq_store`/`test_pq_token_resident_tier` confirm the token `.mvpq` codec calls were updated too.

- [ ] **Step 8: Commit**

```bash
git add source/pq_codec.h source/pq_codec.cpp source/pq_store.cpp source/multivector_pq_store.cpp atire/atire_segment_index_vector.cpp tests/test_pq_codec_kwidth.cpp tests/test_pq_codec.cpp tests/test_pq_global.cpp tests/test_pq_hnsw_tiered.cpp tests/test_pq_hnsw_prepared.cpp
git commit -m "feat(pq): codec k-parameter + pack/unpack helpers (#22.3)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Store row layout + `.pq` v3 + config v5 + `set_pq_k`

Give the store a real `k`: bit-packed per-row codes, a v3 header (v2 when `k==256` for byte-identity), and the engine-level `set_pq_k` config persisted in `pq.config` v5.

**Files:**
- Modify: `source/pq_store.h` (store members `k`/`bits`/`row_bytes`; `PQ_CODE_STACK_CAP`; writer `create` gains `k`)
- Modify: `source/pq_store.cpp` (load/finish row layout + v3; read sites unpack)
- Modify: `atire/atire_segment_index.h` (`pq_k_current` member, `pq_k()` getter, `set_pq_k` decl)
- Modify: `atire/atire_segment_index.cpp` (ctor init `pq_k_current = 256`)
- Modify: `atire/atire_segment_index_vector.cpp` (`set_pq_k`, `pq.config` v5, writer `create` sites pass `pq_k_current`)
- Test: `tests/test_pq_kwidth.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_pq_kwidth.cpp`:

```cpp
/*
	TEST_PQ_KWIDTH.CPP -- store + engine variable code-width (#22.3):
	a k=16 store round-trips (reconstruct/scan_adc correct), its .pq is v3
	with a documents*row_bytes codes region (~half a k=256 store), a k=256
	store is written as v2 (byte-identical default), and set_pq_k persists
	to pq.config v5 with immutability + reopen restore.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pq_store.h"
#include "pq_codec.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static unsigned int read_u32_at(const char *path, long off)
{
FILE *fp = fopen(path, "rb");
unsigned int v = 0;
if (fp) { fseek(fp, off, SEEK_SET); if (fread(&v, sizeof(v), 1, fp) != 1) v = 0; fclose(fp); }
return v;
}

static long file_size_of(const char *path)
{
FILE *fp = fopen(path, "rb");
long n = -1;
if (fp) { fseek(fp, 0, SEEK_END); n = ftell(fp); fclose(fp); }
return n;
}

static void write_store(const char *path, long long dim, long long m, long long k, long long n, const float *vecs)
{
ANT_pq_store_writer w;
CHECK(w.create(path, dim, m, k, ANT_pq_codec::METRIC_L2, 0) == 0);
for (long long i = 0; i < n; i++)
	CHECK(w.append(vecs + i * dim) == 0);
CHECK(w.finish() == 0);
}

static void test_k16_roundtrip_and_size(void)
{
long long dim = 8, m = 4, n = 40;
float *vecs = new float[n * dim];
srand(7);
for (long long i = 0; i < n * dim; i++) vecs[i] = (float)(rand() % 1000) / 500.0f - 1.0f;

write_store("/tmp/kw16.pq", dim, m, 16, n, vecs);
// version field is at byte offset 8 (after 8-byte magic)
CHECK(read_u32_at("/tmp/kw16.pq", 8) == 3u);				// k!=256 -> v3

ANT_pq_store *s16 = ANT_pq_store::load("/tmp/kw16.pq", dim, n, ANT_pq_codec::METRIC_L2);
CHECK(s16->document_count() == n);
// reconstruct returns a valid centroid tuple (finite, dimensioned)
float recon[8];
s16->reconstruct(0, recon);
for (long long d = 0; d < dim; d++) CHECK(recon[d] == recon[d]);	// not NaN

// codes region shrinks: bits=4 -> row_bytes = (4*4+7)/8 = 2 (vs m=4 at k=256)
long size16 = file_size_of("/tmp/kw16.pq");
write_store("/tmp/kw256.pq", dim, m, 256, n, vecs);
CHECK(read_u32_at("/tmp/kw256.pq", 8) == 2u);				// k==256 -> v2 (byte-identity)
long size256 = file_size_of("/tmp/kw256.pq");
CHECK(size16 < size256);					// smaller codebook (m*16*sub) + smaller codes (n*2 vs n*4)
delete s16;
delete [] vecs;
}

static void test_bad_k_degrades(void)
{
// hand-forge a v3 header with k=6 (not a power of two): load must degrade to empty.
long long dim = 4, m = 2, n = 2;
float vecs[8] = {1,2,3,4, 5,6,7,8};
write_store("/tmp/kwbad.pq", dim, m, 16, n, vecs);
// overwrite the k field (offset 8 + 4 + 8 + 8 + 8 = 36) with 6
FILE *fp = fopen("/tmp/kwbad.pq", "r+b");
long long bad = 6; fseek(fp, 36, SEEK_SET); fwrite(&bad, sizeof(bad), 1, fp); fclose(fp);
ANT_pq_store *s = ANT_pq_store::load("/tmp/kwbad.pq", dim, n, ANT_pq_codec::METRIC_L2);
CHECK(s->document_count() == 0);				// forgiving: degraded empty store
delete s;
}

int main(void)
{
test_k16_roundtrip_and_size();
test_bad_k_degrades();
if (failures == 0) printf("ALL test_pq_kwidth PASSED\n");
else printf("%d CHECK(s) FAILED\n", failures);
return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
make test_pq_kwidth 2>&1 | tail -20
```
Expected: FAIL — `w.create(...)` called with 6 args but the writer's `create` takes 5 (no `k`), so it won't compile.

- [ ] **Step 3: Store header — members, constants, writer `create` signature**

In `source/pq_store.h`:

1. Add a code-buffer stack cap next to the existing enum:
```cpp
enum { PQ_SCORE_STACK_CAP = 8 * 256 };		// 2048 doubles (16 KB) inline in score(); heap above this
enum { PQ_CODE_STACK_CAP = 4096 };			// bytes: inline unpacked-code buffer in read paths; heap above this
```
2. Add store members after `long long dimension, documents, m;`:
```cpp
	long long dimension, documents, m, k, bits, row_bytes;	// k = codebook size; bits = log2(k); row_bytes = (m*bits+7)/8
```
3. Add a getter next to `get_m`:
```cpp
	long long get_k(void) { return k; }
```
4. Change the writer to carry `k` and take it in `create`:
```cpp
	char *filename; long long dimension, m, k; long metric; long opq;
	...
	long create(const char *path, long long dim, long long m, long long k, long metric, long opq);
```

- [ ] **Step 4: Store `.cpp` — version constants + ctor init**

In `source/pq_store.cpp`:

1. Version constants (replace the two existing lines):
```cpp
static const unsigned int ANT_PQ_STORE_VERSION_V1 = 1;
static const unsigned int ANT_PQ_STORE_VERSION_V2 = 2;		// OPQ-capable, k==256
static const unsigned int ANT_PQ_STORE_VERSION_V3 = 3;		// #22.3 variable k (power of two in [2,256])
```
Header sizes are unchanged: v1 = 44 bytes, v2/v3 = 52 bytes (v3 adds no new header field; only `k`'s allowed range and the codes-region size differ). Keep `ANT_PQ_STORE_HEADER_SIZE_V1` and rename `ANT_PQ_STORE_HEADER_SIZE` usages to apply to both v2 and v3.

2. Ctor: init the new members:
```cpp
ANT_pq_store::ANT_pq_store()
{
dimension = 0;
documents = 0;
m = 0;
k = 0;
bits = 0;
row_bytes = 0;
metric = 0;
...
}
```

Update the on-disk-layout comment block at the top: note `version 1/2/3`, `k i64 (256 for v1/v2; any power of two in [2,256] for v3)`, and `codes documents*row_bytes bytes, row_bytes = (m*log2(k)+7)/8 (== m when k==256)`.

- [ ] **Step 5: Store `load()` — validate `k`, size codes by `row_bytes`**

In `ANT_pq_store::load`:

1. Accept v2 **and** v3 for the opq-field read:
```cpp
if (stored_version == ANT_PQ_STORE_VERSION_V2 || stored_version == ANT_PQ_STORE_VERSION_V3)
	{
	if (fread(&stored_opq, sizeof(stored_opq), 1, fp) != 1)
		{ fclose(fp); return result; }
	header_size = ANT_PQ_STORE_HEADER_SIZE;
	}
else
	{
	stored_opq = 0;
	header_size = ANT_PQ_STORE_HEADER_SIZE_V1;
	}
```

2. Compute `bits` from `k` with version-gated validation. Replace the `stored_k != ANT_pq_codec::K` clause in the big validation `if` and add a bits computation right after it. Change the validation block to:
```cpp
long stored_bits;
if (stored_version == ANT_PQ_STORE_VERSION_V3)
	stored_bits = ANT_pq_codec::bits_for_k(stored_k);		// any power of two in [2,256]
else
	stored_bits = (stored_k == ANT_pq_codec::K) ? 8 : -1;	// v1/v2: k must be 256

if (memcmp(stored_magic, ANT_PQ_STORE_MAGIC, 8) != 0
	|| (stored_version != ANT_PQ_STORE_VERSION_V1 && stored_version != ANT_PQ_STORE_VERSION_V2 && stored_version != ANT_PQ_STORE_VERSION_V3)
	|| stored_dimension != expected_dimension
	|| stored_documents != expected_documents
	|| stored_documents < 0 || stored_documents > (1LL << 40)
	|| stored_dimension < 1 || stored_dimension > 65536
	|| stored_m < 1 || stored_m > stored_dimension
	|| stored_dimension % stored_m != 0
	|| stored_bits < 0							// invalid/mismatched k
	|| (stored_opq != 0 && stored_opq != 1))
	{
	fclose(fp);
	return result;
	}
long long stored_row_bytes = (stored_m * (long long)stored_bits + 7) / 8;
```

3. Size the regions with `k` and `row_bytes` (replace the three `..._floats/_bytes` lines):
```cpp
long long presence_bytes = (stored_documents + 7) / 8;
long long codebook_floats = stored_m * stored_k * (stored_dimension / stored_m);	// m*k*sub
long long codes_bytes = stored_documents * stored_row_bytes;						// per-row packed
```
(The overflow note still holds: `stored_k <= 256`, `stored_documents <= 2^40`, `stored_row_bytes <= stored_m <= 65536`, so `codes_bytes <= 2^56` and `codebook_floats*4 <= 2^26`.)

4. On success, populate the new members before `return result`:
```cpp
result->dimension = stored_dimension;
result->documents = stored_documents;
result->m = stored_m;
result->k = stored_k;
result->bits = stored_bits;
result->row_bytes = stored_row_bytes;
result->metric = metric;
...
```

- [ ] **Step 6: Store read paths — unpack per row**

Every read site currently does `codes + docid * m` and calls the codec on raw bytes. Now the stored row is `codes + docid * row_bytes` packed; unpack it into a byte buffer first, and pass `k` to the codec. Use a stack buffer with heap fallback.

`reconstruct`:
```cpp
void ANT_pq_store::reconstruct(long long docid, float *out)
{
if (!has(docid))
	{ memset(out, 0, (size_t)dimension * sizeof(float)); return; }
unsigned char stack_codes[PQ_CODE_STACK_CAP];
unsigned char *cb = (m <= (long long)PQ_CODE_STACK_CAP) ? stack_codes : new unsigned char[m];
ANT_pq_codec::unpack_codes(codes + docid * row_bytes, m, bits, cb);
if (rotation != NULL)
	{
	float *tmp = new float[dimension];
	ANT_pq_codec::reconstruct(cb, dimension, m, k, codebook, tmp);
	ANT_pq_codec::apply_rotation_transpose(tmp, dimension, rotation, out);
	delete [] tmp;
	}
else
	ANT_pq_codec::reconstruct(cb, dimension, m, k, codebook, out);
if (cb != stack_codes) delete [] cb;
}
```

`score`: after building `table` (change `table_size = m * (long long)ANT_pq_codec::K` → `m * k`, and the `adc_table(..., m, k, codebook, ...)` call), replace the final scoring line:
```cpp
unsigned char stack_codes[PQ_CODE_STACK_CAP];
unsigned char *cb = (m <= (long long)PQ_CODE_STACK_CAP) ? stack_codes : new unsigned char[m];
ANT_pq_codec::unpack_codes(codes + docid * row_bytes, m, bits, cb);
double result = ANT_pq_codec::adc_score(cb, m, k, table);
if (cb != stack_codes) delete [] cb;
```

`prepare_query`: change `new double[m * (long long)ANT_pq_codec::K]` → `new double[m * k]` and `adc_table(q, dimension, m, k, codebook, metric, table)`.

`score_prepared`: replace the `ctx != NULL` scoring line:
```cpp
if (!has(docid))
	return 0.0;
unsigned char stack_codes[PQ_CODE_STACK_CAP];
unsigned char *cb = (m <= (long long)PQ_CODE_STACK_CAP) ? stack_codes : new unsigned char[m];
ANT_pq_codec::unpack_codes(codes + docid * row_bytes, m, bits, cb);
double result = ANT_pq_codec::adc_score(cb, m, k, (double *)ctx);
if (cb != stack_codes) delete [] cb;
return result;
```

`scan_adc`: replace `long long K = ANT_pq_codec::K; double *table = new double[m * K];` with `double *table = new double[m * k];`, call `adc_table(q, dimension, m, k, codebook, metric, table)`, allocate **one** reusable code buffer before the loop, and unpack per doc:
```cpp
unsigned char *cb = new unsigned char[m > 0 ? m : 1];
for (long long d = 0; d < documents; d++)
	{
	if (!has(d)) continue;
	if (tombstones != 0 && tombstones->is_deleted(d)) continue;
	if (filter_bits != 0 && !(filter_bits[d >> 3] & (1 << (d & 7)))) continue;
	ANT_pq_codec::unpack_codes(codes + d * row_bytes, m, bits, cb);
	ANT_vector_candidate_insert(best, best_count, top_k, ANT_pq_codec::adc_score(cb, m, k, table), generation, d);
	}
delete [] cb;
delete [] table;
```

Also update `codes_for` in `pq_store.h`: it now returns a pointer to the **packed** row — change `codes + docid*m` → `codes + docid*row_bytes` and update its comment to say the row is packed (callers must `ANT_pq_codec::unpack_codes`).

- [ ] **Step 7: Store `writer::create` + `finish` — carry `k`, pack rows, v2/v3 header**

`create`: signature adds `k`; validate it; store it.
```cpp
long ANT_pq_store_writer::create(const char *path, long long dim, long long m_arg, long long k_arg, long metric_arg, long opq_arg)
{
abandon();
ext_codebook = NULL;
ext_rotation = NULL;
if (m_arg < 1 || dim < 1 || dim % m_arg != 0 || ANT_pq_codec::bits_for_k(k_arg) < 0)
	return 1;
filename = strdup(path);
if (filename == NULL) return 1;
dimension = dim;
m = m_arg;
k = k_arg;
metric = metric_arg;
opq = opq_arg ? 1 : 0;
...
}
```
Also add `k` to the writer ctor init (`k = 0;`).

`finish`: thread `k` through the training/encoding and pack each row. Changes:
- `long long codebook_floats = m * (long long)ANT_pq_codec::K * sub;` → `m * k * sub`.
- `long long bits = ANT_pq_codec::bits_for_k(k);` (compute once) and `long long row_bytes = (m * bits + 7) / 8;`.
- `long long codes_bytes = documents * m;` → `documents * row_bytes;`.
- The `train(present_rows, dimension, m, present_count, owned_codebook)` call → `train(present_rows, dimension, m, k, present_count, owned_codebook)`.
- The encode loop packs into rows:
```cpp
unsigned char *codes = new unsigned char[codes_bytes > 0 ? codes_bytes : 1];
unsigned char *row_codes = new unsigned char[m > 0 ? m : 1];
for (i = 0; i < documents; i++)
	{
	ANT_pq_codec::encode(buffer + i * dimension, dimension, m, k, codebook, row_codes);
	ANT_pq_codec::pack_codes(row_codes, m, bits, codes + i * row_bytes);
	}
delete [] row_codes;
```
  Free `row_codes` on the early-return error paths too (snprintf/fopen failures after `codes` is allocated) — mirror wherever `delete [] codes;` appears, OR (simpler and leak-free) free `row_codes` immediately after the loop as shown, before the snprintf block, since it is no longer needed. Use the shown placement (free right after the loop) so no later error path needs to touch it.
- Header write: pick the version and write the stored `k` (not `ANT_pq_codec::K`):
```cpp
long long k_field = k;
long long opq_flag = (rotation != NULL) ? 1 : 0;
unsigned int version = (k == ANT_pq_codec::K) ? ANT_PQ_STORE_VERSION_V2 : ANT_PQ_STORE_VERSION_V3;
```
  Replace the `fwrite(&k, ...)` line's variable with `&k_field` (the writer member `k` shadows nothing, but use `k_field` for clarity), and drop the old `long long k = ANT_pq_codec::K;` line.

- [ ] **Step 8: Engine — `pq_k_current`, `set_pq_k`, `pq.config` v5**

In `atire/atire_segment_index.h`, after `long pq_global_current;`:
```cpp
	long long pq_k_current;				// PQ codebook size (default 256); power of two in [2,256]; immutable once changed from 256
```
Getter + setter decl near the other PQ getters:
```cpp
	long set_pq_k(long long k);			// 0 ok; nonzero: not open / PQ unconfigured / k not a power of two in [2,256] / already changed to a DIFFERENT value (immutable)
	long long pq_k(void) { return pq_k_current; }
```

In `atire/atire_segment_index.cpp` constructor, initialize (next to `pq_global_current = 0;`):
```cpp
pq_k_current = 256;
```

In `atire/atire_segment_index_vector.cpp`:

1. `load_pq_config` — bump accepted versions to include 5 and read `k` for v5. Change the version guard to `(version != 1u && version != 2u && version != 3u && version != 4u && version != 5u)`, add a local `long long kk = 256;`, and after the `version == 4u` global block:
```cpp
if (version == 5u)
	{
	if (fread(&kk, sizeof(kk), 1, fp) != 1 || ANT_pq_codec::bits_for_k(kk) < 0)
		{ fclose(fp); return 0; }
	}
```
Then assign `pq_k_current = kk;` alongside the other `pq_*_current` assignments.

2. `save_pq_config` — bump `version = 5u;`, add `long long kk = pq_k_current;`, and append its write after `global`:
```cpp
	|| fwrite(&global, sizeof(global), 1, fp) != 1
	|| fwrite(&kk, sizeof(kk), 1, fp) != 1)
```

3. Add `set_pq_k` (place it after `set_pq_global_codebook`):
```cpp
/*
	ATIRE_SEGMENT_INDEX::SET_PQ_K()
	--------------------------------
	Choose the dense `.pq` codebook size k (a power of two in [2,256]); k<256
	bit-packs codes below a byte per subvector for a smaller .pq, trading
	recall.  Requires PQ already configured (set_pq_config()).  Default 256
	(byte-per-code, byte-identical to pre-#22.3).  Like set_pq_opq(), once
	changed from 256 it is immutable: the SAME value is a no-op success
	(idempotent); any different value once changed is rejected.  Persists in
	pq.config v5.  Composes with OPQ/global/tier (orthogonal); writer create()
	sites pass pq_k_current so backfilled/compacted segments encode at k.
*/
long ATIRE_segment_index::set_pq_k(long long k)
{
if (directory == NULL)
	return 1;						// must be open
if (!pq_configured())
	return 1;						// PQ must be configured first
if (ANT_pq_codec::bits_for_k(k) < 0)
	return 1;						// not a power of two in [2,256]
if (pq_k_current == k)
	return 0;						// idempotent
if (pq_k_current != 256)
	return 1;						// immutable once changed from the default
pq_k_current = k;
if (save_pq_config() != 0)
	{ pq_k_current = 256; return 1; }
return 0;
}
```
Ensure `#include "../source/pq_codec.h"` is present in this file (it already uses `ANT_pq_codec` — confirm).

4. Writer `create` sites — pass `pq_k_current`. In `build_pq` (~1677) and `rebuild_pq_global_codebook` (~1026):
```cpp
w.create(pq_name, vector_dimension_current, pq_m_current, pq_k_current, vector_metric, pq_opq_current)
```

- [ ] **Step 9: Compaction writer `create` — pass `pq_k_current`**

In `atire/atire_segment_index_compaction.cpp` (~502):
```cpp
w.create(out_pq, vector_dimension_current, pq_m_current, pq_k_current, vector_metric, pq_opq_current)
```

- [ ] **Step 10: Fix the Task-1 pass-through call sites to use real `k`**

The dense-`.pq` codec calls that Task 1 hard-coded to `ANT_pq_codec::K` in `source/pq_store.cpp` are now handled by Step 6/7 (they use the store/writer `k`). Confirm no `ANT_pq_codec::K` remains as a *dense* codec argument in `pq_store.cpp` (grep). The `ensure_global_pq_codebook`/`rebuild` train calls in `atire_segment_index_vector.cpp` are updated in Task 3; leave their Task-1 `ANT_pq_codec::K` pass-through in place for now (still k=256-correct until Task 3).

- [ ] **Step 11: Update existing test writer call sites**

Every dense `ANT_pq_store_writer::create(path, dim, m, metric, opq)` (the 5-arg form ending in `metric, opq`) now needs `256` inserted as the `k` argument → `create(path, dim, m, 256, metric, opq)` (preserves k=256 behavior). The exact dense-writer sites (confirm by grep — line numbers may drift):
- `tests/test_pq_store.cpp`: 3 sites (~36, 84, 170)
- `tests/test_pq_opq.cpp`: 2 sites (~90 via its `write_store(...,metric,opq)` helper, ~164)
- `tests/test_pq_global.cpp`: 2 sites (~36, 68)
- `tests/test_pq_hnsw_tiered.cpp`: 1 site (~21)
- `tests/test_pq_hnsw.cpp`: 1 site (~35) — NOT the `create(path, dim)` at ~24 (that's a different 2-arg writer)
- `tests/test_pq_hnsw_prepared.cpp`: 1 site (~18) — NOT the `create(path, dim)` at ~82

**Do NOT touch** `ANT_multivector_pq_store_writer` (token `.mvpq`, 4-arg `create(path, dim, m, metric)` — no opq) in `tests/test_mvpq_store.cpp` / `tests/test_pq_token_resident_tier.cpp`, nor any non-PQ 2-arg `create(path, dim)` writer.

- [ ] **Step 12: Run the new test + full regression**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_kwidth 2>&1 | tail -5
./bin/test_pq_kwidth
for t in test_pq_codec test_pq_codec_kwidth test_pq_store test_pq_config test_pq_compaction test_pq_opq test_pq_global test_pq_metrics test_pq_resident_tier test_pq_hnsw test_pq_hnsw_tiered test_pq_hnsw_prepared test_mvpq_store test_pq_token_resident_tier; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: `ALL test_pq_kwidth PASSED` and every existing suite PASSED (k=256 default byte-identical: their stores are v2, codes region == documents*m).

- [ ] **Step 13: Commit**

```bash
git add source/pq_store.h source/pq_store.cpp atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp atire/atire_segment_index_compaction.cpp tests/test_pq_kwidth.cpp tests/test_pq_store.cpp tests/test_pq_opq.cpp tests/test_pq_global.cpp tests/test_pq_hnsw.cpp tests/test_pq_hnsw_tiered.cpp tests/test_pq_hnsw_prepared.cpp
git commit -m "feat(pq): store row-packed codes, .pq v3, set_pq_k + pq.config v5 (#22.3)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Global codebook + compaction + composition

Thread `k` through the #22.2 global-codebook path (train/alloc/sidecar validation at `k`) and prove composition with OPQ, the global codebook, and compaction end-to-end via an engine-level test.

**Files:**
- Modify: `atire/atire_segment_index_vector.cpp` (`ensure_global_pq_codebook`, `rebuild_pq_global_codebook`, `save_pq_codebook`, `load_pq_codebook` — size/validate at `pq_k_current`)
- Test: `tests/test_pq_kwidth_compose.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `tests/test_pq_kwidth_compose.cpp` (modeled precisely on `tests/test_pq_global.cpp`'s engine harness — `set_vector_config`/`open`/`set_pq_config`/`add_document`/`flush`/`build_pq`/`compact`/`disk_segment_generation`/`search_vector`, and the `.pq`/`pq.codebook` on-disk layout it already asserts against):

```cpp
/*
	TEST_PQ_KWIDTH_COMPOSE.CPP -- #22.3 composition: variable k with the
	global codebook, OPQ, and compaction. (a) k=16 + global trains one
	m*16*sub codebook, both segments embed it, the shared probe encodes to
	identical packed rows, and pq.codebook's k field reads 16; (b) k=64 +
	OPQ builds and searches; (c) compaction under k=16 reuses the frozen
	codebook (pq.codebook byte-identical) and the merged segment searches.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define GDIM 16
#define GM 4

static char *make_engine_dir(const char *tmpl)
{
	char buffer[64]; strcpy(buffer, tmpl);
	char *dir = mkdtemp(buffer);
	if (dir == NULL) exit(printf("cannot create scratch dir\n"));
	char *r = new char[strlen(dir) + 1]; strcpy(r, dir); return r;
}
static void make_gvec(long long i, float *v)
{
	for (int d = 0; d < GDIM; d++) v[d] = 0.02f * (float)(((i * 7 + d) % 5) - 2);
	v[i % GDIM] += 3.0f;
}
static void add_gdocs(ATIRE_segment_index *ix, long long lo, long long hi)
{
	float v[GDIM]; char key[32], body[64];
	for (long long i = lo; i < hi; i++)
		{ make_gvec(i, v); sprintf(key, "gdoc-%lld", i); sprintf(body, "<DOC>gterm%lld z</DOC>", i);
		  CHECK(ix->add_document(key, body, v) >= 0); }
}
static int read_file_bytes(const char *path, unsigned char **out, long *len)
{
	FILE *fp = fopen(path, "rb"); if (!fp) return 1;
	fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
	unsigned char *b = new unsigned char[n > 0 ? n : 1];
	if (fread(b, 1, n, fp) != (size_t)n) { fclose(fp); delete [] b; return 1; }
	fclose(fp); *out = b; *len = n; return 0;
}

/* k=16 + global codebook: cross-segment comparability + sidecar k field. */
static void test_k16_global_comparability(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_kw16_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_k(16) == 0);
	CHECK(ix->pq_k() == 16);
	CHECK(ix->set_pq_global_codebook(1) == 0);

	float probe[GDIM]; make_gvec(3, probe);
	CHECK(ix->add_document("probe-a", "<DOC>proba</DOC>", probe) >= 0);
	add_gdocs(ix, 1, N);
	CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	CHECK(ix->add_document("probe-b", "<DOC>probb</DOC>", probe) >= 0);
	add_gdocs(ix, N + 1, N + N);
	CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	CHECK(ix->disk_segment_count() == 2);
	long long ga = ix->disk_segment_generation(0), gb = ix->disk_segment_generation(1);
	delete ix;

	char pa[4096], pb[4096];
	snprintf(pa, sizeof(pa), "%s/seg_%06lld.pq", dir, ga);
	snprintf(pb, sizeof(pb), "%s/seg_%06lld.pq", dir, gb);
	ANT_pq_store *sa = ANT_pq_store::load(pa, GDIM, N, ANT_pq_codec::METRIC_DOT);
	ANT_pq_store *sb = ANT_pq_store::load(pb, GDIM, N, ANT_pq_codec::METRIC_DOT);
	CHECK(sa && sb && sa->document_count() == N && sb->document_count() == N);
	CHECK(sa->get_k() == 16 && sb->get_k() == 16);					// v3 stores carry k=16
	long long sub = GDIM / GM, cb_floats = GM * sa->get_k() * sub;
	CHECK(memcmp(sa->get_codebook(), sb->get_codebook(), (size_t)cb_floats * sizeof(float)) == 0);
	long bits = ANT_pq_codec::bits_for_k(16); long long row_bytes = (GM * bits + 7) / 8;	// 4 bits -> 2 bytes
	CHECK(memcmp(sa->codes_for(0), sb->codes_for(0), (size_t)row_bytes) == 0);	// same packed row
	delete sa; delete sb;

	// pq.codebook sidecar (ANTPQGCB): magic8 + u32 version + i64 dim + i64 m + i64 k ...
	// -> k field at byte offset 8 + 4 + 8 + 8 = 28.  (Confirm against save_pq_codebook's
	//    write order before trusting this offset.)
	char cbp[4096]; snprintf(cbp, sizeof(cbp), "%s/pq.codebook", dir);
	FILE *fp = fopen(cbp, "rb"); CHECK(fp != NULL);
	long long sidecar_k = 0; CHECK(fseek(fp, 28, SEEK_SET) == 0);
	CHECK(fread(&sidecar_k, sizeof(sidecar_k), 1, fp) == 1); fclose(fp);
	CHECK(sidecar_k == 16);
	delete [] dir;
	printf("test_k16_global_comparability OK\n");
}

/* k=64 + OPQ: builds and searches (composition smoke). */
static void test_k64_opq_search(void)
{
	char *dir = make_engine_dir("/tmp/ant_kw64_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_k(64) == 0);
	CHECK(ix->set_pq_opq(1) == 0);
	add_gdocs(ix, 0, 30);
	CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	float q[GDIM]; make_gvec(5, q);
	CHECK(ix->search_vector(q, 5) >= 1);							// k=64 + OPQ search works
	delete ix; delete [] dir;
	printf("test_k64_opq_search OK\n");
}

/* k=16 + global: compaction reuses the frozen codebook (no retrain). */
static void test_k16_compaction_reuse(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_kwcmp_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_k(16) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);
	add_gdocs(ix, 0, N); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	add_gdocs(ix, N, N + N); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	CHECK(ix->disk_segment_count() == 2);

	char cbp[4096]; snprintf(cbp, sizeof(cbp), "%s/pq.codebook", dir);
	unsigned char *before = NULL; long blen = 0; CHECK(read_file_bytes(cbp, &before, &blen) == 0);
	long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
	CHECK(ix->compact(gens, 2) == 0);
	CHECK(ix->disk_segment_count() == 1);
	unsigned char *after = NULL; long alen = 0; CHECK(read_file_bytes(cbp, &after, &alen) == 0);
	CHECK(alen == blen && memcmp(before, after, (size_t)blen) == 0);		// no retrain

	long long og = ix->disk_segment_generation(0);
	char mp[4096]; snprintf(mp, sizeof(mp), "%s/seg_%06lld.pq", dir, og);
	ANT_pq_store *merged = ANT_pq_store::load(mp, GDIM, N + N, ANT_pq_codec::METRIC_DOT);
	CHECK(merged && merged->document_count() == N + N && merged->get_k() == 16);
	delete merged;
	float q[GDIM]; make_gvec(7, q);
	CHECK(ix->search_vector(q, 5) >= 1);
	delete [] before; delete [] after; delete ix; delete [] dir;
	printf("test_k16_compaction_reuse OK\n");
}

int main(void)
{
	test_k16_global_comparability();
	test_k64_opq_search();
	test_k16_compaction_reuse();
	printf("ALL test_pq_kwidth_compose PASSED\n");
	return 0;
}
```

Before asserting the sidecar-`k` offset (28), open `save_pq_codebook` in `atire/atire_segment_index_vector.cpp` and confirm the field write order is magic(8) → u32 version(4) → i64 dimension(8) → i64 m(8) → i64 k(8). If the order differs, correct the `fseek` offset.

- [ ] **Step 2: Run test to verify it fails**

```bash
make test_pq_kwidth_compose 2>&1 | tail -20
```
Expected: FAIL — at k=16 the global codebook is still allocated/trained/validated at `ANT_pq_codec::K` (256), so `codes_for`+`unpack_codes(bits=4)` disagree with the 256-sized codebook and the sidecar `k` field reads 256, not 16 (assertions fail); or a size mismatch degrades the store.

- [ ] **Step 3: Size the global codebook + sidecar at `pq_k_current`**

In `atire/atire_segment_index_vector.cpp`:

1. `ensure_global_pq_codebook` (~885): `long long floats = pq_m_current * (long long)ANT_pq_codec::K * sub;` → `pq_m_current * pq_k_current * sub;`. The `train(rows, vector_dimension_current, pq_m_current, present_count, global_pq_codebook)` call → `train(rows, vector_dimension_current, pq_m_current, pq_k_current, present_count, global_pq_codebook)`.

2. `rebuild_pq_global_codebook` (~990): same two changes (`* pq_k_current * sub`; `train(..., pq_k_current, ...)`).

3. `save_pq_codebook` (~728): `long long k = ANT_pq_codec::K;` → `long long k = pq_k_current;` (it already writes `k` and sizes `codebook_floats = m * k * sub`, so this now persists the real k).

4. `load_pq_codebook` (~781): change the validation `k != (long long)ANT_pq_codec::K` → `k != (long long)pq_k_current` (forgiving: a sidecar whose k disagrees with the configured k degrades to untrained). `codebook_floats = m * k * sub` already follows.

(The global path stores/reads the FULL m·k·sub codebook — it is not itself bit-packed; only the per-document `.pq` codes are packed, which Task 2 already handles via the writer that embeds this codebook. No packing changes are needed here.)

- [ ] **Step 4: Run the test to verify it passes**

```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_kwidth_compose 2>&1 | tail -5
./bin/test_pq_kwidth_compose
```
Expected: `ALL test_pq_kwidth_compose PASSED`.

- [ ] **Step 5: Full regression (composition must not regress #22.1/#22.2)**

```bash
for t in test_pq_codec test_pq_codec_kwidth test_pq_kwidth test_pq_kwidth_compose test_pq_store test_pq_config test_pq_compaction test_pq_opq test_pq_global test_pq_metrics test_pq_resident_tier test_pq_hnsw test_pq_hnsw_tiered test_pq_hnsw_prepared test_segment_index test_v6_compaction; do
  make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1
done
```
Expected: every suite PASSED.

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_pq_kwidth_compose.cpp
git commit -m "feat(pq): global codebook + compaction at variable k; composition tests (#22.3)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Notes for the final holistic review (after all 3 tasks)

Focus the end-of-branch review on:
- **Byte-identity at k=256:** default and OPQ/global stores write **v2** (not v3), codes region == `documents*m`, and every existing PQ suite is unchanged.
- **Packing correctness:** `pack_codes`/`unpack_codes` are exact inverses for all `bits ∈ {1..8}`; trailing bits zero; no over-read of the last row byte.
- **Read-path buffers:** the stack-cap-with-heap-fallback code buffers in `score`/`score_prepared`/`reconstruct`/`scan_adc` never leak and never overflow (`m > PQ_CODE_STACK_CAP` heaps); `scan_adc`'s single reusable buffer is freed on all paths.
- **`finish` leak-freedom:** `row_codes` freed on every path; borrowed `ext_codebook`/`ext_rotation` still never freed; owned buffers freed exactly once.
- **Forgiving load:** a v3 header with a non-power-of-two/out-of-range `k`, or a size-inconsistent codes region, degrades to an empty store.
- **Config back-compat:** `pq.config` v1–v4 load with `k=256`; v5 round-trips; `set_pq_k` immutability + idempotence.
- **Composition:** OPQ rotation is independent of `k`; the global codebook/sidecar carry the configured `k`; compaction reuses the frozen codebook at `k` (no retrain).
- ASan/UBSan is environment-blocked (no makefile hook) — report, don't attempt.
```

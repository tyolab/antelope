# Vector V2 Signature Prefilter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in SimHash binary-signature prefilter in front of the exact dense vector scan, to speed up `search_vector`/`search_hybrid` on large collections.

**Architecture:** A new `ANT_signature` (index-wide deterministic random-hyperplane projection + sign + Hamming) and `ANT_signature_store`/`_writer` (per-segment `seg_G.vsig` sidecar, mirroring `ANT_vector_store`). `ATIRE_segment_index` gains a `signature.config`, lifecycle hooks (flush/compact/open/backfill), a cached per-segment signature store, and `*_approx` search methods that Hamming-shortlist then exact-rerank. Cosine/dot only; L2 and unsupported segments fall back to the untouched V1 exact path.

**Tech Stack:** C++, GNU make (auto-discovers `source/*.cpp` and `tests/*.cpp`), existing `ANT_mersenne_twister`, `ANT_vector_store`, `ANT_index_tombstones`, the Node-API addon.

**Task dependency order (important):** the cached `segment.signatures` field + its load (Task 6) must land **before** the `*_approx` searches (Tasks 7-8) that read it. Do the tasks in the numbered order.

---

## Conventions used throughout

- **Build after any header change:** `rm -f obj/*.o && make all && make tests` (no header dependency tracking). Pure `.cpp` edits can skip the purge.
- **Run a C++ test binary:** `make tests && ./bin/<name>` (each `tests/*.cpp` auto-builds to `bin/<name>`).
- **Signature byte width** is always `(bits + 7) / 8`; `bits` is a multiple of 8 (default 256).
- **Reference patterns:** `source/vector_store.{h,cpp}` (sidecar load/writer/scan shape) and `source/mersenne_twister.h` (`init_genrand64(seed)`, `genrand64_real2()` → double in [0,1)).
- **Signature config magic** is the 64-bit little-endian value `0x3130474953544E41ULL` (the bytes "ANTSIG01").

---

## Task 1: `ANT_signature` — projection, sign, Hamming

**Files:** Create `source/signature.h`, `source/signature.cpp`, `tests/test_signature.cpp`

- [ ] **Step 1: Write the failing test** (`tests/test_signature.cpp`)

```cpp
/*
	TEST_SIGNATURE.CPP -- unit tests for ANT_signature (SimHash + Hamming).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../source/signature.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void test_deterministic(void)
{
long long dim = 32, bits = 128;
ANT_signature a(dim, bits, 12345ULL), b(dim, bits, 12345ULL);
float v[32];
for (long long i = 0; i < dim; i++) v[i] = (float)sin((double)i);
unsigned char sa[16], sb[16];
a.sign(v, sa);
b.sign(v, sb);
CHECK(memcmp(sa, sb, 16) == 0);
CHECK(a.signature_bytes() == 16);
printf("test_deterministic OK\n");
}

static void test_hamming_tracks_angle(void)
{
long long dim = 64, bits = 256;
ANT_signature s(dim, bits, 99ULL);
float base[64], near_v[64], far_v[64];
for (long long i = 0; i < dim; i++) { base[i] = (float)((i % 7) - 3); near_v[i] = base[i]; far_v[i] = -base[i]; }
near_v[0] += 0.01f;
unsigned char sb[32], sn[32], sf[32];
s.sign(base, sb); s.sign(near_v, sn); s.sign(far_v, sf);
long long h_near = ANT_signature::hamming(sb, sn, 32);
long long h_far  = ANT_signature::hamming(sb, sf, 32);
CHECK(h_near < h_far);
CHECK(h_near <= 8);
printf("test_hamming_tracks_angle OK\n");
}

int main(void)
{
test_deterministic();
test_hamming_tracks_angle();
printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `make tests 2>&1 | tail -3` → FAIL (`source/signature.h` missing).

- [ ] **Step 3: `source/signature.h`**

```cpp
/*
	SIGNATURE.H -- index-wide SimHash: a fixed random-hyperplane projection
	(materialized in RAM from a seed) mapping a dense vector to a packed
	bit-signature whose Hamming distance tracks angular distance.  See
	docs/superpowers/specs/2026-07-06-vector-v2-signature-prefilter-design.md.
*/
#ifndef SIGNATURE_H_
#define SIGNATURE_H_

class ANT_signature
{
private:
	long long dimension;
	long long bits;					// signature width; multiple of 8
	float *projection;				// bits * dimension, row-major (one hyperplane per row)

public:
	ANT_signature(long long dimension, long long bits, unsigned long long seed);
	~ANT_signature();

	long long signature_bytes(void) { return (bits + 7) / 8; }
	void sign(const float *vector, unsigned char *out_signature);					// writes signature_bytes() bytes
	static long long hamming(const unsigned char *a, const unsigned char *b, long long bytes);
} ;

#endif /* SIGNATURE_H_ */
```

- [ ] **Step 4: `source/signature.cpp`**

```cpp
/*
	SIGNATURE.CPP
*/
#include <string.h>
#include "signature.h"
#include "mersenne_twister.h"

ANT_signature::ANT_signature(long long dimension, long long bits, unsigned long long seed)
{
long long i, total;
ANT_mersenne_twister twister;

this->dimension = dimension;
this->bits = bits;
total = bits * dimension;
projection = new float[total > 0 ? total : 1];
twister.init_genrand64(seed);
for (i = 0; i < total; i++)
	projection[i] = (float)(twister.genrand64_real2() * 2.0 - 1.0);		// uniform in [-1, 1)
}

ANT_signature::~ANT_signature()
{
delete [] projection;
}

void ANT_signature::sign(const float *vector, unsigned char *out_signature)
{
long long bit, d;
const float *hyperplane;
double dot;

memset(out_signature, 0, (size_t)signature_bytes());
for (bit = 0; bit < bits; bit++)
	{
	hyperplane = projection + bit * dimension;
	dot = 0.0;
	for (d = 0; d < dimension; d++)
		dot += (double)hyperplane[d] * (double)vector[d];
	if (dot >= 0.0)
		out_signature[bit / 8] |= (unsigned char)(1 << (bit % 8));
	}
}

long long ANT_signature::hamming(const unsigned char *a, const unsigned char *b, long long bytes)
{
long long i, count = 0;
for (i = 0; i < bytes; i++)
	count += (long long)__builtin_popcount((unsigned int)(a[i] ^ b[i]));
return count;
}
```

- [ ] **Step 5: Run to verify it passes** — `make tests 2>&1 | tail -3 && ./bin/test_signature` → `PASSED`.

- [ ] **Step 6: Commit**

```bash
git add source/signature.h source/signature.cpp tests/test_signature.cpp
git commit -m "feat(vector-v2): ANT_signature SimHash projection + Hamming"
```

---

## Task 2: `ANT_signature_store` + writer — the `.vsig` sidecar

**Files:** Create `source/signature_store.h`, `source/signature_store.cpp`, `tests/test_signature_store.cpp`

- [ ] **Step 1: Write the failing test** (`tests/test_signature_store.cpp`)

```cpp
/*
	TEST_SIGNATURE_STORE.CPP -- unit tests for the per-segment .vsig sidecar.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/signature_store.h"
#include "../source/index_tombstones.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char *scratch(void)
{
static char path[64];
strcpy(path, "/tmp/ant_vsig_XXXXXX");
int fd = mkstemp(path); if (fd >= 0) close(fd); unlink(path);
return path;
}

static void test_roundtrip(void)
{
char name[64]; strcpy(name, scratch());
long long bits = 16;
unsigned char s0[2] = {0xAB, 0xCD}, s2[2] = {0x12, 0x34};
ANT_signature_store_writer w;
CHECK(w.create(name, bits) == 0);
CHECK(w.append(s0) == 0);			// docid 0 present
CHECK(w.append(NULL) == 0);			// docid 1 absent
CHECK(w.append(s2) == 0);			// docid 2 present
CHECK(w.finish() == 0);
ANT_signature_store *store = ANT_signature_store::load(name, bits, 3);
CHECK(store->document_count() == 3);
CHECK(store->has(0) && !store->has(1) && store->has(2));
CHECK(memcmp(store->get(0), s0, 2) == 0);
CHECK(memcmp(store->get(2), s2, 2) == 0);
delete store;
unlink(name);
printf("test_roundtrip OK\n");
}

static void test_degrade(void)
{
char name[64]; strcpy(name, scratch());
unsigned char s0[2] = {0x00, 0xFF};
ANT_signature_store_writer w;
CHECK(w.create(name, 16) == 0);
CHECK(w.append(s0) == 0);
CHECK(w.finish() == 0);
ANT_signature_store *wrong_bits = ANT_signature_store::load(name, 32, 1);	// bits mismatch -> empty
CHECK(wrong_bits->document_count() == 0);
delete wrong_bits;
ANT_signature_store *missing = ANT_signature_store::load("/tmp/does_not_exist_vsig", 16, 1);
CHECK(missing->document_count() == 0);
delete missing;
unlink(name);
printf("test_degrade OK\n");
}

static void test_shortlist(void)
{
char name[64]; strcpy(name, scratch());
long long bits = 8;
unsigned char a = 0x00, b = 0x0F, c = 0xFF;			// vs query 0x00: hamming 0, 4, 8
ANT_signature_store_writer w;
CHECK(w.create(name, bits) == 0);
CHECK(w.append(&a) == 0);
CHECK(w.append(&b) == 0);
CHECK(w.append(&c) == 0);
CHECK(w.finish() == 0);
ANT_signature_store *store = ANT_signature_store::load(name, bits, 3);
ANT_index_tombstones stones(3);
unsigned char query = 0x00;
long long docids[2], count = 0;
store->shortlist(&query, &stones, /*pool_size=*/2, docids, &count);
CHECK(count == 2);
long long has0 = (docids[0] == 0 || docids[1] == 0);
long long has1 = (docids[0] == 1 || docids[1] == 1);
CHECK(has0 && has1);				// nearest two are docids 0 and 1; docid 2 (hamming 8) excluded
delete store;
unlink(name);
printf("test_shortlist OK\n");
}

int main(void)
{
test_roundtrip();
test_degrade();
test_shortlist();
printf("PASSED\n");
return 0;
}
```

Confirm `ANT_index_tombstones`'s constructor takes a document count (`ANT_index_tombstones(long long)`); if the actual ctor differs, adjust the test's `stones` construction to match (check `source/index_tombstones.h`).

- [ ] **Step 2: Run to verify it fails** — `make tests 2>&1 | tail -3` → FAIL (`source/signature_store.h` missing).

- [ ] **Step 3: `source/signature_store.h`**

```cpp
/*
	SIGNATURE_STORE.H -- per-segment binary-signature sidecar (seg_G.vsig),
	mirroring ANT_vector_store's shape and forgiving-load posture.  On-disk:
	  uint64 magic ("ANTSIG01") | uint32 version | int64 bits | int64 document_count
	  byte[] presence bitmap ((count+7)/8) | byte[] signatures (count*bits/8)
	Any load failure (magic/version/bits/count/size) degrades to an empty store.
*/
#ifndef SIGNATURE_STORE_H_
#define SIGNATURE_STORE_H_

class ANT_index_tombstones;

class ANT_signature_store
{
private:
	long long bits;
	long long documents;
	unsigned char *presence;			// NULL when degraded/empty
	unsigned char *signatures;			// NULL when degraded/empty

	ANT_signature_store();

public:
	~ANT_signature_store();

	static ANT_signature_store *load(const char *filename, long long expected_bits, long long expected_documents);

	long long document_count(void) { return documents; }
	long long signature_bytes(void) { return (bits + 7) / 8; }
	long has(long long docid) { return presence != NULL && (presence[docid / 8] & (1 << (docid % 8))) != 0; }
	const unsigned char *get(long long docid) { return signatures + docid * signature_bytes(); }

	/*
		Fill out_docids[0..*out_count) with up to pool_size smallest-Hamming
		present, non-tombstoned docids for query_signature (order unspecified).
	*/
	void shortlist(const unsigned char *query_signature, ANT_index_tombstones *tombstones, long long pool_size, long long *out_docids, long long *out_count);
} ;

class ANT_signature_store_writer
{
private:
	char filename[4096];
	long long bits;
	long long documents;
	long long capacity;
	unsigned char *presence;
	unsigned char *signatures;

	long long sig_bytes(void) { return (bits + 7) / 8; }
	long grow(void);

public:
	ANT_signature_store_writer();
	~ANT_signature_store_writer();

	long create(const char *filename, long long bits);		// 0 on success
	long append(const unsigned char *signature_or_null);	// 0 on success
	long finish(void);										// write + rename; 0 on success
	void abandon(void);
} ;

#endif /* SIGNATURE_STORE_H_ */
```

- [ ] **Step 4: `source/signature_store.cpp`**

```cpp
/*
	SIGNATURE_STORE.CPP
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "signature_store.h"
#include "index_tombstones.h"

#define ANT_SIGNATURE_STORE_MAGIC 0x3130474953544E41ULL		/* "ANTSIG01" little-endian */
#define ANT_SIGNATURE_STORE_VERSION 1u

ANT_signature_store::ANT_signature_store()
{
bits = 0; documents = 0; presence = NULL; signatures = NULL;
}

ANT_signature_store::~ANT_signature_store()
{
delete [] presence;
delete [] signatures;
}

ANT_signature_store *ANT_signature_store::load(const char *filename, long long expected_bits, long long expected_documents)
{
FILE *fp;
unsigned long long magic;
unsigned int version;
long long stored_bits, stored_documents, sig_bytes, presence_bytes, file_size, expected_size;
ANT_signature_store *result = new ANT_signature_store();

if ((fp = fopen(filename, "rb")) == NULL)
	return result;

if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != ANT_SIGNATURE_STORE_MAGIC
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != ANT_SIGNATURE_STORE_VERSION
	|| fread(&stored_bits, sizeof(stored_bits), 1, fp) != 1
	|| fread(&stored_documents, sizeof(stored_documents), 1, fp) != 1
	|| stored_bits != expected_bits || stored_documents != expected_documents
	|| stored_bits < 8 || stored_bits > 65536 || (stored_bits % 8) != 0
	|| stored_documents < 0 || stored_documents > (1LL << 40))
	{
	fclose(fp);
	return result;
	}

sig_bytes = (stored_bits + 7) / 8;
presence_bytes = (stored_documents + 7) / 8;
expected_size = 28 + presence_bytes + stored_documents * sig_bytes;		// header is 8+4+8+8 = 28 bytes
if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) != expected_size || fseek(fp, 28, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

unsigned char *presence_buffer = new unsigned char[presence_bytes > 0 ? presence_bytes : 1];
unsigned char *signature_buffer = new unsigned char[stored_documents * sig_bytes > 0 ? stored_documents * sig_bytes : 1];
if (fread(presence_buffer, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
	|| fread(signature_buffer, 1, (size_t)(stored_documents * sig_bytes), fp) != (size_t)(stored_documents * sig_bytes))
	{
	delete [] presence_buffer; delete [] signature_buffer; fclose(fp);
	return result;
	}
fclose(fp);

result->bits = stored_bits;
result->documents = stored_documents;
result->presence = presence_buffer;
result->signatures = signature_buffer;
return result;
}

void ANT_signature_store::shortlist(const unsigned char *query_signature, ANT_index_tombstones *tombstones, long long pool_size, long long *out_docids, long long *out_count)
{
long long docid, i, count = 0, worst_index, h, sb = signature_bytes();
long long *hammings;

if (presence == NULL || signatures == NULL || pool_size < 1)
	{ *out_count = 0; return; }
hammings = new long long[pool_size];

for (docid = 0; docid < documents; docid++)
	{
	if (!has(docid))
		continue;
	if (tombstones != NULL && tombstones->is_deleted(docid))
		continue;
	{
	const unsigned char *sig = get(docid);
	h = 0;
	for (i = 0; i < sb; i++)
		h += (long long)__builtin_popcount((unsigned int)(sig[i] ^ query_signature[i]));
	}
	if (count < pool_size)
		{ out_docids[count] = docid; hammings[count] = h; count++; }
	else
		{
		worst_index = 0;
		for (i = 1; i < pool_size; i++)
			if (hammings[i] > hammings[worst_index])
				worst_index = i;
		if (h < hammings[worst_index])
			{ out_docids[worst_index] = docid; hammings[worst_index] = h; }
		}
	}
*out_count = count;
delete [] hammings;
}

/* ---- writer ---- */

ANT_signature_store_writer::ANT_signature_store_writer()
{
filename[0] = '\0'; bits = 0; documents = 0; capacity = 0; presence = NULL; signatures = NULL;
}

ANT_signature_store_writer::~ANT_signature_store_writer()
{
delete [] presence;
delete [] signatures;
}

long ANT_signature_store_writer::create(const char *name, long long width)
{
if (width < 8 || width > 65536 || (width % 8) != 0)
	return 1;
strncpy(filename, name, sizeof(filename) - 1);
filename[sizeof(filename) - 1] = '\0';
bits = width;
documents = 0;
capacity = 1024;
presence = new unsigned char[(capacity + 7) / 8];
memset(presence, 0, (size_t)((capacity + 7) / 8));
signatures = new unsigned char[capacity * sig_bytes()];
return 0;
}

long ANT_signature_store_writer::grow(void)
{
long long new_capacity = capacity * 2, sb = sig_bytes();
unsigned char *new_presence = new unsigned char[(new_capacity + 7) / 8];
unsigned char *new_signatures = new unsigned char[new_capacity * sb];
memset(new_presence, 0, (size_t)((new_capacity + 7) / 8));
memcpy(new_presence, presence, (size_t)((capacity + 7) / 8));
memcpy(new_signatures, signatures, (size_t)(capacity * sb));
delete [] presence; delete [] signatures;
presence = new_presence; signatures = new_signatures; capacity = new_capacity;
return 0;
}

long ANT_signature_store_writer::append(const unsigned char *signature_or_null)
{
long long sb = sig_bytes();
if (documents >= capacity)
	grow();
if (signature_or_null == NULL)
	memset(signatures + documents * sb, 0, (size_t)sb);
else
	{
	memcpy(signatures + documents * sb, signature_or_null, (size_t)sb);
	presence[documents / 8] |= (unsigned char)(1 << (documents % 8));
	}
documents++;
return 0;
}

long ANT_signature_store_writer::finish(void)
{
char temp_name[4200];
FILE *fp;
unsigned long long magic = ANT_SIGNATURE_STORE_MAGIC;
unsigned int version = ANT_SIGNATURE_STORE_VERSION;
long long sb = sig_bytes(), presence_bytes = (documents + 7) / 8;

if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&bits, sizeof(bits), 1, fp) != 1 || fwrite(&documents, sizeof(documents), 1, fp) != 1
	|| fwrite(presence, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
	|| fwrite(signatures, 1, (size_t)(documents * sb), fp) != (size_t)(documents * sb))
	{ fclose(fp); remove(temp_name); return 1; }
fclose(fp);
if (rename(temp_name, filename) != 0)
	{ remove(temp_name); return 1; }
return 0;
}

void ANT_signature_store_writer::abandon(void)
{
delete [] presence; delete [] signatures;
presence = NULL; signatures = NULL; documents = 0; capacity = 0;
}
```

- [ ] **Step 5: Run to verify it passes** — `rm -f obj/*.o && make tests 2>&1 | tail -3 && ./bin/test_signature_store` → `PASSED`.

- [ ] **Step 6: Commit**

```bash
git add source/signature_store.h source/signature_store.cpp tests/test_signature_store.cpp
git commit -m "feat(vector-v2): ANT_signature_store .vsig sidecar + writer + shortlist"
```

---

## Task 3: `signature.config` + `set_approximate_config` + open-load

**Files:** Modify `atire/atire_segment_index.h`, `atire/atire_segment_index.cpp`, `atire/atire_segment_index_vector.cpp`; `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** (append to `tests/test_segment_index.cpp`, add its call in `main()` after the vector tests)

```cpp
static void test_approx_config_persists(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(a->open(dir) == 0);
CHECK(a->set_approximate_config(0) == 0);		// 0 => default 256 bits
CHECK(a->approximate_configured() == 1);
delete a;
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(dir) == 0);
CHECK(b->approximate_configured() == 1);		// config reloads
delete b;
delete [] dir;
printf("test_approx_config_persists OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — `rm -f obj/*.o && make tests 2>&1 | tail -5` → FAIL (`set_approximate_config` not a member).

- [ ] **Step 3: `atire/atire_segment_index.h`** — add a forward decl near the other `class` forward decls:

```cpp
class ANT_signature;
```

Add private members near `vector_dimension_current`:

```cpp
	long long signature_bits_current;		// 0 = approximate not configured
	unsigned long long signature_seed;
	long long candidate_multiplier;			// rerank pool = top_k * this (default 4)
	ANT_signature *query_signer;			// index-wide projection, built at open when configured (NULL otherwise)
```

Add private method decls near `load_vector_config`:

```cpp
	long load_signature_config(void);
	long save_signature_config(void);
	void rebuild_query_signer(void);
	long long vector_candidates_approx(const float *query, long long top_k, ANT_vector_candidate *best);	// Task 7
```

Add public API near `set_vector_config`:

```cpp
	long set_approximate_config(long long bits);		// bits<=0 => default 256; persists signature.config on first enable; 0 on success
	void set_candidate_multiplier(long long n);			// clamps to >= 1
	long approximate_configured(void) { return signature_bits_current != 0; }
	long build_signatures(void);												// Task 6-mate: backfill
	long long search_vector_approx(const float *query, long long top_k);		// Task 7
	long long search_hybrid_approx(char *query_text, const float *query_vector, long long top_k);	// Task 8
	hit *get_hit(long long i) { return &results[i]; }							// test hook: valid until next search
```

- [ ] **Step 4: `atire/atire_segment_index.cpp`** — in the ctor body (by the vector-member inits):

```cpp
signature_bits_current = 0;
signature_seed = 0;
candidate_multiplier = 4;
query_signer = NULL;
```

In the destructor (by the vector cleanup): `delete query_signer;`

At the top of `atire/atire_segment_index.cpp` and `atire/atire_segment_index_vector.cpp` add:

```cpp
#include "../source/signature.h"
#include "../source/signature_store.h"
```

- [ ] **Step 5: config load/save + setters in `atire/atire_segment_index_vector.cpp`** (also add `#include <time.h>`)

```cpp
long ATIRE_segment_index::load_signature_config(void)
{
char filename[4096];
FILE *fp;
unsigned long long magic, seed;
unsigned int version;
long long bits;

snprintf(filename, sizeof(filename), "%s/signature.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != 0x3130474953544E41ULL
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != 1u
	|| fread(&bits, sizeof(bits), 1, fp) != 1
	|| fread(&seed, sizeof(seed), 1, fp) != 1
	|| bits < 8 || bits > 65536 || (bits % 8) != 0)
	{ fclose(fp); return 0; }
fclose(fp);
signature_bits_current = bits;
signature_seed = seed;
return 0;
}

long ATIRE_segment_index::save_signature_config(void)
{
char filename[4096], temp_name[4200];
FILE *fp;
unsigned long long magic = 0x3130474953544E41ULL, seed = signature_seed;
unsigned int version = 1u;
long long bits = signature_bits_current;

snprintf(filename, sizeof(filename), "%s/signature.config", directory);
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&bits, sizeof(bits), 1, fp) != 1 || fwrite(&seed, sizeof(seed), 1, fp) != 1)
	{ fclose(fp); remove(temp_name); return 1; }
fclose(fp);
if (rename(temp_name, filename) != 0)
	{ remove(temp_name); return 1; }
return 0;
}

void ATIRE_segment_index::rebuild_query_signer(void)
{
delete query_signer;
query_signer = NULL;
if (signature_bits_current != 0 && vector_dimension_current != 0)
	query_signer = new ANT_signature(vector_dimension_current, signature_bits_current, signature_seed);
}

long ATIRE_segment_index::set_approximate_config(long long bits)
{
if (directory == NULL)
	return 1;						// must be open (needs directory + dimension)
if (vector_dimension_current == 0)
	return 1;						// approximate requires vectors enabled
if (signature_bits_current != 0)
	return 0;						// already configured; immutable
if (bits <= 0)
	bits = 256;
if (bits > 65536 || (bits % 8) != 0)
	return 1;
signature_bits_current = bits;
signature_seed = (unsigned long long)time(NULL) ^ ((unsigned long long)bits << 32) ^ 0x9E3779B97F4A7C15ULL;
if (save_signature_config() != 0)
	{ signature_bits_current = 0; signature_seed = 0; return 1; }
rebuild_query_signer();
return 0;
}

void ATIRE_segment_index::set_candidate_multiplier(long long n)
{
candidate_multiplier = n < 1 ? 1 : n;
}
```

- [ ] **Step 6: call config load in `open()`** (`atire/atire_segment_index.cpp`) — immediately after the existing `load_vector_config()` call:

```cpp
load_signature_config();
rebuild_query_signer();
```

- [ ] **Step 7: Run to verify it passes** — `rm -f obj/*.o && make all && make tests 2>&1 | tail -3 && ./bin/test_segment_index 2>&1 | grep -E "approx_config|PASSED"` → `test_approx_config_persists OK`, `PASSED`.

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v2): signature.config + set_approximate_config + open-load"
```

---

## Task 4: flush writes `seg_G.vsig`

**Files:** Modify `atire/atire_segment_index.cpp`, `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** (append + call in `main()`)

```cpp
static void test_flush_writes_signatures(void)
{
char *dir = make_index_dir();
char vsig[4096];
float v[8] = {1,0,0,0,0,0,0,0};
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_approximate_config(64) == 0);
CHECK(idx->add_document("d1", "<DOC>alpha</DOC>", v) >= 0);
CHECK(idx->flush() == 0);
long long g = idx->disk_segment_generation(0);
snprintf(vsig, sizeof(vsig), "%s/seg_%lld.vsig", dir, g);
FILE *fp = fopen(vsig, "rb");
CHECK(fp != NULL);				// signature sidecar written at flush
fclose(fp);
delete idx;
delete [] dir;
printf("test_flush_writes_signatures OK\n");
}
```

Note: this assumes segment sidecars are named `seg_<gen>.<ext>`. Confirm against
`segment_filename()` in `atire/atire_segment_index.cpp` and adjust the `snprintf` if the
naming differs.

- [ ] **Step 2: Run to verify it fails** — `make tests 2>&1 | tail -3 && ./bin/test_segment_index 2>&1 | grep -E "flush_writes_signatures|FAIL"` → FAIL.

- [ ] **Step 3: Implement in `flush()`** — find the `.vec` write block in `flush()` (`ANT_vector_store_writer` producing `seg_<writer_generation>.vec`). Immediately after its successful `finish()`, add:

```cpp
	/*
		V2: write the signature sidecar alongside .vec, signing each present
		vector.  Non-fatal to the flush -- a failure leaves the segment
		signature-less (exact-scanned) until build_signatures()/compaction.
	*/
	if (signature_bits_current != 0 && query_signer != NULL)
		{
		char vsig_name[4096];
		segment_filename(vsig_name, sizeof(vsig_name), writer_generation, "vsig");
		ANT_signature_store_writer sig_writer;
		unsigned char *sig = new unsigned char[query_signer->signature_bytes()];
		long sig_failed = sig_writer.create(vsig_name, signature_bits_current) != 0;
		for (long long docid = 0; !sig_failed && docid < writer_documents; docid++)
			{
			if (writer_vector_presence != NULL && (writer_vector_presence[docid / 8] & (1 << (docid % 8))))
				{ query_signer->sign(writer_vector_data + docid * vector_dimension_current, sig); sig_failed = sig_writer.append(sig) != 0; }
			else
				sig_failed = sig_writer.append(NULL) != 0;
			}
		if (!sig_failed) sig_writer.finish(); else sig_writer.abandon();
		delete [] sig;
		}
```

- [ ] **Step 4: Run to verify it passes** — `make all && ./bin/test_segment_index 2>&1 | grep -E "flush_writes_signatures|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v2): flush writes the seg_G.vsig signature sidecar"
```

---

## Task 5: `build_signatures()` backfill

**Files:** Modify `atire/atire_segment_index_vector.cpp`, `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** (append + call in `main()`)

```cpp
static void test_build_signatures_backfill(void)
{
char *dir = make_index_dir();
char vsig[4096];
float v[8] = {0,1,0,0,0,0,0,0};
ATIRE_segment_index *a = new ATIRE_segment_index();		// segment created BEFORE approximate enabled
CHECK(a->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(a->open(dir) == 0);
CHECK(a->add_document("d1", "<DOC>beta</DOC>", v) >= 0);
CHECK(a->flush() == 0);
long long g = a->disk_segment_generation(0);
delete a;
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(dir) == 0);
CHECK(b->set_approximate_config(64) == 0);
snprintf(vsig, sizeof(vsig), "%s/seg_%lld.vsig", dir, g);
CHECK(fopen(vsig, "rb") == NULL);		// not there yet
CHECK(b->build_signatures() == 0);
FILE *fp = fopen(vsig, "rb");
CHECK(fp != NULL);						// backfilled
fclose(fp);
delete b;
delete [] dir;
printf("test_build_signatures_backfill OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — `make tests && ./bin/test_segment_index 2>&1 | grep -E "build_signatures_backfill|FAIL"` → FAIL.

- [ ] **Step 3: Implement `build_signatures()`** (`atire/atire_segment_index_vector.cpp`)

```cpp
/*
	ATIRE_SEGMENT_INDEX::BUILD_SIGNATURES()
	---------------------------------------
	Idempotent backfill: for every manifested disk segment with vectors but no
	valid .vsig, sign its dense vectors into a fresh sidecar (loaded on next
	open()).  Per-segment failures are skipped, never left corrupt.  Returns 0
	on success (1 if approximate is unconfigured).
*/
long ATIRE_segment_index::build_signatures(void)
{
long long which, docid;
char vec_name[4096], vsig_name[4096];

if (signature_bits_current == 0 || query_signer == NULL)
	return 1;

for (which = 0; which < segment_count; which++)
	{
	long long generation = segments[which].generation;
	long long docs = segments[which].engine->get_document_count();

	segment_filename(vsig_name, sizeof(vsig_name), generation, "vsig");
	ANT_signature_store *existing = ANT_signature_store::load(vsig_name, signature_bits_current, docs);
	long long already = existing->document_count() == docs && docs > 0;
	delete existing;
	if (already)
		continue;

	segment_filename(vec_name, sizeof(vec_name), generation, "vec");
	ANT_vector_store *vectors = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
	if (vectors->document_count() != docs)
		{ delete vectors; continue; }

	ANT_signature_store_writer sig_writer;
	unsigned char *sig = new unsigned char[query_signer->signature_bytes()];
	long failed = sig_writer.create(vsig_name, signature_bits_current) != 0;
	for (docid = 0; !failed && docid < docs; docid++)
		{
		if (vectors->has(docid)) { query_signer->sign(vectors->get(docid), sig); failed = sig_writer.append(sig) != 0; }
		else failed = sig_writer.append(NULL) != 0;
		}
	if (!failed) sig_writer.finish(); else sig_writer.abandon();
	delete [] sig;
	delete vectors;
	}
return 0;
}
```

- [ ] **Step 4: Run to verify it passes** — `make all && ./bin/test_segment_index 2>&1 | grep -E "build_signatures_backfill|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v2): build_signatures() backfill for pre-existing segments"
```

---

## Task 6: cache `segment.signatures` — load on segment open + orphan sweep

This is the prerequisite for the `*_approx` searches: give each in-memory segment a cached
signature store, loaded whenever a segment is opened/appended (including at flush time).

**Files:** Modify `atire/atire_segment_index.h` (segment struct), `atire/atire_segment_index.cpp`; `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** (append + call in `main()`)

```cpp
static void test_segment_signatures_loaded(void)
{
char *dir = make_index_dir();
float v[8] = {1,1,0,0,0,0,0,0};
ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(a->open(dir) == 0);
CHECK(a->set_approximate_config(64) == 0);
CHECK(a->add_document("d1", "<DOC>alpha</DOC>", v) >= 0);
CHECK(a->flush() == 0);
delete a;
/* reopen: the flushed segment's .vsig must be cached and expose a signature store */
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(dir) == 0);
CHECK(b->disk_segment_count() == 1);
CHECK(b->disk_segment_has_signatures(0) == 1);		// new test hook (Step 3)
delete b;
delete [] dir;
printf("test_segment_signatures_loaded OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL (`disk_segment_has_signatures` not a member).

- [ ] **Step 3: Add the field, loading, freeing, sweep, and the test hook**

In `atire/atire_segment_index.h`, in the `segment` struct (next to `ANT_vector_store *vectors;`):

```cpp
	ANT_signature_store *signatures;		// NULL when absent/degraded
```

Add a public test hook near the other `disk_segment_*` hooks:

```cpp
	long disk_segment_has_signatures(long long which) { return segments[which].signatures != NULL && segments[which].signatures->document_count() > 0; }
```

In `atire/atire_segment_index.cpp`, wherever a segment slot is initialized and its `.vec`
is loaded (search for `.vectors = ANT_vector_store::load` — this is the shared segment-open
path used by both `open()` and `append_segment()`), load the `.vsig` right after, and
initialize the field to NULL when approximate is off:

```cpp
	if (signature_bits_current != 0)
		{
		char vsig_name[4096];
		segment_filename(vsig_name, sizeof(vsig_name), segments[i].generation, "vsig");
		segments[i].signatures = ANT_signature_store::load(vsig_name, signature_bits_current, segments[i].engine->get_document_count());
		}
	else
		segments[i].signatures = NULL;
```

(Use the same index variable / generation expression the surrounding `.vec` load uses; the
snippet above assumes an index `i` and `segments[i].generation` — match the local code.)

Wherever a segment's `vectors` is `delete`d (segment teardown, dtor, `remove`/compaction
input drop), add alongside:

```cpp
	delete segments[...].signatures;
	segments[...].signatures = NULL;
```

In the orphan-sweep in `open()` (where unmanifested per-generation `.vec`/`.del` files are
removed for generations not in the manifest), add `.vsig` to the extensions removed, exactly
parallel to `.vec`.

- [ ] **Step 4: Run to verify it passes** — `rm -f obj/*.o && make all && ./bin/test_segment_index 2>&1 | grep -E "segment_signatures_loaded|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v2): cache segment.signatures (load on open/append) + orphan sweep"
```

---

## Task 7: `search_vector_approx` — prefilter + rerank + L2 fallback

**Files:** Modify `atire/atire_segment_index_vector.cpp`, `tests/test_segment_index.cpp`

Read `search_vector` and `vector_candidates` in `atire_segment_index_vector.cpp` first — the
approx path reuses `search_vector`'s downstream (sort + `resolve_hit_filename` + top-k
truncation) verbatim; only candidate-gathering differs.

- [ ] **Step 1: Write the failing tests** (append + call both in `main()`)

```cpp
static void test_approx_recall(void)
{
char *dir = make_index_dir();
long long dim = 32, n = 400, k = 10, i, d;
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_approximate_config(256) == 0);
idx->set_candidate_multiplier(4);
srand(7);
float *vecs = new float[n * dim];
char key[32], doc[64];
for (i = 0; i < n; i++)
	{
	for (d = 0; d < dim; d++) vecs[i * dim + d] = (float)(rand() % 200 - 100);
	snprintf(key, sizeof(key), "k%lld", i);
	snprintf(doc, sizeof(doc), "<DOC>term%lld</DOC>", i);
	CHECK(idx->add_document(key, doc, vecs + i * dim) >= 0);
	}
CHECK(idx->flush() == 0);
float query[32]; for (d = 0; d < dim; d++) query[d] = (float)(rand() % 200 - 100);
long long exact_hits = idx->search_vector(query, k);
CHECK(exact_hits == k);
char exact_keys[10][256];
for (i = 0; i < k; i++) strcpy(exact_keys[i], idx->get_hit(i)->filename);
long long approx_hits = idx->search_vector_approx(query, k);
CHECK(approx_hits == k);
long long overlap = 0, j;
for (i = 0; i < k; i++)
	for (j = 0; j < k; j++)
		if (strcmp(exact_keys[i], idx->get_hit(j)->filename) == 0) { overlap++; break; }
CHECK((double)overlap / (double)k >= 0.9);
delete [] vecs; delete idx; delete [] dir;
printf("test_approx_recall OK (recall=%.2f)\n", (double)overlap / (double)k);
}

static void test_approx_l2_fallback(void)
{
char *dir = make_index_dir();
long long dim = 8, i, d;
float v[8];
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_L2) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_approximate_config(64) == 0);
for (i = 0; i < 20; i++)
	{
	for (d = 0; d < dim; d++) v[d] = (float)((i * 7 + d) % 11);
	char key[16]; snprintf(key, sizeof(key), "k%lld", i);
	CHECK(idx->add_document(key, "<DOC>x</DOC>", v) >= 0);
	}
CHECK(idx->flush() == 0);
float q[8]; for (d = 0; d < dim; d++) q[d] = (float)(d % 5);
long long he = idx->search_vector(q, 5);
char ek[5][256]; for (i = 0; i < he; i++) strcpy(ek[i], idx->get_hit(i)->filename);
long long ha = idx->search_vector_approx(q, 5);
CHECK(ha == he);
for (i = 0; i < ha; i++) CHECK(strcmp(ek[i], idx->get_hit(i)->filename) == 0);		// identical ranking
delete idx; delete [] dir;
printf("test_approx_l2_fallback OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL (`search_vector_approx` unimplemented).

- [ ] **Step 3: Implement the approx gatherer** (`atire/atire_segment_index_vector.cpp`)

```cpp
/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES_APPROX()
	-----------------------------------------------
	Like vector_candidates(), but each disk segment WITH a valid cached signature
	store is Hamming-shortlisted (pool = top_k * candidate_multiplier) and only
	those candidates are exact-reranked.  Segments without signatures, and the
	live memory buffer, are exact-scanned.  Caller guarantees metric != L2 and
	approximate is configured.
*/
long long ATIRE_segment_index::vector_candidates_approx(const float *query, long long top_k, ANT_vector_candidate *best)
{
long long which, docid, best_count = 0, pool_size = top_k * candidate_multiplier;
float *normalized = NULL;
unsigned char *query_sig = new unsigned char[query_signer->signature_bytes()];
long long *pool = new long long[pool_size > 0 ? pool_size : 1];

if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{ delete [] normalized; delete [] query_sig; delete [] pool; return 0; }
	query = normalized;
	}

query_signer->sign(query, query_sig);

for (which = 0; which < segment_count; which++)
	{
	if (segments[which].vectors == NULL)
		continue;
	if (segments[which].signatures != NULL && segments[which].signatures->document_count() == segments[which].engine->get_document_count())
		{
		long long count = 0, p;
		segments[which].signatures->shortlist(query_sig, segments[which].tombstones, pool_size, pool, &count);
		for (p = 0; p < count; p++)
			{
			docid = pool[p];
			if (!segments[which].vectors->has(docid))
				continue;
			ANT_vector_candidate_insert(best, &best_count, top_k,
				ANT_vector_store::kernel(query, segments[which].vectors->get(docid), vector_dimension_current, vector_metric),
				segments[which].generation, docid);
			}
		}
	else
		segments[which].vectors->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k);
	}

for (docid = 0; docid < writer_documents; docid++)		// live memory buffer: always exact
	{
	if (writer_vector_presence == NULL || !(writer_vector_presence[docid / 8] & (1 << (docid % 8))))
		continue;
	if (writer_tombstones->is_deleted(docid))
		continue;
	ANT_vector_candidate_insert(best, &best_count, top_k,
		ANT_vector_store::kernel(query, writer_vector_data + docid * vector_dimension_current, vector_dimension_current, vector_metric),
		writer_generation, docid);
	}

delete [] normalized;
delete [] query_sig;
delete [] pool;
return best_count;
}
```

- [ ] **Step 4: Implement `search_vector_approx`** — copy `search_vector`'s body verbatim into a new method, changing only (a) the guard at the top and (b) the single `vector_candidates(` call to `vector_candidates_approx(`:

```cpp
long long ATIRE_segment_index::search_vector_approx(const float *query, long long top_k)
{
if (signature_bits_current == 0 || query_signer == NULL || vector_metric == VECTOR_METRIC_L2)
	return search_vector(query, top_k);			// transparent fallback
/* --- begin: verbatim copy of search_vector()'s body, with vector_candidates(...) --> vector_candidates_approx(...) --- */
/* ...paste the exact contents of search_vector() here, changing only that one call... */
/* --- end verbatim copy --- */
}
```

Open `search_vector` in the same file and paste its full body into the marked region,
renaming the one `vector_candidates(query, top_k, best)` call to
`vector_candidates_approx(query, top_k, best)`. Do not change anything else (the `qsort`,
`reset_results`, `append_result`/`resolve_hit_filename` loop, truncation, and return stay
identical, so approximate and exact share ranking behavior).

- [ ] **Step 5: Run to verify it passes** — `make all && ./bin/test_segment_index 2>&1 | grep -E "approx_recall|approx_l2_fallback|PASSED"` → both OK.

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v2): search_vector_approx prefilter + rerank + L2 fallback"
```

---

## Task 8: `search_hybrid_approx`

**Files:** Modify `atire/atire_segment_index_vector.cpp`, `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** (append + call in `main()`)

```cpp
static void test_hybrid_approx_smoke(void)
{
char *dir = make_index_dir();
long long dim = 16, i, d;
float v[16];
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_approximate_config(128) == 0);
for (i = 0; i < 50; i++)
	{
	for (d = 0; d < dim; d++) v[d] = (float)((i + d) % 9 - 4);
	char key[16]; snprintf(key, sizeof(key), "k%lld", i);
	CHECK(idx->add_document(key, "<DOC>alpha beta</DOC>", v) >= 0);
	}
CHECK(idx->flush() == 0);
char query[32]; strcpy(query, "alpha");
float qv[16]; for (d = 0; d < dim; d++) qv[d] = (float)(d % 5 - 2);
long long hits = idx->search_hybrid_approx(query, qv, 5);
CHECK(hits > 0 && hits <= 5);
delete idx; delete [] dir;
printf("test_hybrid_approx_smoke OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL (`search_hybrid_approx` unimplemented).

- [ ] **Step 3: Implement `search_hybrid_approx`** — copy `search_hybrid`'s body verbatim into a new method, changing only (a) the fallback guard and (b) the single `vector_candidates(` call to `vector_candidates_approx(`:

```cpp
long long ATIRE_segment_index::search_hybrid_approx(char *query_text, const float *query_vector, long long top_k)
{
if (signature_bits_current == 0 || query_signer == NULL || vector_metric == VECTOR_METRIC_L2)
	return search_hybrid(query_text, query_vector, top_k);		// transparent fallback
/* --- begin: verbatim copy of search_hybrid()'s body, with vector_candidates(...) --> vector_candidates_approx(...) --- */
/* ...paste the exact contents of search_hybrid() here, changing only that one call... */
/* --- end verbatim copy --- */
}
```

Do not alter the RRF (k=60) math or the lexical leg — only the vector-candidate source changes.

- [ ] **Step 4: Run to verify it passes** — `make all && ./bin/test_segment_index 2>&1 | grep -E "hybrid_approx_smoke|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v2): search_hybrid_approx (RRF over approximate vector leg)"
```

---

## Task 9: compaction rewrites `.vsig`

**Files:** Modify `atire/atire_segment_index_compaction.cpp`, `tests/test_segment_index.cpp`

- [ ] **Step 1: Write the failing test** (append + call in `main()`)

```cpp
static void test_compaction_preserves_signatures(void)
{
char *dir = make_index_dir();
char vsig[4096];
long long dim = 16, i, d;
float v[16];
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_approximate_config(128) == 0);
for (i = 0; i < 10; i++) { for (d=0;d<dim;d++) v[d]=(float)((i+d)%7); char k[16]; snprintf(k,sizeof(k),"a%lld",i); CHECK(idx->add_document(k,"<DOC>x</DOC>",v)>=0); }
CHECK(idx->flush() == 0);
for (i = 0; i < 10; i++) { for (d=0;d<dim;d++) v[d]=(float)((i*3+d)%7); char k[16]; snprintf(k,sizeof(k),"b%lld",i); CHECK(idx->add_document(k,"<DOC>x</DOC>",v)>=0); }
CHECK(idx->flush() == 0);
long long gens[2] = { idx->disk_segment_generation(0), idx->disk_segment_generation(1) };
CHECK(idx->compact(gens, 2) == 0);
long long out_gen = idx->disk_segment_generation(0);
snprintf(vsig, sizeof(vsig), "%s/seg_%lld.vsig", dir, out_gen);
FILE *fp = fopen(vsig, "rb");
CHECK(fp != NULL);						// compaction wrote a .vsig for the merged segment
fclose(fp);
float q[16]; for (d=0;d<dim;d++) q[d]=(float)(d%5);
CHECK(idx->search_vector_approx(q, 5) == 5);		// approx still returns k after compaction
delete idx; delete [] dir;
printf("test_compaction_preserves_signatures OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL (no `.vsig` for the merged output).

- [ ] **Step 3: Rewrite `.vsig` during compaction** (`atire/atire_segment_index_compaction.cpp`; add the two `signature*` includes at the top)

Find the `.vec` rewrite block in `compact()` (the `ANT_docid_renumberer` + `ANT_vector_store_writer` producing `seg_<output_generation>.vec`). **After** `append_segment(output_generation)` has populated `output_segment`, add:

```cpp
	/*
		V2: rebuild the merged segment's signature sidecar by signing the merged
		DENSE vectors we just wrote (re-load the fresh output .vec) so signatures
		stay aligned to the compacted docids.  Best-effort: failure leaves the
		output signature-less (exact-scanned), never aborts a successful merge.
	*/
	if (signature_bits_current != 0 && query_signer != NULL)
		{
		char out_vec[4096], out_vsig[4096];
		segment_filename(out_vec, sizeof(out_vec), output_generation, "vec");
		segment_filename(out_vsig, sizeof(out_vsig), output_generation, "vsig");
		long long out_docs = output_segment->engine->get_document_count();
		ANT_vector_store *out_vectors = ANT_vector_store::load(out_vec, vector_dimension_current, out_docs);
		if (out_vectors->document_count() == out_docs && out_docs > 0)
			{
			ANT_signature_store_writer sig_writer;
			unsigned char *sig = new unsigned char[query_signer->signature_bytes()];
			long failed = sig_writer.create(out_vsig, signature_bits_current) != 0;
			for (long long docid = 0; !failed && docid < out_docs; docid++)
				{
				if (out_vectors->has(docid)) { query_signer->sign(out_vectors->get(docid), sig); failed = sig_writer.append(sig) != 0; }
				else failed = sig_writer.append(NULL) != 0;
				}
			if (!failed) sig_writer.finish(); else sig_writer.abandon();
			delete [] sig;
			}
		delete out_vectors;
		}
```

Also cache the new segment's signatures in memory so the same-session `search_vector_approx`
in the test engages the prefilter: after the block above, load it into `output_segment`:

```cpp
	if (signature_bits_current != 0)
		{
		delete output_segment->signatures;
		char out_vsig2[4096];
		segment_filename(out_vsig2, sizeof(out_vsig2), output_generation, "vsig");
		output_segment->signatures = ANT_signature_store::load(out_vsig2, signature_bits_current, output_segment->engine->get_document_count());
		}
```

(If `compact()` already reloads/rebuilds the `output_segment` vector store in memory after the
swap, mirror that exact pattern for signatures instead of the snippet above.)

- [ ] **Step 4: Run to verify it passes** — `rm -f obj/*.o && make all && ./bin/test_segment_index 2>&1 | grep -E "compaction_preserves_signatures|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_compaction.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v2): compaction rewrites the merged .vsig sidecar"
```

---

## Task 10: Node binding rider

**Files:** Modify `nodejs/addon/segment_index.cpp`, `nodejs/segment_index.d.ts`, `nodejs/README.md`; create `nodejs/test/approximate.test.js` (match the repo's JS test layout / `test:segment` file list — add the new file to that list if it is explicit)

- [ ] **Step 1: Write the failing JS test** (`nodejs/test/approximate.test.js`)

```js
const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('..');

test('approximate: buildSignatures + searchVectorApprox returns hits', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_approx_'));
  const dim = 16;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', approximate: { bits: 128, multiplier: 4 } });
  idx.open(dir);
  for (let i = 0; i < 40; i++) {
    const v = new Float32Array(dim);
    for (let d = 0; d < dim; d++) v[d] = ((i + d) % 7) - 3;
    idx.addDocument('k' + i, '<DOC>alpha</DOC>', v);
  }
  await idx.flush();
  await idx.buildSignatures();
  const q = new Float32Array(dim);
  for (let d = 0; d < dim; d++) q[d] = (d % 5) - 2;
  const hits = idx.searchVectorApprox(q, 5);
  assert.strictEqual(hits.length, 5);
  assert.ok(hits[0].key && typeof hits[0].score === 'number');
  idx.close();
});
```

- [ ] **Step 2: Run to verify it fails** — `cd nodejs && npm run build:segment && npm run test:segment 2>&1 | tail -8` → FAIL (`approximate`/`buildSignatures`/`searchVectorApprox` unknown).

- [ ] **Step 3: Wire the addon** (`nodejs/addon/segment_index.cpp`)

After `open()` succeeds in the options-parsing path (mirroring where `set_vector_config` is
applied), read `approximate`:

```cpp
	if (options.Has("approximate") && options.Get("approximate").IsObject())
		{
		Napi::Object approx = options.Get("approximate").As<Napi::Object>();
		long long bits = approx.Has("bits") ? (long long)approx.Get("bits").As<Napi::Number>().Int64Value() : 0;
		index->set_approximate_config(bits);					// non-fatal on failure: approximate simply stays off
		if (approx.Has("multiplier"))
			index->set_candidate_multiplier((long long)approx.Get("multiplier").As<Napi::Number>().Int64Value());
		}
```

Add a `BuildSignatures` AsyncWorker method mirroring the existing `Flush`/`Maintain` worker +
busy-guard (state must be OPEN; concurrent maintenance throws "maintenance in progress"); its
`Execute()` calls `index->build_signatures()`. Add sync `SearchVectorApprox` and
`SearchHybridApprox` mirroring `SearchVector`/`SearchHybrid` (call `search_vector_approx` /
`search_hybrid_approx`, return the same `{key, score, generation, docid}` array). Register
`buildSignatures`, `searchVectorApprox`, `searchHybridApprox` in `DefineClass`.

- [ ] **Step 4: `d.ts` + README** — in `nodejs/segment_index.d.ts` add to `SegmentIndexOptions`:

```ts
  approximate?: { bits?: number; multiplier?: number };
```

and to the class:

```ts
  buildSignatures(): Promise<void>;
  searchVectorApprox(vector: Float32Array | number[], k: number): Hit[];
  searchHybridApprox(text: string, vector: Float32Array | number[], k: number): Hit[];
```

In `nodejs/README.md`, add an "Approximate vector search (V2)" subsection documenting the
`approximate` option, `buildSignatures()`, and the `*Approx` methods, noting cosine/dot only
(L2 transparently falls back to exact).

- [ ] **Step 5: Run to verify it passes** — `cd nodejs && npm run build:segment && npm run test:segment 2>&1 | tail -8` → `pass 12  fail 0`.

- [ ] **Step 6: Commit**

```bash
git add nodejs/addon/segment_index.cpp nodejs/segment_index.d.ts nodejs/README.md nodejs/test/approximate.test.js
git commit -m "feat(vector-v2): Node binding -- approximate option, buildSignatures, *Approx"
```

---

## Final verification (after all tasks)

```bash
cd /data/tyolab/code/antelope
rm -f obj/*.o && make all && make tests
for t in test_signature test_signature_store test_segment_index test_index_keymap test_index_manifest \
         test_index_tombstones test_index_merge test_vector_store test_wal test_memory_engine_ownership; do
  printf "%-30s " "$t:"; ./bin/$t >/tmp/v2_$t.out 2>&1 && tail -1 /tmp/v2_$t.out || { echo FAILED; tail -8 /tmp/v2_$t.out; }
done
make engine_lib && ( cd nodejs && npm run build:segment && npm run test:segment 2>&1 | tail -6 )
```
Expected: all C++ suites `PASSED` (now 10 binaries), JS `pass 12  fail 0`. The exact
`search_vector`/`search_hybrid` paths are byte-for-byte unchanged from pre-V2 — the
pre-existing vector tests (`test_vector_search_nrt_and_persistence`,
`test_hybrid_search_rrf`, etc.) must still pass identically.

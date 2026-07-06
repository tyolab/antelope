# Hybrid Vector Search V1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dense-vector similarity search and RRF hybrid retrieval on `ATIRE_segment_index`, per spec `docs/superpowers/specs/2026-07-06-hybrid-vector-search-design.md` — per-segment `.vec` sidecar, exact scan, optional vectors, dot/cosine/L2, full lifecycle integration.

**Architecture:** `ANT_vector_store` (`source/`) owns the `seg_G.vec` format (header + presence bitmap + dense float32 rows), validated loads that degrade to empty, an exact top-k scan with inline tombstone skip, and an atomic writer. `ATIRE_segment_index` grows: `vector.config` handling, a growable memory-segment vector buffer, three-arg `add/update_document` overloads, `search_vector` (shared candidate machinery), `search_hybrid` (RRF k=60), flush/compaction sidecar writes (compaction re-derives the merger's numbering via its own `ANT_docid_renumberer`). Vectors are caller-supplied; the engine never embeds.

**Tech Stack:** C++ house style (pre-C++11, no STL, column-0 bodies, banner comments). Build auto-globs `source/*.cpp`; `tests/<name>.cpp` → `make <name>` → `bin/<name>`. Existing harness: `CHECK`, `make_index_dir()`, `unique_term(char *buffer, long long n)` in `tests/test_segment_index.cpp` (22 functions green at plan time).

**Established facts (do not re-derive):** segment struct lives in `atire/atire_segment_index.h` (`{generation, engine, tombstones}` — this plan adds a `vectors` member; touch points: `append_segment`, dtor, `compact()` step 6 teardown, `flush()`); orphan sweep already covers any `seg_G.*` extension; docids are 0-based and stable across flush; memory-segment filenames come straight from `writer->get_doc_list(&count)[docid]` (no engine needed); disk-segment filenames via `segments[i].engine->get_document_filename(buffer, docid)`; `hit` array lifetime = until next search, filenames deep-copied; `ANT_docid_renumberer` (`source/index_merge.h`) is public and deterministic (same tombstones + order ⇒ same numbering as the merger); `compact()`'s step numbering and failure paths are documented in its banner; `search()` frees prior result filenames at entry.

**Worktree:** `.worktrees/hybrid-vector-search`, branch `feature/hybrid-vector-search`; first build `mkdir -p obj bin && make all`, then incremental.

---

### Task 1: `ANT_vector_store` — format, validated load, scan, atomic writer

**Files:**
- Create: `source/vector_store.h`
- Create: `source/vector_store.cpp`
- Test: `tests/test_vector_store.cpp`

- [ ] **Step 1: the failing test**

```cpp
/*
	TEST_VECTOR_STORE.CPP
	---------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "vector_store.h"
#include "index_tombstones.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_vecstore_XXXXXX";
char *dir = mkdtemp(dir_template);
char filename[1024], tmpname[1200];
long long which;
CHECK(dir != NULL);
sprintf(filename, "%s/seg_000001.vec", dir);

/*
	Write 4 docs, dimension 3; doc 2 has no vector
*/
float v0[3] = {1.0f, 0.0f, 0.0f};
float v1[3] = {0.0f, 1.0f, 0.0f};
float v3[3] = {0.6f, 0.8f, 0.0f};

ANT_vector_store_writer *writer = new ANT_vector_store_writer();
CHECK(writer->create(filename, 3) == 0);
CHECK(writer->append(v0) == 0);
CHECK(writer->append(v1) == 0);
CHECK(writer->append(NULL) == 0);		// doc 2: absent
CHECK(writer->append(v3) == 0);
CHECK(writer->finish() == 0);
delete writer;
sprintf(tmpname, "%s.tmp", filename);
CHECK(access(tmpname, F_OK) != 0);		// atomic: no temp survivor

/*
	Load and verify contents + presence
*/
ANT_vector_store *store = ANT_vector_store::load(filename, 3, 4);
CHECK(store != NULL);
CHECK(store->document_count() == 4);
CHECK(store->has(0));
CHECK(store->has(1));
CHECK(!store->has(2));
CHECK(store->has(3));
CHECK(store->get(0)[0] == 1.0f && store->get(0)[1] == 0.0f);
CHECK(store->get(3)[1] == 0.8f);

/*
	Top-k scan, dot metric: query {1,0,0} -> doc0 (1.0), doc3 (0.6), doc1 (0.0);
	doc2 absent must never appear
*/
float query[3] = {1.0f, 0.0f, 0.0f};
ANT_vector_candidate best[8];
long long best_count = 0;
ANT_index_tombstones *no_deletes = new ANT_index_tombstones(4);
store->scan(query, ANT_vector_store::METRIC_DOT, no_deletes, 7, best, &best_count, 3);
CHECK(best_count == 3);
/* order within best[] is unspecified (replace-min set); verify membership + scores */
long saw0 = 0, saw1 = 0, saw3 = 0;
for (which = 0; which < best_count; which++)
	{
	if (best[which].docid == 0) { saw0 = 1; CHECK(fabs(best[which].score - 1.0) < 1e-6); }
	if (best[which].docid == 1) { saw1 = 1; CHECK(fabs(best[which].score - 0.0) < 1e-6); }
	if (best[which].docid == 3) { saw3 = 1; CHECK(fabs(best[which].score - 0.6) < 1e-6); }
	CHECK(best[which].generation == 7);
	CHECK(best[which].docid != 2);
	}
CHECK(saw0 && saw1 && saw3);

/*
	Tombstone skip: delete doc 0, k=2 -> docs 3 and 1
*/
ANT_index_tombstones *dead0 = new ANT_index_tombstones(4);
dead0->set_deleted(0);
best_count = 0;
store->scan(query, ANT_vector_store::METRIC_DOT, dead0, 7, best, &best_count, 2);
CHECK(best_count == 2);
for (which = 0; which < best_count; which++)
	CHECK(best[which].docid == 1 || best[which].docid == 3);

/*
	k larger than live vector count: returns what exists
*/
best_count = 0;
store->scan(query, ANT_vector_store::METRIC_DOT, no_deletes, 7, best, &best_count, 8);
CHECK(best_count == 3);

/*
	L2 metric: query {0,1,0} -> doc1 distance 0 (score 0), doc0/-2, doc3 -(0.36+0.04)=-0.4
*/
float q2[3] = {0.0f, 1.0f, 0.0f};
best_count = 0;
store->scan(q2, ANT_vector_store::METRIC_L2, no_deletes, 7, best, &best_count, 1);
CHECK(best_count == 1);
CHECK(best[which = 0].docid == 1);
CHECK(fabs(best[0].score - 0.0) < 1e-6);

/*
	Normalization helper (cosine support): {3,4,0} -> {0.6,0.8,0}; zero vector rejected
*/
float n[3] = {3.0f, 4.0f, 0.0f};
CHECK(ANT_vector_store::normalize(n, 3) == 0);
CHECK(fabs(n[0] - 0.6f) < 1e-6 && fabs(n[1] - 0.8f) < 1e-6);
float z[3] = {0.0f, 0.0f, 0.0f};
CHECK(ANT_vector_store::normalize(z, 3) != 0);

/*
	Corruption: bad magic -> load returns degraded empty store (never NULL-crash)
*/
char corrupt_name[1024];
sprintf(corrupt_name, "%s/corrupt.vec", dir);
FILE *fp = fopen(corrupt_name, "wb");
fputs("not a vector store at all", fp);
fclose(fp);
ANT_vector_store *corrupt = ANT_vector_store::load(corrupt_name, 3, 4);
CHECK(corrupt != NULL);
CHECK(!corrupt->has(0));
best_count = 0;
corrupt->scan(query, ANT_vector_store::METRIC_DOT, no_deletes, 7, best, &best_count, 3);
CHECK(best_count == 0);

/*
	Dimension mismatch and missing file also degrade to empty
*/
ANT_vector_store *wrong_dim = ANT_vector_store::load(filename, 5, 4);
CHECK(wrong_dim != NULL && !wrong_dim->has(0));
char missing[1024];
sprintf(missing, "%s/absent.vec", dir);
ANT_vector_store *none = ANT_vector_store::load(missing, 3, 4);
CHECK(none != NULL && !none->has(0));

delete store;
delete corrupt;
delete wrong_dim;
delete none;
delete no_deletes;
delete dead0;
printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2:** `make test_vector_store` → compile FAILURE (header missing). Confirm before implementing.

- [ ] **Step 3: implement**

`source/vector_store.h`:

```cpp
/*
	VECTOR_STORE.H
	--------------
	Per-segment dense-vector sidecar (seg_G.vec) for the segmented incremental
	index (see docs/superpowers/specs/2026-07-06-hybrid-vector-search-design.md).

	On-disk format:
		uint64  magic ("ANTVEC01")
		int64   dimension
		int64   document_count
		byte[]  presence bitmap ((document_count + 7) / 8 bytes)
		float[] vectors (document_count * dimension, dense, absent rows zeroed)

	Loads validate magic/dimension/count/file size; any failure degrades to an
	empty store (no vectors) rather than failing the segment -- the same
	forgiving posture as a missing .del file.

	Score conventions: dot and cosine return the raw dot product (cosine is dot
	on pre-normalized data, normalized by the caller); L2 returns the NEGATED
	squared distance so "higher is better" holds for every metric.
*/
#ifndef VECTOR_STORE_H_
#define VECTOR_STORE_H_

class ANT_index_tombstones;

/*
	One entry in a top-k candidate set, shared by disk-store scans and the
	coordinator's memory-buffer scan.
*/
struct ANT_vector_candidate
{
double score;
long long generation;
long long docid;
} ;

/*
	ANT_VECTOR_CANDIDATE_INSERT()
	-----------------------------
	Replace-min insertion into a fixed-capacity candidate set (O(k) per call).
*/
void ANT_vector_candidate_insert(ANT_vector_candidate *best, long long *best_count, long long top_k, double score, long long generation, long long docid);

class ANT_vector_store
{
public:
	enum { METRIC_DOT = 0, METRIC_COSINE = 1, METRIC_L2 = 2 };

private:
	long long dimension;
	long long documents;
	unsigned char *presence;		// NULL when degraded/empty
	float *vectors;					// NULL when degraded/empty

private:
	ANT_vector_store();

public:
	~ANT_vector_store();

	static ANT_vector_store *load(const char *filename, long long expected_dimension, long long expected_documents);

	long long document_count(void) { return documents; }
	long has(long long docid) { return presence != NULL && (presence[docid / 8] & (1 << (docid % 8))) != 0; }
	const float *get(long long docid) { return vectors + docid * dimension; }

	void scan(const float *query, long metric, ANT_index_tombstones *tombstones, long long generation, ANT_vector_candidate *best, long long *best_count, long long top_k);

	static long normalize(float *vector, long long dimension);	// in place; nonzero if magnitude is zero
	static double kernel(const float *a, const float *b, long long dimension, long metric);
} ;

/*
	class ANT_VECTOR_STORE_WRITER
	-----------------------------
	Buffered writer with atomic finish (write .tmp, rename).
*/
class ANT_vector_store_writer
{
private:
	char filename[4096];
	long long dimension;
	long long documents;
	long long capacity;
	unsigned char *presence;
	float *vectors;

private:
	long grow(void);

public:
	ANT_vector_store_writer();
	~ANT_vector_store_writer();

	long create(const char *filename, long long dimension);		// 0 on success
	long append(const float *vector_or_null);					// 0 on success
	long finish(void);											// writes + renames; 0 on success
	void abandon(void);											// discard without writing
} ;

#endif /* VECTOR_STORE_H_ */
```

`source/vector_store.cpp`:

```cpp
/*
	VECTOR_STORE.CPP
	----------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vector_store.h"
#include "index_tombstones.h"

static const unsigned long long ANT_VECTOR_STORE_MAGIC = 0x3130434556544E41ULL;	// "ANTVEC01" little-endian

/*
	ANT_VECTOR_CANDIDATE_INSERT()
	-----------------------------
*/
void ANT_vector_candidate_insert(ANT_vector_candidate *best, long long *best_count, long long top_k, double score, long long generation, long long docid)
{
long long which, weakest;

if (*best_count < top_k)
	{
	best[*best_count].score = score;
	best[*best_count].generation = generation;
	best[*best_count].docid = docid;
	(*best_count)++;
	return;
	}
weakest = 0;
for (which = 1; which < *best_count; which++)
	if (best[which].score < best[weakest].score)
		weakest = which;
if (score > best[weakest].score)
	{
	best[weakest].score = score;
	best[weakest].generation = generation;
	best[weakest].docid = docid;
	}
}

/*
	ANT_VECTOR_STORE::ANT_VECTOR_STORE()
	------------------------------------
*/
ANT_vector_store::ANT_vector_store()
{
dimension = 0;
documents = 0;
presence = NULL;
vectors = NULL;
}

/*
	ANT_VECTOR_STORE::~ANT_VECTOR_STORE()
	-------------------------------------
*/
ANT_vector_store::~ANT_vector_store()
{
delete [] presence;
delete [] vectors;
}

/*
	ANT_VECTOR_STORE::LOAD()
	------------------------
	Any validation failure returns a degraded empty store: the segment keeps
	working lexically, it just has no vectors.
*/
ANT_vector_store *ANT_vector_store::load(const char *filename, long long expected_dimension, long long expected_documents)
{
FILE *fp;
unsigned long long magic;
long long stored_dimension, stored_documents, presence_bytes, file_size, expected_size;
ANT_vector_store *result = new ANT_vector_store();

if ((fp = fopen(filename, "rb")) == NULL)
	return result;

if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != ANT_VECTOR_STORE_MAGIC
	|| fread(&stored_dimension, sizeof(stored_dimension), 1, fp) != 1
	|| fread(&stored_documents, sizeof(stored_documents), 1, fp) != 1
	|| stored_dimension != expected_dimension
	|| stored_documents != expected_documents
	|| stored_dimension < 1 || stored_dimension > 65536
	|| stored_documents < 0 || stored_documents > (1LL << 40))
	{
	fclose(fp);
	return result;
	}

presence_bytes = (stored_documents + 7) / 8;

/*
	The header's counts drive the allocations below, so a corrupt file lying
	about them could trigger an absurd new[] and abort the process.  Verify
	the actual file size matches exactly what the writer would have produced
	before trusting the header.
*/
expected_size = 24 + presence_bytes + stored_documents * stored_dimension * (long long)sizeof(float);
if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) != expected_size || fseek(fp, 24, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

unsigned char *presence_buffer = new unsigned char[presence_bytes > 0 ? presence_bytes : 1];
float *vector_buffer = new float[stored_documents * stored_dimension > 0 ? stored_documents * stored_dimension : 1];
if (fread(presence_buffer, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
	|| fread(vector_buffer, sizeof(float), (size_t)(stored_documents * stored_dimension), fp) != (size_t)(stored_documents * stored_dimension))
	{
	delete [] presence_buffer;
	delete [] vector_buffer;
	fclose(fp);
	return result;
	}
fclose(fp);

result->dimension = stored_dimension;
result->documents = stored_documents;
result->presence = presence_buffer;
result->vectors = vector_buffer;
return result;
}

/*
	ANT_VECTOR_STORE::KERNEL()
	--------------------------
*/
double ANT_vector_store::kernel(const float *a, const float *b, long long dimension, long metric)
{
long long which;
double sum = 0.0;

if (metric == METRIC_L2)
	{
	for (which = 0; which < dimension; which++)
		{
		double difference = (double)a[which] - (double)b[which];
		sum += difference * difference;
		}
	return -sum;
	}
for (which = 0; which < dimension; which++)
	sum += (double)a[which] * (double)b[which];
return sum;
}

/*
	ANT_VECTOR_STORE::SCAN()
	------------------------
	Exhaustive scan of present, non-tombstoned documents; tombstones filtered
	inline so no over-fetch is needed on the vector side.
*/
void ANT_vector_store::scan(const float *query, long metric, ANT_index_tombstones *tombstones, long long generation, ANT_vector_candidate *best, long long *best_count, long long top_k)
{
long long docid;

if (presence == NULL || vectors == NULL)
	return;
for (docid = 0; docid < documents; docid++)
	{
	if (!has(docid))
		continue;
	if (tombstones != NULL && tombstones->is_deleted(docid))
		continue;
	ANT_vector_candidate_insert(best, best_count, top_k, kernel(query, vectors + docid * dimension, dimension, metric), generation, docid);
	}
}

/*
	ANT_VECTOR_STORE::NORMALIZE()
	-----------------------------
*/
long ANT_vector_store::normalize(float *vector, long long dimension)
{
long long which;
double sum = 0.0;

for (which = 0; which < dimension; which++)
	sum += (double)vector[which] * (double)vector[which];
if (sum <= 0.0)
	return 1;
double scale = 1.0 / sqrt(sum);
for (which = 0; which < dimension; which++)
	vector[which] = (float)(vector[which] * scale);
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::ANT_VECTOR_STORE_WRITER()
	--------------------------------------------------
*/
ANT_vector_store_writer::ANT_vector_store_writer()
{
filename[0] = '\0';
dimension = 0;
documents = 0;
capacity = 0;
presence = NULL;
vectors = NULL;
}

/*
	ANT_VECTOR_STORE_WRITER::~ANT_VECTOR_STORE_WRITER()
	---------------------------------------------------
*/
ANT_vector_store_writer::~ANT_vector_store_writer()
{
delete [] presence;
delete [] vectors;
}

/*
	ANT_VECTOR_STORE_WRITER::CREATE()
	---------------------------------
	Resets any prior state, so a writer may be reused across create() calls.
*/
long ANT_vector_store_writer::create(const char *name, long long width)
{
if (width < 1 || width > 65536)
	return 1;
if (snprintf(filename, sizeof(filename), "%s", name) >= (int)sizeof(filename))
	return 1;
delete [] presence;
delete [] vectors;
dimension = width;
documents = 0;
capacity = dimension > 4096 ? 64 : 1024;		// keep the up-front buffer modest for very wide vectors
presence = new unsigned char[(capacity + 7) / 8];
memset(presence, 0, (size_t)((capacity + 7) / 8));
vectors = new float[capacity * dimension];
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::GROW()
	-------------------------------
*/
long ANT_vector_store_writer::grow(void)
{
long long new_capacity = capacity * 2;
unsigned char *new_presence = new unsigned char[(new_capacity + 7) / 8];
float *new_vectors = new float[new_capacity * dimension];

memset(new_presence, 0, (size_t)((new_capacity + 7) / 8));
memcpy(new_presence, presence, (size_t)((capacity + 7) / 8));
memcpy(new_vectors, vectors, (size_t)(documents * dimension * sizeof(float)));
delete [] presence;
delete [] vectors;
presence = new_presence;
vectors = new_vectors;
capacity = new_capacity;
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::APPEND()
	---------------------------------
*/
long ANT_vector_store_writer::append(const float *vector_or_null)
{
if (vectors == NULL)
	return 1;
if (documents >= capacity)
	grow();
if (vector_or_null == NULL)
	memset(vectors + documents * dimension, 0, (size_t)(dimension * sizeof(float)));
else
	{
	memcpy(vectors + documents * dimension, vector_or_null, (size_t)(dimension * sizeof(float)));
	presence[documents / 8] |= (unsigned char)(1 << (documents % 8));
	}
documents++;
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::FINISH()
	---------------------------------
	Write-temp then rename, per the crash-safety convention.
*/
long ANT_vector_store_writer::finish(void)
{
char temp_name[4200];
FILE *fp;
long long presence_bytes = (documents + 7) / 8;

if (vectors == NULL)
	return 1;
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
if (fwrite(&ANT_VECTOR_STORE_MAGIC, sizeof(ANT_VECTOR_STORE_MAGIC), 1, fp) != 1
	|| fwrite(&dimension, sizeof(dimension), 1, fp) != 1
	|| fwrite(&documents, sizeof(documents), 1, fp) != 1
	|| fwrite(presence, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
	|| fwrite(vectors, sizeof(float), (size_t)(documents * dimension), fp) != (size_t)(documents * dimension))
	{
	fclose(fp);
	remove(temp_name);
	return 1;
	}
fclose(fp);
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::ABANDON()
	----------------------------------
*/
void ANT_vector_store_writer::abandon(void)
{
delete [] presence;
delete [] vectors;
presence = NULL;
vectors = NULL;
documents = 0;
}
```

Note: `fwrite(&ANT_VECTOR_STORE_MAGIC, ...)` on a `static const` at namespace scope is fine; if the compiler objects to taking its address, hoist into a local `unsigned long long magic = ANT_VECTOR_STORE_MAGIC;`.

- [ ] **Step 4:** `make test_vector_store && ./bin/test_vector_store` → PASSED. Existing six binaries still PASSED; `make internal` exit 0.

- [ ] **Step 5: Commit** — `feat: per-segment vector store with validated load and atomic writer`

---

### Task 2: vector.config + API overloads + memory-segment vector buffer

**Files:**
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp` (append `test_vector_config_and_add`)

- [ ] **Step 1: the failing test — append, call from `main()`:**

```cpp
#include "../source/vector_store.h"

/*
	TEST_VECTOOR_CONFIG_AND_ADD()  -- fix the typo when typing: TEST_VECTOR_CONFIG_AND_ADD
	--------------------------------
*/
static void test_vector_config_and_add(void)
{
char *dir = make_index_dir();
float v[4] = {1.0f, 0.0f, 0.0f, 0.0f};

/*
	Enable vectors on a fresh index
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(index->open(dir) == 0);
CHECK(index->vector_dimension() == 4);

CHECK(index->add_document("doc-1", "<DOC>aardvark</DOC>", v) >= 0);
CHECK(index->add_document("doc-2", "<DOC>zebra</DOC>", NULL) >= 0);		// lexical-only doc
char query[64];
strcpy(query, "aardvark");
CHECK(index->search(query, 10) == 1);		// lexical search unchanged
delete index;

/*
	Reopen without set_vector_config: config is read from disk
*/
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(reopened->vector_dimension() == 4);
delete reopened;

/*
	Mismatched set_vector_config on an existing index fails open
*/
ATIRE_segment_index *mismatch = new ATIRE_segment_index();
CHECK(mismatch->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(mismatch->open(dir) != 0);
delete mismatch;

/*
	Vectors on a non-enabled index are rejected; plain index unaffected
*/
char *plain_dir = make_index_dir();
ATIRE_segment_index *plain = new ATIRE_segment_index();
CHECK(plain->open(plain_dir) == 0);
CHECK(plain->vector_dimension() == 0);
CHECK(plain->add_document("doc-1", "<DOC>aardvark</DOC>", v) == -1);
CHECK(plain->add_document("doc-1", "<DOC>aardvark</DOC>") >= 0);
delete plain;

/*
	set_vector_config validation
*/
ATIRE_segment_index *bad = new ATIRE_segment_index();
CHECK(bad->set_vector_config(0, ATIRE_segment_index::VECTOR_METRIC_DOT) != 0);
CHECK(bad->set_vector_config(4, 99) != 0);
delete bad;

delete [] dir;
delete [] plain_dir;
printf("test_vector_config_and_add OK\n");
}
```

- [ ] **Step 2:** run → compile FAILURE (methods undeclared).

- [ ] **Step 3: implement.** Header additions (`atire/atire_segment_index.h`):

```cpp
public:
	enum { VECTOR_METRIC_DOT = 0, VECTOR_METRIC_COSINE = 1, VECTOR_METRIC_L2 = 2 };

	long set_vector_config(long long dimension, long metric);		// before open(); 0 on success
	long long vector_dimension(void) { return vector_dimension_current; }

	long long add_document(const char *key, const char *document, const float *vector);
	long long update_document(const char *key, const char *document, const float *vector);

private:
	long long vector_dimension_current;		// 0 = vectors disabled
	long vector_metric;
	long long pending_vector_dimension;		// set_vector_config before open
	long pending_vector_metric;
	long vector_config_pending;

	/* memory-segment vector buffer, parallel to the writer's docids */
	float *writer_vector_data;
	unsigned char *writer_vector_presence;
	long long writer_vector_capacity;
	long long writer_vectors_present;		// how many docs in the buffer HAVE vectors

	long load_vector_config(void);			// reads <dir>/vector.config; 0 = ok (absent is ok)
	long save_vector_config(void);			// atomic write; 0 on success
	long writer_vector_append(const float *vector_or_null);
	void reset_writer_vectors(void);
```

Implementation (`atire/atire_segment_index.cpp`; include `../source/vector_store.h`):

```cpp
/*
	ATIRE_SEGMENT_INDEX::SET_VECTOR_CONFIG()
	----------------------------------------
	Must be called before open().  The configuration is fixed for the life of
	the index; open() writes it to <dir>/vector.config on first use and
	verifies it against an existing file on every later use.
*/
long ATIRE_segment_index::set_vector_config(long long dimension, long metric)
{
if (directory != NULL)
	return 1;			// already open
if (dimension < 1 || dimension > 65536)
	return 1;
if (metric != VECTOR_METRIC_DOT && metric != VECTOR_METRIC_COSINE && metric != VECTOR_METRIC_L2)
	return 1;
pending_vector_dimension = dimension;
pending_vector_metric = metric;
vector_config_pending = 1;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_VECTOR_CONFIG()
	-----------------------------------------
	Reads <dir>/vector.config (two lines: dimension, metric).  Absent file
	leaves vectors disabled.  Garbage is treated as absent (defensive parsing
	per house convention).
*/
long ATIRE_segment_index::load_vector_config(void)
{
char filename[4096], line[64];
FILE *fp;
long long dimension = 0;
long metric = -1;

snprintf(filename, sizeof(filename), "%s/vector.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fgets(line, sizeof(line), fp) != NULL)
	dimension = atoll(line);
if (fgets(line, sizeof(line), fp) != NULL)
	metric = atol(line);
fclose(fp);
if (dimension < 1 || dimension > 65536 || metric < VECTOR_METRIC_DOT || metric > VECTOR_METRIC_L2)
	return 0;			// corrupt: treat as absent
vector_dimension_current = dimension;
vector_metric = metric;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_VECTOR_CONFIG()
	-----------------------------------------
*/
long ATIRE_segment_index::save_vector_config(void)
{
char filename[4096], temp_name[4200];
FILE *fp;

snprintf(filename, sizeof(filename), "%s/vector.config", directory);
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
fprintf(fp, "%lld\n%ld\n", vector_dimension_current, vector_metric);
fclose(fp);
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}
```

Wire into `open()`, immediately after `directory` is copied and before the manifest load:

```cpp
if (load_vector_config() != 0)
	return 1;
if (vector_config_pending)
	{
	if (vector_dimension_current != 0)
		{
		/*
			An existing config must agree with the caller's -- silent
			dimension/metric mixups corrupt results, so fail loudly.
		*/
		if (vector_dimension_current != pending_vector_dimension || vector_metric != pending_vector_metric)
			return 1;
		}
	else
		{
		vector_dimension_current = pending_vector_dimension;
		vector_metric = pending_vector_metric;
		if (save_vector_config() != 0)
			return 1;
		}
	}
```

Memory buffer plumbing:

```cpp
/*
	ATIRE_SEGMENT_INDEX::WRITER_VECTOR_APPEND()
	-------------------------------------------
	Keeps the vector buffer parallel to the writer's docids: called exactly
	once per successfully indexed document (NULL for lexical-only docs).
	In cosine mode the stored copy is unit-normalized (spec section 2.2).
*/
long ATIRE_segment_index::writer_vector_append(const float *vector_or_null)
{
long long docid = writer_documents;		// the docid the document just received (0-based, pre-increment caller side -- see call site)

if (writer_vector_capacity == 0)
	{
	writer_vector_capacity = 1024;
	writer_vector_data = new float[writer_vector_capacity * vector_dimension_current];
	writer_vector_presence = new unsigned char[(writer_vector_capacity + 7) / 8];
	memset(writer_vector_presence, 0, (size_t)((writer_vector_capacity + 7) / 8));
	}
if (docid >= writer_vector_capacity)
	{
	long long new_capacity = writer_vector_capacity * 2;
	float *new_data = new float[new_capacity * vector_dimension_current];
	unsigned char *new_presence = new unsigned char[(new_capacity + 7) / 8];
	memset(new_presence, 0, (size_t)((new_capacity + 7) / 8));
	memcpy(new_data, writer_vector_data, (size_t)(writer_vector_capacity * vector_dimension_current * sizeof(float)));
	memcpy(new_presence, writer_vector_presence, (size_t)((writer_vector_capacity + 7) / 8));
	delete [] writer_vector_data;
	delete [] writer_vector_presence;
	writer_vector_data = new_data;
	writer_vector_presence = new_presence;
	writer_vector_capacity = new_capacity;
	}
if (vector_or_null == NULL)
	memset(writer_vector_data + docid * vector_dimension_current, 0, (size_t)(vector_dimension_current * sizeof(float)));
else
	{
	memcpy(writer_vector_data + docid * vector_dimension_current, vector_or_null, (size_t)(vector_dimension_current * sizeof(float)));
	writer_vector_presence[docid / 8] |= (unsigned char)(1 << (docid % 8));
	writer_vectors_present++;
	}
return 0;
}
```

The three-arg overloads:

```cpp
/*
	ATIRE_SEGMENT_INDEX::ADD_DOCUMENT()  (vector overload)
	------------------------------------------------------
*/
long long ATIRE_segment_index::add_document(const char *key, const char *document, const float *vector)
{
float *normalized = NULL;
long long handle;

if (vector != NULL && vector_dimension_current == 0)
	return -1;			// vectors on a non-vector index
if (vector != NULL && vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, vector, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{
		delete [] normalized;
		return -1;		// zero vector is meaningless under cosine
		}
	vector = normalized;
	}

handle = add_document(key, document);		// existing two-arg path: all guards, docno, keymap, auto-flush
if (handle >= 0 && vector_dimension_current != 0)
	writer_vector_append(vector);			// NULL appends an absent row
delete [] normalized;
return handle;
}
```

CRITICAL ordering trap the implementer must solve when wiring this: the two-arg `add_document` may AUTO-FLUSH after the add (handle computed pre-flush). The vector append must attach to the document's docid IN ITS SEGMENT — if the auto-flush fired, the doc was flushed WITHOUT its vector. Fix: the docid/buffer bookkeeping must happen before the flush check. Cleanest: refactor the two-arg `add_document` core into a private `add_document_core(key, document, vector)` that does index → docid → `writer_vector_append(vector)` (when enabled) → keymap → handle → auto-flush-check → return, and make BOTH public overloads one-line wrappers (two-arg passes NULL). This keeps one code path and fixes the ordering by construction. `writer_vector_append`'s `docid = writer_documents` line must then read the docid BEFORE `writer_documents++` or receive it as a parameter — pass it as a parameter (`writer_vector_append(docid, vector)`) to remove the coupling. Adjust the signature accordingly.

`update_document(key, document, vector)`: same body as the existing update but calling the three-arg add:

```cpp
long long ATIRE_segment_index::update_document(const char *key, const char *document, const float *vector)
{
long long old_generation, old_docid;
long had_old = keymap->find(key, &old_generation, &old_docid);
long long handle = add_document(key, document, vector);
if (handle < 0)
	return -1;
if (had_old)
	tombstone(old_generation, old_docid);
return handle;
}
```
(Refactor the existing two-arg `update_document` to call this with `vector = NULL` — one code path.)

`reset_writer_vectors()`: frees buffer + zeroes the five members; call it from `start_new_writer()` (fresh segment = fresh buffer) and the destructor.

Constructor: zero-init all seven new members.

- [ ] **Step 4:** all tests green (`test_segment_index` now 23 functions), `make internal` exit 0.

- [ ] **Step 5: Commit** — `feat: vector config, memory-segment vector buffer, add/update vector overloads`

---

### Task 3: `search_vector` + flush persistence + disk-store loading

**Files:**
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp` (append `test_vector_search_nrt_and_persistence`)

- [ ] **Step 1: the failing test — append, call from `main()`:**

```cpp
/*
	TEST_VECTOR_SEARCH_NRT_AND_PERSISTENCE()
	----------------------------------------
*/
static void test_vector_search_nrt_and_persistence(void)
{
char *dir = make_index_dir();
float va[4] = {1.0f, 0.0f, 0.0f, 0.0f};
float vb[4] = {0.9f, 0.1f, 0.0f, 0.0f};
float vc[4] = {0.0f, 1.0f, 0.0f, 0.0f};
float query[4] = {1.0f, 0.0f, 0.0f, 0.0f};

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(index->open(dir) == 0);
CHECK(index->add_document("doc-a", "<DOC>alpha content</DOC>", va) >= 0);
CHECK(index->add_document("doc-b", "<DOC>beta content</DOC>", vb) >= 0);
CHECK(index->add_document("doc-c", "<DOC>gamma content</DOC>", vc) >= 0);
CHECK(index->add_document("doc-d", "<DOC>delta lexical only</DOC>") >= 0);

/*
	NRT: searchable before any flush; ranked by similarity; lexical-only
	doc-d absent
*/
CHECK(index->search_vector(query, 3) == 3);
CHECK(strcmp(index->get_hit(0)->filename, "doc-a") == 0);
CHECK(strcmp(index->get_hit(1)->filename, "doc-b") == 0);
CHECK(strcmp(index->get_hit(2)->filename, "doc-c") == 0);
CHECK(index->get_hit(0)->score > index->get_hit(1)->score);
CHECK(index->get_hit(1)->score > index->get_hit(2)->score);

/*
	Delete removes from vector results immediately; update replaces the vector
*/
CHECK(index->delete_document("doc-c") == 0);
CHECK(index->search_vector(query, 10) == 2);
float vb2[4] = {0.0f, 0.0f, 1.0f, 0.0f};
CHECK(index->update_document("doc-b", "<DOC>beta revised</DOC>", vb2) >= 0);
CHECK(index->search_vector(query, 10) == 2);
CHECK(strcmp(index->get_hit(0)->filename, "doc-a") == 0);	// doc-b now orthogonal, ranks below
CHECK(index->get_hit(1)->score < 0.5);

/*
	Flush + same-session search spans disk store + fresh memory buffer
*/
CHECK(index->flush() == 0);
CHECK(index->search_vector(query, 10) == 2);
CHECK(strcmp(index->get_hit(0)->filename, "doc-a") == 0);
float ve[4] = {0.95f, 0.0f, 0.0f, 0.0f};
CHECK(index->add_document("doc-e", "<DOC>epsilon</DOC>", ve) >= 0);
CHECK(index->search_vector(query, 10) == 3);
CHECK(strcmp(index->get_hit(0)->filename, "doc-a") == 0);
CHECK(strcmp(index->get_hit(1)->filename, "doc-e") == 0);
delete index;			// doc-e unflushed: lost (relaxed durability)

/*
	Reopen: vectors persisted
*/
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(reopened->vector_dimension() == 4);
CHECK(reopened->search_vector(query, 10) == 2);
CHECK(strcmp(reopened->get_hit(0)->filename, "doc-a") == 0);

/*
	search_vector on a vector-less index / NULL query
*/
CHECK(reopened->search_vector(NULL, 10) == 0);
delete reopened;
delete [] dir;
printf("test_vector_search_nrt_and_persistence OK\n");
}
```

- [ ] **Step 2:** run → compile FAILURE (`search_vector` undeclared).

- [ ] **Step 3: implement.**

(a) Extend `struct segment` with `ANT_vector_store *vectors;`. Update EVERY site that builds or tears down a segment: `append_segment` (load `seg_G.vec` via `ANT_vector_store::load(vec_name, vector_dimension_current, engine->get_document_count())` when `vector_dimension_current != 0`, else NULL), the destructor, and `compact()` step 6 (delete `segments[which].vectors` beside engine/tombstones).

(b) Private candidate collector + filename resolution, public search:

```cpp
private:
	long long vector_candidates(const float *query, long long top_k, ANT_vector_candidate *best);
	char *resolve_hit_filename(long long generation, long long docid, char *buffer, long long buffer_size);
public:
	long long search_vector(const float *query, long long top_k);
```

```cpp
/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES()
	----------------------------------------
	Exact top-k across every disk store and the live memory buffer.  In cosine
	mode the query is normalized here (stored vectors already are).  Returns
	the candidate count; caller supplies best[top_k].
*/
long long ATIRE_segment_index::vector_candidates(const float *query, long long top_k, ANT_vector_candidate *best)
{
long long which, docid, best_count = 0;
float *normalized = NULL;

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;
if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{
		delete [] normalized;
		return 0;
		}
	query = normalized;
	}

for (which = 0; which < segment_count; which++)
	if (segments[which].vectors != NULL)
		segments[which].vectors->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k);

for (docid = 0; docid < writer_documents; docid++)
	{
	if (writer_vector_presence == NULL || !(writer_vector_presence[docid / 8] & (1 << (docid % 8))))
		continue;
	if (writer_tombstones->is_deleted(docid))
		continue;
	ANT_vector_candidate_insert(best, &best_count, top_k, ANT_vector_store::kernel(query, writer_vector_data + docid * vector_dimension_current, vector_dimension_current, vector_metric), writer_generation, docid);
	}

delete [] normalized;
return best_count;
}

/*
	ATIRE_SEGMENT_INDEX::RESOLVE_HIT_FILENAME()
	-------------------------------------------
	Memory-segment docs resolve through the writer's doc list; disk segments
	through the engine's filename index.
*/
char *ATIRE_segment_index::resolve_hit_filename(long long generation, long long docid, char *buffer, long long buffer_size)
{
long long which, count;

if (writer != NULL && generation == writer_generation)
	{
	char **doc_list = writer->get_doc_list(&count);
	if (docid < count && doc_list[docid] != NULL)
		{
		snprintf(buffer, (size_t)buffer_size, "%s", doc_list[docid]);
		return buffer;
		}
	return NULL;
	}
for (which = 0; which < segment_count; which++)
	if (segments[which].generation == generation)
		return segments[which].engine->get_document_filename(buffer, docid);
return NULL;
}

/*
	VECTOR_CANDIDATE_COMPARE()
	--------------------------
	qsort: score descending, ties (generation, docid) ascending.
*/
static int vector_candidate_compare(const void *a, const void *b)
{
const ANT_vector_candidate *one = (const ANT_vector_candidate *)a;
const ANT_vector_candidate *two = (const ANT_vector_candidate *)b;

if (one->score > two->score)
	return -1;
if (one->score < two->score)
	return 1;
if (one->generation != two->generation)
	return one->generation < two->generation ? -1 : 1;
return one->docid < two->docid ? -1 : (one->docid == two->docid ? 0 : 1);
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_VECTOR()
	------------------------------------
*/
long long ATIRE_segment_index::search_vector(const float *query, long long top_k)
{
char filename_buffer[4096];
long long which, count;

/* free the previous results' filenames, per the shared results contract */
for (which = 0; which < results_count; which++)
	delete [] results[which].filename;
results_count = 0;

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;

ANT_vector_candidate *best = new ANT_vector_candidate[top_k];
count = vector_candidates(query, top_k, best);
qsort(best, (size_t)count, sizeof(*best), vector_candidate_compare);

for (which = 0; which < count; which++)
	{
	char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));
	/* grow results[] by doubling, as search() does */
	if (results_count >= results_allocated)
		{
		long long bigger_size = results_allocated == 0 ? 256 : results_allocated * 2;
		hit *bigger = new hit[bigger_size];
		memcpy(bigger, results, (size_t)(results_count * sizeof(*results)));
		delete [] results;
		results = bigger;
		results_allocated = bigger_size;
		}
	results[results_count].generation = best[which].generation;
	results[results_count].docid = best[which].docid;
	results[results_count].score = best[which].score;
	if (filename != NULL)
		{
		results[results_count].filename = new char[strlen(filename) + 1];
		strcpy(results[results_count].filename, filename);
		}
	else
		{
		results[results_count].filename = new char[1];
		results[results_count].filename[0] = '\0';
		}
	results_count++;
	}
delete [] best;
return results_count;
}
```

(Match the actual member names for `results`/`results_count`/`results_allocated` and the existing free-loop/grow code in `search()` — reuse a private helper if `search()` already factors it, else mirror it.)

(c) Flush persistence — in `flush()`, right after `writer->finish()` succeeds and BEFORE the tombstone-save step:

```cpp
/*
	Persist the memory segment's vectors alongside its postings (only when
	vectors are enabled and at least one document in this segment has one).
	Same crash contract as every other segment file: written fully before the
	manifest references the generation.
*/
if (vector_dimension_current != 0 && writer_vectors_present > 0)
	{
	char vec_name[4096];
	segment_filename(vec_name, sizeof(vec_name), flushed_generation_variable, "vec");
	ANT_vector_store_writer vec_writer;
	long vec_failed = vec_writer.create(vec_name, vector_dimension_current) != 0;
	for (long long docid = 0; !vec_failed && docid < flushed_document_count; docid++)
		{
		const float *row = (writer_vector_presence[docid / 8] & (1 << (docid % 8))) ? writer_vector_data + docid * vector_dimension_current : NULL;
		vec_failed = vec_writer.append(row) != 0;
		}
	if (!vec_failed)
		vec_failed = vec_writer.finish() != 0;
	if (vec_failed)
		return 1;		/* pre-manifest failure: degraded per flush()'s existing contract */
	}
```
Adapt the variable names to flush()'s actual locals (the generation variable and the pre-teardown document count — capture `writer_documents` into a local BEFORE the writer teardown zeroes it; check the current code's order). This block must run while `writer_vector_data` is still alive — i.e. before `reset_writer_vectors()`, which should be called at the same point the writer itself is torn down.

- [ ] **Step 4:** all green (`test_segment_index` 24 functions), six binaries, `make internal`.

- [ ] **Step 5: Commit** — `feat: exact vector search across memory buffer and persisted segment stores`

---

### Task 4: compaction rewrites the vector sidecar + vector equivalence

**Files:**
- Modify: `atire/atire_segment_index.cpp` (`compact()`)
- Test: `tests/test_segment_index.cpp` (append `test_vector_compaction_equivalence`)

- [ ] **Step 1: the failing test — append, call from `main()`:**

```cpp
/*
	TEST_VECTOR_COMPACTION_EQUIVALENCE()
	------------------------------------
	After a messy history + maintain(), vector search results (keys and
	scores) must equal a one-shot index of the surviving collection.
*/
static void test_vector_compaction_equivalence(void)
{
char *dir_messy = make_index_dir();
char *dir_oneshot = make_index_dir();
char key[64], doc[256], letters[16];
long long i, which;
float vecs[12][4];
float query[4] = {0.7f, 0.7f, 0.1f, 0.0f};

for (i = 0; i < 12; i++)
	{
	vecs[i][0] = (float)(i + 1) / 12.0f;
	vecs[i][1] = 1.0f - (float)i / 12.0f;
	vecs[i][2] = (float)(i % 3) / 3.0f;
	vecs[i][3] = 0.0f;
	}

ATIRE_segment_index *messy = new ATIRE_segment_index();
CHECK(messy->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(messy->open(dir_messy) == 0);
messy->set_flush_threshold(4);
messy->set_merge_factor(2);
for (i = 0; i < 12; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(messy->add_document(key, doc, vecs[i]) >= 0);
	}
/* update doc-2's vector, delete docs 9-11 */
float revised[4] = {0.0f, 0.0f, 0.0f, 1.0f};
unique_term(letters, 2);
sprintf(doc, "<DOC>common revised %s</DOC>", letters);
CHECK(messy->update_document("doc-2", doc, revised) >= 0);
for (i = 9; i < 12; i++)
	{
	sprintf(key, "doc-%lld", i);
	CHECK(messy->delete_document(key) == 0);
	}
CHECK(messy->flush() == 0);
CHECK(messy->maintain() == 0);
CHECK(messy->maintain() == 0);
CHECK(messy->disk_segment_count() == 1);

ATIRE_segment_index *oneshot = new ATIRE_segment_index();
CHECK(oneshot->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(oneshot->open(dir_oneshot) == 0);
for (i = 0; i < 9; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	if (i == 2)
		{
		sprintf(doc, "<DOC>common revised %s</DOC>", letters);
		CHECK(oneshot->add_document(key, doc, revised) >= 0);
		}
	else
		{
		sprintf(doc, "<DOC>common %s</DOC>", letters);
		CHECK(oneshot->add_document(key, doc, vecs[i]) >= 0);
		}
	}
CHECK(oneshot->flush() == 0);

/*
	Same result sets: keys AND scores, top-9 (everything live)
*/
long long messy_hits = messy->search_vector(query, 9);
long long oneshot_hits = oneshot->search_vector(query, 9);
CHECK(messy_hits == 9);
CHECK(messy_hits == oneshot_hits);
for (which = 0; which < messy_hits; which++)
	{
	CHECK(strcmp(messy->get_hit(which)->filename, oneshot->get_hit(which)->filename) == 0);
	CHECK(fabs(messy->get_hit(which)->score - oneshot->get_hit(which)->score) < 1e-6);
	}

delete messy;
delete oneshot;
delete [] dir_messy;
delete [] dir_oneshot;
printf("test_vector_compaction_equivalence OK\n");
}
```

Add `#include <math.h>` to the test file if absent.

- [ ] **Step 2:** run → FAIL (compaction drops vectors: messy side returns fewer/no vector hits after maintain()).

- [ ] **Step 3: implement.** In `compact()`, immediately after the merger succeeds (step 2) and BEFORE the marker (step 3):

```cpp
/*
	Step 2b: rewrite the vector sidecar for the merged output.  The
	renumbering below is byte-identical to the merger's: both are built from
	the same tombstones in the same input order (ANT_docid_renumberer is
	deterministic).  Inputs without vectors contribute absent rows.  A .vec
	failure aborts the compaction pre-marker, leaving the index untouched
	(the output .aspt is removed like any pre-step-3 failure).
*/
if (vector_dimension_current != 0)
	{
	long any_vectors = false;
	for (input = 0; input < input_count; input++)
		if (inputs[input]->vectors != NULL && inputs[input]->vectors->document_count() > 0)
			any_vectors = true;
	if (any_vectors)
		{
		ANT_index_tombstones **stone_list = new ANT_index_tombstones *[input_count];
		long long *doc_counts = new long long[input_count];
		for (input = 0; input < input_count; input++)
			{
			stone_list[input] = inputs[input]->tombstones;
			doc_counts[input] = inputs[input]->engine->get_document_count();
			}
		ANT_docid_renumberer *vec_renumberer = new ANT_docid_renumberer(stone_list, doc_counts, input_count);
		char vec_name[4096];
		segment_filename(vec_name, sizeof(vec_name), output_generation, "vec");
		ANT_vector_store_writer vec_writer;
		long vec_failed = vec_writer.create(vec_name, vector_dimension_current) != 0;
		for (input = 0; !vec_failed && input < input_count; input++)
			for (long long docid = 0; !vec_failed && docid < doc_counts[input]; docid++)
				{
				if (vec_renumberer->renumber(input, docid) < 0)
					continue;		/* tombstoned: dropped, exactly like its postings */
				const float *row = (inputs[input]->vectors != NULL && inputs[input]->vectors->has(docid)) ? inputs[input]->vectors->get(docid) : NULL;
				vec_failed = vec_writer.append(row) != 0;
				}
		if (!vec_failed)
			vec_failed = vec_writer.finish() != 0;
		delete vec_renumberer;
		delete [] stone_list;
		delete [] doc_counts;
		if (vec_failed)
			{
			remove(output_name);
			delete [] inputs;
			return 1;
			}
		}
	}
```

Notes: `inputs[]` pointers are still valid here (pre-reshuffle). Adapt local names (`output_name`, `output_generation`, `input`) to compact()'s actual locals. Requires `index_merge.h` (already included since Phase 2) and `vector_store.h`. Also confirm `append_segment(output_generation)` (step 4) now loads the freshly written `.vec` — it does if Task 3's (a) wiring is in place, because it runs after this block.

- [ ] **Step 4:** all green (`test_segment_index` 25 functions), six binaries, `make internal`.

- [ ] **Step 5: Commit** — `feat: compaction rewrites the vector sidecar under the merger's renumbering`

---

### Task 5: `search_hybrid` — RRF fusion

**Files:**
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp` (append `test_hybrid_search_rrf`)

- [ ] **Step 1: the failing test — append, call from `main()`:**

```cpp
/*
	TEST_HYBRID_SEARCH_RRF()
	------------------------
	A document matching BOTH the keyword and the vector side must outrank
	documents matching only one side; each side alone degrades cleanly.
*/
static void test_hybrid_search_rrf(void)
{
char *dir = make_index_dir();
float both[4] = {1.0f, 0.0f, 0.0f, 0.0f};		// matches query vector strongly
float vec_only[4] = {0.99f, 0.1f, 0.0f, 0.0f};	// nearly as strong
float weak[4] = {0.0f, 0.0f, 1.0f, 0.0f};		// orthogonal
float query_vec[4] = {1.0f, 0.0f, 0.0f, 0.0f};
char query_text[64];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(index->open(dir) == 0);
CHECK(index->add_document("doc-both", "<DOC>quokka wombat</DOC>", both) >= 0);
CHECK(index->add_document("doc-text", "<DOC>quokka numbat</DOC>", weak) >= 0);
CHECK(index->add_document("doc-vec", "<DOC>unrelated words</DOC>", vec_only) >= 0);

/*
	Keyword "quokka" matches doc-both + doc-text; vector matches doc-both +
	doc-vec strongly.  doc-both is in both lists -> highest fused score.
*/
strcpy(query_text, "quokka");
long long hits = index->search_hybrid(query_text, query_vec, 3);
CHECK(hits == 3);
CHECK(strcmp(index->get_hit(0)->filename, "doc-both") == 0);
CHECK(index->get_hit(0)->score > index->get_hit(1)->score);

/*
	Degradation both ways
*/
strcpy(query_text, "quokka");
CHECK(index->search_hybrid(query_text, NULL, 3) == 2);			// pure lexical
CHECK(index->search_hybrid(NULL, query_vec, 3) == 3);			// pure vector
strcpy(query_text, "quokka");

/*
	Tombstones respected through fusion
*/
CHECK(index->delete_document("doc-both") == 0);
strcpy(query_text, "quokka");
hits = index->search_hybrid(query_text, query_vec, 3);
CHECK(hits == 2);
for (long long which = 0; which < hits; which++)
	CHECK(strcmp(index->get_hit(which)->filename, "doc-both") != 0);

delete index;
delete [] dir;
printf("test_hybrid_search_rrf OK\n");
}
```

- [ ] **Step 2:** run → compile FAILURE.

- [ ] **Step 3: implement.** Header: `long long search_hybrid(char *query_text, const float *query_vector, long long top_k);`

```cpp
/*
	ATIRE_SEGMENT_INDEX::SEARCH_HYBRID()
	------------------------------------
	Reciprocal Rank Fusion of the lexical top-k and the vector top-k:
	fused(d) = sum over lists containing d of 1 / (60 + rank_d), ranks
	1-based.  60 is the standard RRF constant.  Either side may be absent;
	the result degrades to the other side (still RRF-scored, order preserved).
*/
long long ATIRE_segment_index::search_hybrid(char *query_text, const float *query_vector, long long top_k)
{
long long lexical_count = 0, vector_count = 0, fused_count = 0, which, other;
char filename_buffer[4096];

if (top_k < 1)
	return 0;

/*
	Lexical side first: run the existing search and snapshot its hits (the
	results array is shared, so the snapshot must deep-copy the filenames).
*/
ANT_vector_candidate *fused = new ANT_vector_candidate[top_k * 2];
char **fused_filenames = new char *[top_k * 2];

if (query_text != NULL && *query_text != '\0')
	lexical_count = search(query_text, top_k);
for (which = 0; which < lexical_count; which++)
	{
	fused[fused_count].generation = results[which].generation;
	fused[fused_count].docid = results[which].docid;
	fused[fused_count].score = 1.0 / (60.0 + (double)(which + 1));
	fused_filenames[fused_count] = new char[strlen(results[which].filename) + 1];
	strcpy(fused_filenames[fused_count], results[which].filename);
	fused_count++;
	}

/*
	Vector side: candidates + rank contribution, merged into the fused set
	by (generation, docid) identity.
*/
if (query_vector != NULL && vector_dimension_current != 0)
	{
	ANT_vector_candidate *best = new ANT_vector_candidate[top_k];
	vector_count = vector_candidates(query_vector, top_k, best);
	qsort(best, (size_t)vector_count, sizeof(*best), vector_candidate_compare);
	for (which = 0; which < vector_count; which++)
		{
		double contribution = 1.0 / (60.0 + (double)(which + 1));
		long found = false;
		for (other = 0; other < fused_count; other++)
			if (fused[other].generation == best[which].generation && fused[other].docid == best[which].docid)
				{
				fused[other].score += contribution;
				found = true;
				break;
				}
		if (!found)
			{
			fused[fused_count].generation = best[which].generation;
			fused[fused_count].docid = best[which].docid;
			fused[fused_count].score = contribution;
			char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));
			fused_filenames[fused_count] = new char[(filename != NULL ? strlen(filename) : 0) + 1];
			strcpy(fused_filenames[fused_count], filename != NULL ? filename : "");
			fused_count++;
			}
		}
	delete [] best;
	}

/*
	Sort fused by score desc (ties: generation, docid asc), truncate, publish
	into the shared results array.  The lexical search above already freed the
	previous results' filenames; free ITS filenames now that they're
	snapshotted, then repopulate.
*/
qsort(fused, (size_t)fused_count, sizeof(*fused), vector_candidate_compare);

for (which = 0; which < results_count; which++)
	delete [] results[which].filename;
results_count = 0;

long long publish = fused_count < top_k ? fused_count : top_k;
for (which = 0; which < publish; which++)
	{
	if (results_count >= results_allocated)
		{
		long long bigger_size = results_allocated == 0 ? 256 : results_allocated * 2;
		hit *bigger = new hit[bigger_size];
		memcpy(bigger, results, (size_t)(results_count * sizeof(*results)));
		delete [] results;
		results = bigger;
		results_allocated = bigger_size;
		}
	results[results_count].generation = fused[which].generation;
	results[results_count].docid = fused[which].docid;
	results[results_count].score = fused[which].score;
	results[results_count].filename = fused_filenames[which];		/* ownership transfer */
	fused_filenames[which] = NULL;
	results_count++;
	}
for (which = publish; which < fused_count; which++)
	delete [] fused_filenames[which];
delete [] fused_filenames;
delete [] fused;
return results_count;
}
```

OWNERSHIP TRAP the implementer must double-check: after the truncation sort, `fused_filenames[which]` no longer corresponds to `fused[which]` (qsort reordered `fused` but not the parallel filename array!). Fix by fusing the filename pointer INTO the sorted structure: extend a local struct `{ANT_vector_candidate c; char *filename;}` and sort THAT (one comparator adaptation), or carry an index-indirection array through the sort. Do NOT ship the parallel-array version above as-is — restructure so filename and candidate travel together, then the publish loop transfers ownership correctly and the tail loop frees the unpublished ones. The test's assertion on `get_hit(0)->filename` will catch a mismatch, but only for the specific fixture — restructure regardless.

- [ ] **Step 4:** all green (26 functions), six binaries, `make internal`.

- [ ] **Step 5: Commit** — `feat: RRF hybrid search fusing lexical and vector top-k`

---

### Task 6: metric modes end-to-end + backward compatibility

**Files:**
- Test: `tests/test_segment_index.cpp` (append `test_vector_metrics_and_compat`)
- Modify: `atire/atire_segment_index.cpp` only if a metric bug falls out.

- [ ] **Step 1: the test — append, call from `main()`:**

```cpp
/*
	TEST_VECTOR_METRICS_AND_COMPAT()
	--------------------------------
*/
static void test_vector_metrics_and_compat(void)
{
char *dir_cos = make_index_dir();
char *dir_l2 = make_index_dir();
float query[4] = {2.0f, 0.0f, 0.0f, 0.0f};		// deliberately unnormalized

/*
	Cosine: unnormalized inputs rank identically to their normalized forms;
	zero vector rejected
*/
ATIRE_segment_index *cos_index = new ATIRE_segment_index();
CHECK(cos_index->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(cos_index->open(dir_cos) == 0);
float big[4] = {10.0f, 0.0f, 0.0f, 0.0f};		// same direction as query, huge magnitude
float small_off[4] = {0.1f, 0.1f, 0.0f, 0.0f};	// 45 degrees off
CHECK(cos_index->add_document("doc-aligned", "<DOC>alpha</DOC>", big) >= 0);
CHECK(cos_index->add_document("doc-off", "<DOC>beta</DOC>", small_off) >= 0);
float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
CHECK(cos_index->add_document("doc-zero", "<DOC>gamma</DOC>", zero) == -1);
CHECK(cos_index->search_vector(query, 2) == 2);
CHECK(strcmp(cos_index->get_hit(0)->filename, "doc-aligned") == 0);
CHECK(fabs(cos_index->get_hit(0)->score - 1.0) < 1e-5);		// cosine of aligned unit vectors
CHECK(fabs(cos_index->get_hit(1)->score - 0.7071) < 1e-3);	// cos 45deg
delete cos_index;

/*
	L2: nearest by euclidean distance wins; scores are negative squared distances
*/
ATIRE_segment_index *l2_index = new ATIRE_segment_index();
CHECK(l2_index->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_L2) == 0);
CHECK(l2_index->open(dir_l2) == 0);
float near[4] = {2.1f, 0.0f, 0.0f, 0.0f};
float far_[4] = {5.0f, 5.0f, 0.0f, 0.0f};
CHECK(l2_index->add_document("doc-near", "<DOC>alpha</DOC>", near) >= 0);
CHECK(l2_index->add_document("doc-far", "<DOC>beta</DOC>", far_) >= 0);
CHECK(l2_index->search_vector(query, 2) == 2);
CHECK(strcmp(l2_index->get_hit(0)->filename, "doc-near") == 0);
CHECK(fabs(l2_index->get_hit(0)->score - (-0.01)) < 1e-5);
CHECK(l2_index->get_hit(1)->score < l2_index->get_hit(0)->score);
delete l2_index;

/*
	Backward compatibility: a pre-vector index (no vector.config) opens,
	searches lexically, and vector calls are safe no-ops
*/
char *plain_dir = make_index_dir();
ATIRE_segment_index *plain = new ATIRE_segment_index();
CHECK(plain->open(plain_dir) == 0);
CHECK(plain->add_document("doc-1", "<DOC>aardvark</DOC>") >= 0);
CHECK(plain->flush() == 0);
delete plain;
ATIRE_segment_index *plain_reopened = new ATIRE_segment_index();
CHECK(plain_reopened->open(plain_dir) == 0);
CHECK(plain_reopened->vector_dimension() == 0);
char query_text[64];
strcpy(query_text, "aardvark");
CHECK(plain_reopened->search(query_text, 10) == 1);
CHECK(plain_reopened->search_vector(query, 10) == 0);
strcpy(query_text, "aardvark");
CHECK(plain_reopened->search_hybrid(query_text, query, 10) == 1);	// degrades to lexical
delete plain_reopened;

delete [] dir_cos;
delete [] dir_l2;
delete [] plain_dir;
printf("test_vector_metrics_and_compat OK\n");
}
```

- [ ] **Step 2:** run. Cosine/L2 kernels and normalization were built in Tasks 1–3, so this SHOULD pass; a failure is a real bug (likely: query normalization missing in one path, or L2 sign). Root-cause and fix; don't weaken tolerances beyond what float32 warrants.

- [ ] **Step 3: Commit** — `test: cosine and L2 metric modes; pre-vector index compatibility`

---

### Task 7: full regression + docs sweep

**Files:**
- Test-only run + possible comment fixes.

- [ ] **Step 1:** Full sweep, paste all outputs: seven test binaries (`test_vector_store` + existing six) PASSED; `test_segment_index` runs 27 functions; run it twice; `make internal` exit 0; `git status --short` clean.
- [ ] **Step 2:** Grep the new code for plan-task references, stray debug prints, or TODOs (`grep -n "Task [0-9]\|TODO\|fprintf(stderr" source/vector_store.* atire/atire_segment_index.*` — fix any found; mechanism names, not plan numbers).
- [ ] **Step 3:** Commit anything from step 2 as `docs: comment cleanup for vector search`; otherwise note clean.

---

## Self-review record

- **Spec coverage:** §1 decisions (optional vectors/presence, metric enum, exact scan, RRF k=60, dense layout, caller-supplied embeddings) → Tasks 1/2/3/5; §2.1 vector.config semantics incl. mismatch-fails-open and corrupt-treated-absent → Task 2; §2.2 .vec format/validation/degradation/atomicity/orphan-sweep-inheritance (no code needed — established) + cosine add-time normalization + zero-vector rejection → Tasks 1/2; §3.1 store scan/writer → Task 1; §3.2 memory buffer → Task 2; §3.3 API surface incl. NULL degradations and shared-results contract → Tasks 2/3/5; §3.4 flush/compact/tombstones-updates-deletes integration → Tasks 3/4; §4 error handling table → Tasks 1/2/3/6; §5 tests: unit (Task 1), NRT/hybrid/mixed/update/delete/flush-reopen (Tasks 3/5), compaction equivalence (Task 4), metrics + dimension/disabled rejections + backward compat (Tasks 2/6). §6 out-of-scope respected.
- **Flagged-in-task risks:** auto-flush vs vector-append ordering (Task 2 — refactor to add_document_core), parallel-array ownership through qsort in search_hybrid (Task 5 — restructure mandated), flush() local-variable capture order (Task 3), member-name adaptation points marked where the plan can't see the file's current locals.
- **Type consistency:** ANT_vector_candidate/{insert,compare} shared across Tasks 1/3/5; ANT_vector_store::{load,scan,kernel,normalize,METRIC_*} consistent; writer create/append/finish/abandon consistent; coordinator members named once in Task 2 and reused verbatim in Tasks 3/4/5.

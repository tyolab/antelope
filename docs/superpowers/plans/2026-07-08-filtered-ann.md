# Filtered ANN Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Constrain vector / hybrid / lexical / rerank search by rich boolean metadata predicates (AND/OR/NOT over eq/range/in on typed fields), applied in-traversal so a selective filter never under-returns.

**Architecture:** Declared typed attribute columns (`.attr`) + an opaque payload blob (`.pay`) are stored per segment; an `ANT_filter` predicate tree is evaluated per segment into a **match bitset** that composes with tombstones and is passed down to the exact-scan / HNSW / signature / rerank paths (extending the V3 tombstone under-fill admit test to `live AND match`). Low-level scan/HNSW stay attribute-agnostic — they receive a plain `const unsigned char *filter_bits` (NULL = admit all).

**Tech Stack:** C++ (`source/`, `atire/`), Node-API addon (`nodejs/`), reusing the segment lifecycle, `ANT_index_tombstones`, the ragged-store pattern from V5 `.mvec`, and `ANT_vector_quantize` conventions.

**Spec:** `docs/superpowers/specs/2026-07-08-filtered-ann-design.md`.

---

## Conventions (read once)

- **Worktree:** branch `feature/filtered-ann` under `.worktrees/filtered-ann` (executor sets up via using-git-worktrees). Build `make -C <wt>`, git `git -C <wt>`, never `cd`.
- **NO header dependency tracking:** after editing ANY `.h`, `rm -f <wt>/obj/*.o` before rebuilding. `.cpp`-only tasks can use a plain `make`.
- **Build discovery:** `make all` auto-discovers `source/*.cpp` + `atire/*.cpp` (no Makefile edit for new files). `make tests` builds `tests/test_X.cpp` → `bin/test_X`.
- **Test harness:** each test file defines `CHECK(cond)`; read a neighbour first. `ATIRE_segment_index` has NO `close()` — use `delete`. `flush()` is synchronous → 0. Searches return a `long long` count; hits via `get_hit(i)->{generation,docid,score,filename}`. Helpers in `tests/test_segment_index.cpp`: `make_index_dir()`, `dir_has_glob()`.
- **Sidecar discipline (V1–V5):** validate-before-allocate load (exact size from header, checked before any `new[]`, degrade to empty on any mismatch); atomic writer (temp+rename); free every buffer on every error path + in dtor; compaction refreshes `output_segment` caches BEFORE the step-6 shuffle.
- **Bit conventions:** a bitset is `unsigned char[(n+7)/8]`, bit `d` = `bits[d>>3] & (1<<(d&7))`. `filter_bits == NULL` means "admit all" everywhere.
- **Milestones:** attribute+payload stores + filter engine exist after Task 5; filtered vector search end-to-end after Task 9; HNSW under-fill correctness after Task 10; full lifecycle after Task 13; Node after Task 14; hardened after Task 15.

## File structure

- Create `source/attribute_store.{h,cpp}` — `ANT_attribute_store` (columnar `.attr` load + typed column accessors) + `ANT_attribute_store_writer`. Also holds the `ANT_attribute_schema` value type (field name/type/multivalued) reused by config + filter + writer.
- Create `source/payload_store.{h,cpp}` — `ANT_payload_store` (`.pay` ragged blob) + writer.
- Create `source/attribute_filter.{h,cpp}` — `ANT_filter` predicate tree (build + typed validation + per-segment `evaluate(attrs, out_bits)`).
- Create tests: `tests/test_attribute_store.cpp`, `tests/test_payload_store.cpp`, `tests/test_attribute_filter.cpp`.
- Modify `atire/atire_segment_index.h`/`.cpp` — `struct segment.attributes`/`.payload`; schema config; NRT writer buffers; flush/load/dtor/delete; `hit.payload`/`payload_length`.
- Modify `atire/atire_segment_index_vector.cpp` — attribute-config trio, `ANT_attribute_set` capture, filter threading through the vector/approx/hnsw/rerank gatherers.
- Modify `atire/atire_segment_index_search.cpp` — lexical filter admit + payload publication.
- Modify `atire/atire_segment_index_compaction.cpp` — `.attr`/`.pay` merge + dict union-remap + input-free.
- Modify `source/vector_store.{h,cpp}`, `source/hnsw.{h,cpp}` — add trailing `const unsigned char *filter_bits = NULL` to `scan`/`search`; admit = not-deleted AND (filter_bits NULL or bit set).
- Modify `nodejs/addon/segment_index.cpp`, `nodejs/segment_index.d.ts`, `nodejs/README.md`, create `nodejs/test/filter.test.js`.

**Documented limitations (state in code comments, don't fix):** attributes + payload are NOT WAL-logged (durable-mode doc added-but-unflushed loses them on replay; flushed sidecars are durable). No backfill (attrs/payload supply-at-index-time only). `double` attribute type excluded.

---

## Task 1: `ANT_attribute_schema` + `attributes.config` (immutable)

**Files:** Create `source/attribute_store.h` (schema type only, for now); Modify `atire/atire_segment_index.{h,cpp}`, `atire/atire_segment_index_vector.cpp`; Test `tests/test_segment_index.cpp`.

The schema is a small value type describing filterable fields. It is fixed on first `set_attributes_config` and persisted immutably (mirror `rerank.config`).

- [ ] **Step 1: Failing test** (`tests/test_segment_index.cpp`, register in `main()`)

```cpp
static void test_attributes_config_persist(void)
{
char *dir = make_index_dir();
{
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->attributes_configured() == 0);
	ANT_attribute_schema s;
	CHECK(s.add_field("tenant", ANT_attribute_schema::TYPE_STRING, 0) == 0);
	CHECK(s.add_field("created", ANT_attribute_schema::TYPE_INT64, 0) == 0);
	CHECK(s.add_field("lang", ANT_attribute_schema::TYPE_STRING, 1) == 0);		/* multi */
	CHECK(s.add_field("archived", ANT_attribute_schema::TYPE_BOOL, 0) == 0);
	CHECK(s.add_field("flags", ANT_attribute_schema::TYPE_BOOL, 1) != 0);		/* multi bool rejected */
	CHECK(ix->set_attributes_config(s) == 0);
	CHECK(ix->attributes_configured() != 0);
	CHECK(ix->attribute_field_count() == 4);
	delete ix;
}
{
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->attributes_configured() != 0);				/* persisted */
	CHECK(ix->attribute_field_count() == 4);
	ANT_attribute_schema other;
	other.add_field("tenant", ANT_attribute_schema::TYPE_STRING, 0);
	CHECK(ix->set_attributes_config(other) != 0);			/* different schema: rejected (immutable) */
	delete ix;
}
delete [] dir;
printf("test_attributes_config_persist OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — `rm -f <wt>/obj/*.o && make -C <wt> all >/dev/null 2>&1; make -C <wt> test_segment_index` → compile FAIL.

- [ ] **Step 3: `source/attribute_store.h`** — the schema type:

```cpp
#ifndef ATTRIBUTE_STORE_H_
#define ATTRIBUTE_STORE_H_

class ANT_attribute_schema
{
public:
	enum { TYPE_INT64 = 0, TYPE_STRING = 1, TYPE_BOOL = 2 };
	enum { MAX_FIELDS = 64 };

private:
	long field_count;
	char names[MAX_FIELDS][64];
	int types[MAX_FIELDS];
	int multi[MAX_FIELDS];

public:
	ANT_attribute_schema() : field_count(0) {}
	long add_field(const char *name, int type, int multivalued);	// 0 ok; nonzero on dup name / bad type / multi-bool / overflow / long name
	long count(void) const { return field_count; }
	const char *name(long i) const { return names[i]; }
	int type(long i) const { return types[i]; }
	int is_multi(long i) const { return multi[i]; }
	long field_index(const char *name) const;						// -1 if absent
	long equals(const ANT_attribute_schema &o) const;				// 1 if identical (order+name+type+multi)
} ;

#endif
```
Implement these inline-or-in-`attribute_store.cpp` now (create the `.cpp` with just the schema methods; later tasks extend it). `add_field` rejects: duplicate name, `type` not in {0,1,2}, `multivalued && type==TYPE_BOOL`, `field_count>=MAX_FIELDS`, `strlen(name)>=64`.

- [ ] **Step 4: Header wiring** (`atire/atire_segment_index.h`) — `#include "../source/attribute_store.h"` (or forward-declare + include in `.cpp`); add members:
```cpp
	ANT_attribute_schema attribute_schema_current;		// empty (count()==0) = not configured
```
and decls:
```cpp
	long load_attributes_config(void);
	long save_attributes_config(void);
	long set_attributes_config(const ANT_attribute_schema &schema);	// immutable once set; 0 on success
	long attributes_configured(void) { return attribute_schema_current.count() != 0; }
	long attribute_field_count(void) { return attribute_schema_current.count(); }
	int attribute_field_type(long i) { return attribute_schema_current.type(i); }
```

- [ ] **Step 5: Implement config trio** (`atire/atire_segment_index_vector.cpp`, mirror `load/save/set_rerank_config`): `attributes.config` binary — magic `"ANTATTR1"`, version 1u, field_count, then per field: 64-byte name, int32 type, int32 multi. `load_attributes_config` defensively parses (garbage/overflow → leave unconfigured). `set_attributes_config`: require `directory != NULL`; if already configured, return `attribute_schema_current.equals(schema) ? 0 : 1`; else copy + `save`. Constructor: `attribute_schema_current` default-constructs empty; in `open()` call `load_attributes_config()` after `load_rerank_config()`.

- [ ] **Step 6: Verify pass** — `rm -f <wt>/obj/*.o && make -C <wt> all && make -C <wt> test_segment_index && <wt>/bin/test_segment_index` → `PASSED` + `test_attributes_config_persist OK`.

- [ ] **Step 7: Commit** — `git add source/attribute_store.h source/attribute_store.cpp atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp && git commit -m "feat(filter): attribute schema + attributes.config persisted + immutable"`

---

## Task 2: `ANT_attribute_store` + writer (`.attr` columnar sidecar)

**Files:** Modify `source/attribute_store.{h,cpp}`; Test `tests/test_attribute_store.cpp`.

Columnar per-segment store. Per field (schema order): a presence bitset + the column (int64: flat or ragged; bool: value bitset; string: dict-id array or ragged, + per-field string dictionary). Validate-before-allocate load.

- [ ] **Step 1: Failing test** (`tests/test_attribute_store.cpp`) — write a small schema (int64 single, string single, string multi, bool single), append 4 docs (one missing a field), finish, load, assert typed accessors:
```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "attribute_store.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)

int main(void)
{
ANT_attribute_schema s;
s.add_field("created", ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("tenant",  ANT_attribute_schema::TYPE_STRING, 0);
s.add_field("lang",    ANT_attribute_schema::TYPE_STRING, 1);
s.add_field("archived",ANT_attribute_schema::TYPE_BOOL, 0);

char path[64]; strcpy(path,"/tmp/ant_attr_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_attribute_store_writer w;
CHECK(w.create(path, &s) == 0);
/* doc0: full */
w.begin_document();
w.set_int(0, 1700000000LL); w.set_string(1, "acme");
w.add_string(2, "en"); w.add_string(2, "fr"); w.set_bool(3, 1);
w.end_document();
/* doc1: only tenant + archived=false (created + lang MISSING) */
w.begin_document(); w.set_string(1, "beta"); w.set_bool(3, 0); w.end_document();
/* doc2: lang single value */
w.begin_document(); w.set_int(0, 1700000500LL); w.add_string(2, "de"); w.end_document();
/* doc3: empty */
w.begin_document(); w.end_document();
CHECK(w.finish() == 0);

ANT_attribute_store *a = ANT_attribute_store::load(path, &s, 4);
CHECK(a != NULL && a->document_count() == 4);
long long v;
CHECK(a->get_int(0, 0, &v) && v == 1700000000LL);		/* field 0 (created) doc 0 */
CHECK(!a->has_field(1, 0) == 0);						/* doc0 has tenant */
long tenant_id = a->string_id(1, "acme");				/* dict id for "acme" in field 1 */
CHECK(tenant_id >= 0);
CHECK(a->string_matches(1, 0, tenant_id));				/* doc0 tenant == "acme" */
CHECK(a->string_matches(2, 0, a->string_id(2, "fr")));	/* doc0 lang contains "fr" (multi) */
CHECK(!a->string_matches(2, 0, a->string_id(2, "de")));	/* doc0 lang !contains "de" */
CHECK(a->string_matches(2, 2, a->string_id(2, "de")));	/* doc2 lang contains "de" */
CHECK(a->string_id(1, "nonexistent") < 0);				/* dict miss */
CHECK(!a->has_field(0, 1));								/* doc1 missing created */
int b;
CHECK(a->get_bool(3, 1, &b) && b == 0);					/* doc1 archived=false */
CHECK(!a->has_field(3, 3));								/* doc3 missing archived */
delete a; unlink(path);
printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `make -C <wt> test_attribute_store` → FAIL (classes/methods undefined).

- [ ] **Step 3: Extend `source/attribute_store.h`** — add the store + writer classes:
```cpp
class ANT_attribute_store {
	/* per-field: presence bitset; int64 col (flat/ragged); bool value bitset; string dict-id col + dictionary */
	// (private state omitted here — implementer lays it out; see Step 4)
public:
	~ANT_attribute_store();
	static ANT_attribute_store *load(const char *filename, const ANT_attribute_schema *schema, long long expected_documents);
	long long document_count(void);
	long has_field(long field, long long docid);				// presence bit
	long get_int(long field, long long docid, long long *out);	// single-valued int64; 0 if absent, 1 + fills out
	long get_bool(long field, long long docid, int *out);		// single-valued bool
	long string_id(long field, const char *literal);			// dict id for literal in this field's dictionary; <0 if absent
	long string_matches(long field, long long docid, long want_id);	// doc's string field (single OR multi) contains want_id
	long int_matches_range(long field, long long docid, long long lo, long long hi, int lo_incl, int hi_incl);	// any value in range
	long int_equals(long field, long long docid, long long want);	// any value == want (single or multi)
	// enumerate a doc's int values for multi (used by range/eq multi): via a small accessor or fold into the above
};

class ANT_attribute_store_writer {
	/* buffers per-doc field values until finish(); builds per-field dictionaries + columns */
public:
	ANT_attribute_store_writer();
	~ANT_attribute_store_writer();
	long create(const char *path, const ANT_attribute_schema *schema);
	void begin_document(void);
	void set_int(long field, long long value);
	void add_int(long field, long long value);		// multi-valued
	void set_string(long field, const char *value);
	void add_string(long field, const char *value);	// multi-valued
	void set_bool(long field, int value);
	void end_document(void);
	long finish(void);
	void abandon(void);
};
```
Provide `int_equals`/`int_matches_range`/`string_matches` as the *matcher* primitives the filter (Task 4/5) will call (so the store owns comparison against its own columns/dictionary).

- [ ] **Step 4: Implement `source/attribute_store.cpp`** — buffered writer accumulates per-doc values (grow arrays per field; strings interned into a per-field `std::`-free dictionary — use a simple growable char-pool + offset array, first-seen id assignment). `finish()` writes: header (magic `"ANTATTRS"`, version, document_count, field_count) then, per field in schema order: presence bitset; then the column per the field's type/multi (int64 flat `int64[docs]` or ragged `counts/offsets/pool`; bool value bitset; string single `int32[docs]` or ragged id pool; for string fields also write the dictionary: count + length-prefixed strings). Atomic temp+rename. **Load** validates magic/schema-match(field_count + each type/multi)/document_count, computes the EXACT expected size field-by-field BEFORE allocating (degrade to empty on mismatch), reads columns + dictionaries, frees all on error. `string_id` binary/linear-searches the field dictionary. Destructor frees every column/dictionary/presence buffer.

- [ ] **Step 5: Verify pass** — `make -C <wt> all && make -C <wt> test_attribute_store && <wt>/bin/test_attribute_store` → `PASSED`.

- [ ] **Step 6: Commit** — `git add source/attribute_store.h source/attribute_store.cpp tests/test_attribute_store.cpp && git commit -m "feat(filter): columnar .attr attribute store (int64/string-dict/bool, single+multi, validating load)"`

---

## Task 3: `ANT_payload_store` + writer (`.pay` ragged blob)

**Files:** Create `source/payload_store.{h,cpp}`; Test `tests/test_payload_store.cpp`.

- [ ] **Step 1: Failing test** — write 3 payloads (one empty), load, assert bytes + lengths (mirror the `.mvec`/ragged pattern):
```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "payload_store.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
int main(void){
char path[64]; strcpy(path,"/tmp/ant_pay_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_payload_store_writer w; CHECK(w.create(path)==0);
CHECK(w.append("hello",5)==0);
CHECK(w.append(NULL,0)==0);				/* doc1 empty */
CHECK(w.append("world!!",7)==0);
CHECK(w.finish()==0);
ANT_payload_store *p = ANT_payload_store::load(path, 3);
CHECK(p!=NULL && p->document_count()==3);
const unsigned char *b; long long n;
p->get(0,&b,&n); CHECK(n==5 && memcmp(b,"hello",5)==0);
p->get(1,&b,&n); CHECK(n==0);
p->get(2,&b,&n); CHECK(n==7 && memcmp(b,"world!!",7)==0);
/* corrupt -> empty */
FILE *f=fopen(path,"wb"); fputs("nope",f); fclose(f);
ANT_payload_store *bad = ANT_payload_store::load(path, 3);
CHECK(bad!=NULL && bad->document_count()==0);
delete p; delete bad; unlink(path);
printf("PASSED\n"); return 0; }
```

- [ ] **Step 2–4: Implement** `payload_store.{h,cpp}` — header magic `"ANTPAY01"`, version, `document_count`, `total_bytes`; `offsets[document_count+1]` + byte pool. Writer buffers appended blobs; `finish()` writes offsets + pool atomically. `load` validates size (`header + (docs+1)*8 + total_bytes`) before alloc, degrades to empty. `get(docid,&ptr,&len)` returns the slice (NULL/0 for empty). Dtor frees.

- [ ] **Step 5–6: Verify + commit** — `make -C <wt> all && make -C <wt> test_payload_store && <wt>/bin/test_payload_store` → `PASSED`; `git add source/payload_store.* tests/test_payload_store.cpp && git commit -m "feat(filter): ragged .pay payload store"`

---

## Task 4: `ANT_filter` predicate tree + typed build validation

**Files:** Create `source/attribute_filter.{h,cpp}`; Test `tests/test_attribute_filter.cpp`.

- [ ] **Step 1: Failing test** — build well-typed and ill-typed trees; assert `build(schema)` succeeds/rejects:
```cpp
#include <stdio.h>
#include <stdlib.h>
#include "attribute_filter.h"
#include "attribute_store.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
int main(void){
ANT_attribute_schema s;
s.add_field("created", ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("tenant",  ANT_attribute_schema::TYPE_STRING, 0);
s.add_field("archived",ANT_attribute_schema::TYPE_BOOL, 0);

/* valid: created >= X AND tenant == "acme" AND NOT archived */
ANT_filter *f = ANT_filter::and_(3,
	ANT_filter::range_int("created", 1700000000LL, 1, 0, 0, 1, 0),   /* created >= 1700000000 (has_lo=1, has_hi=0) */
	ANT_filter::eq_string("tenant", "acme"),
	ANT_filter::not_(ANT_filter::eq_bool("archived", 1)));
CHECK(f != NULL);
CHECK(f->build(&s) == 0);					/* type-checks */
delete f;

/* invalid: eq_string on an int64 field */
ANT_filter *bad = ANT_filter::eq_string("created", "x");
CHECK(bad->build(&s) != 0);					/* type mismatch */
delete bad;
/* invalid: unknown field */
ANT_filter *bad2 = ANT_filter::eq_int("nope", 1);
CHECK(bad2->build(&s) != 0);
delete bad2;
/* invalid: range on bool */
ANT_filter *bad3 = ANT_filter::range_int("archived", 0, 1, 1, 1, 1, 1);
CHECK(bad3->build(&s) != 0);
delete bad3;
printf("PASSED\n"); return 0; }
```

- [ ] **Step 2–4: Implement** `attribute_filter.{h,cpp}`:
```cpp
class ANT_attribute_store;
class ANT_filter {
public:
	enum { OP_AND, OP_OR, OP_NOT, OP_EQ, OP_RANGE, OP_IN };
	enum { VT_INT, VT_STRING, VT_BOOL };
	// factories (heap; the tree owns its children and is freed by delete):
	static ANT_filter *and_(int n, ...);				// varargs children
	static ANT_filter *or_(int n, ...);
	static ANT_filter *not_(ANT_filter *child);
	static ANT_filter *eq_int(const char *field, long long v);
	static ANT_filter *eq_string(const char *field, const char *v);
	static ANT_filter *eq_bool(const char *field, int v);
	static ANT_filter *range_int(const char *field, long long lo, int has_lo, long long hi, int has_hi, int lo_incl, int hi_incl); // has_lo/has_hi=0 => that bound is open
	static ANT_filter *in_int(const char *field, const long long *vals, long n);
	static ANT_filter *in_string(const char *field, const char *const *vals, long n);
	~ANT_filter();
	long build(const ANT_attribute_schema *schema);		// resolve field indices + type-check; 0 ok, nonzero on any mismatch
	void evaluate(ANT_attribute_store *attrs, long long documents, unsigned char *out_bits);	// Task 5
};
```
`build` walks the tree: each leaf resolves its field name → index (fail if absent), checks the value type against the field type (INT↔INT64, STRING↔STRING, BOOL↔BOOL; RANGE only on INT64 or STRING; fail otherwise), recurses into AND/OR/NOT. Store the resolved field index on each leaf. For `range_int`, represent "no lower bound"/"no upper bound" with explicit `has_lo`/`has_hi` flags (add them to the factory signature — adjust the test call accordingly if you change it). Leaves for strings keep the literal(s) as owned copies. Destructor frees children + owned literals.

- [ ] **Step 5–6: Verify + commit** — `make -C <wt> test_attribute_filter && <wt>/bin/test_attribute_filter` → `PASSED`; commit `source/attribute_filter.* tests/test_attribute_filter.cpp` — "feat(filter): ANT_filter predicate tree + typed build validation".

---

## Task 5: Filter evaluation → match bitset (+ pinned missing-field semantics)

**Files:** Modify `source/attribute_filter.{h,cpp}`; Test `tests/test_attribute_filter.cpp`.

- [ ] **Step 1: Failing test** — build an `.attr` store (reuse the Task-2 writer) with a few docs, evaluate several filters, compare the bitset to a hand-computed expectation, INCLUDING `NOT(EQ(missing))==true`:
```cpp
/* add to main() before PASSED: write a 4-doc .attr like Task 2's, load it, then: */
unsigned char bits[1];		/* 4 docs -> 1 byte */
/* created >= 1700000500 -> only doc2 (doc0=1700000000, doc1 missing, doc3 missing) */
ANT_filter *g = ANT_filter::range_int("created", 1700000500LL, 1, 0, 0, 1, 0);
CHECK(g->build(&s) == 0);
memset(bits,0,1); g->evaluate(a, 4, bits);
CHECK((bits[0] & 1) == 0);					/* doc0 no */
CHECK((bits[0] & 2) == 0);					/* doc1 missing -> false */
CHECK((bits[0] & 4) != 0);					/* doc2 yes */
delete g;
/* NOT(created == 1700000000) -> doc1,doc2,doc3 true (missing counts as NOT-equal) */
ANT_filter *h = ANT_filter::not_(ANT_filter::eq_int("created", 1700000000LL));
CHECK(h->build(&s) == 0);
memset(bits,0,1); h->evaluate(a, 4, bits);
CHECK((bits[0] & 1) == 0);					/* doc0 IS equal -> NOT false */
CHECK((bits[0] & 2) != 0);					/* doc1 missing -> not-equal -> true */
CHECK((bits[0] & 8) != 0);					/* doc3 missing -> true */
delete h;
```

- [ ] **Step 2–4: Implement** `ANT_filter::evaluate(attrs, documents, out_bits)`: recursive, each node fills a `documents`-bit scratch bitset:
  - leaf: for each docid, ask the store's matcher primitive (`int_equals` / `int_matches_range` / `string_matches` via `string_id` lookup / bool compare via `get_bool`), which returns false when the field is absent → leaf bit 0. (A string literal absent from the segment dictionary → all-false for EQ; for IN, OR of the present literals.)
  - `AND`/`OR`: bitwise combine children scratch bitsets.
  - `NOT`: complement over `[0, documents)` (so a doc missing the field flips to true).
  Write the root's bitset into `out_bits`. Allocate scratch bitsets with `new`/`delete[]` per node (or a small pool); zero the unused high bits of the last byte. Add a private helper for AND/OR/NOT over `(documents+7)/8` bytes.

- [ ] **Step 5–6: Verify + commit** — run test → `PASSED` (esp. the NOT-missing asserts); commit — "feat(filter): per-segment bitset evaluation with pinned missing-field semantics".

---

## Task 6: `ANT_attribute_set` ingest builder + NRT writer buffers + add/update capture

**Files:** Modify `source/attribute_store.{h,cpp}` (add `ANT_attribute_set`), `atire/atire_segment_index.{h,cpp}`, `atire/atire_segment_index_vector.cpp`; Test `tests/test_segment_index.cpp`.

`ANT_attribute_set` is a caller-filled per-document builder (typed setters + payload) passed to add/update. The writer buffers accumulate per-pending-doc values + payload (parallel to `writer_vector_data`). This task only CAPTURES; flush is Task 7 (verify capture via a hook).

- [ ] **Step 1: Failing test** — configure schema, `add_document` with an `ANT_attribute_set`, assert a `writer_attribute_count_for_test()` hook and payload length hook. (Mirror `test_writer_multivector_capture`.)
- [ ] **Step 3: `ANT_attribute_set`** — holds a pointer to the schema; `set_int(field,v)`/`add_int`/`set_string`/`add_string`/`set_bool`/`set_payload(ptr,len)`; internally records (field, values) + payload bytes. Provide read accessors the segment-index uses at capture time.
- [ ] **Step 4: writer buffers** — the segment-index keeps an `ANT_attribute_store_writer`-shaped live buffer is overkill; instead keep a growing per-doc record: reuse the store writer's `begin_document/set_*/end_document` accumulation by holding a live `ANT_attribute_store_writer` opened to a temp? NO — simplest: keep parallel live buffers keyed by writer docid: for attributes, hold an in-memory `ANT_attribute_set`-list (one per pending doc) OR directly drive a live `ANT_attribute_store_writer` that flush finalizes. **Chosen approach:** hold a live `ANT_attribute_store_writer *writer_attr` (created lazily when the first attributed doc is added, `create()` to the pending `seg_G.attr` path is deferred — instead buffer in memory) — to avoid disk churn, keep an in-memory vector of captured `ANT_attribute_set` copies (`writer_attribute_sets`) indexed by docid, and a `writer_payload_data`/`writer_payload_offsets` byte buffer. `add_document_core` (trailing `const ANT_attribute_set * = NULL`) copies the set into slot `docid` and appends its payload. Provide `writer_attribute_count_for_test(docid)` (number of set fields) + `writer_payload_len_for_test(docid)`.
- [ ] **Step 5–6:** verify capture + commit — "feat(filter): ANT_attribute_set builder + NRT attribute/payload capture".

(Implementer note: the in-memory captured-sets approach keeps Task 6 self-contained; Task 7's flush drains them through `ANT_attribute_store_writer` + `ANT_payload_store_writer`. Free the captured sets + payload buffer in `reset_writer_vectors()` alongside the multi-vector buffers.)

---

## Task 7: Flush writes `.attr` + `.pay` + segment-load + dtor + delete

**Files:** Modify `atire/atire_segment_index.{h,cpp}`; Test `tests/test_segment_index.cpp`.

- [ ] **Step 1: Failing test** — configure schema, add 20 attributed docs (+payloads), `flush()`, assert `dir_has_glob(dir,"seg_*.attr")` + `"seg_*.pay"`, reopen, `attributes_configured()`.
- [ ] **Step 3: Implement** — `struct segment` gains `ANT_attribute_store *attributes; ANT_payload_store *payload;` (header; forward-declare the classes). Flush: after the `.mvec` block, `if (attributes_configured())` drain the captured writer sets through an `ANT_attribute_store_writer` (`begin/set/end` per docid `[0,flushed_document_count)`, absent docs → empty) → `seg_G.attr`, and the payload buffer through `ANT_payload_store_writer` → `seg_G.pay`. `append_segment`: load both when `attributes_configured()` (else NULL). Destructor: free both. `delete_segment_files`: unlink `.attr` + `.pay`.
- [ ] **Step 5–6:** verify + commit — "feat(filter): flush writes .attr/.pay; segment-load/dtor/delete".

---

## Task 8: `hit.payload` + payload on published results

**Files:** Modify `atire/atire_segment_index.h`, `atire/atire_segment_index_search.cpp` (+ the vector publish in `_vector.cpp`); Test `tests/test_segment_index.cpp`.

- [ ] **Step 1: Failing test** — add attributed docs with payloads, flush, `search_vector`, assert `get_hit(0)->payload_length > 0` and bytes match for a known doc.
- [ ] **Step 3: Implement** — `struct hit` gains `const unsigned char *payload; long long payload_length;` (default NULL/0). Wherever results are published (the `append_result`/slot-fill loops in `search_vector_impl` and `search_hybrid_impl` and lexical `search`), after setting generation/docid, if `attributes_configured()` and the owning segment has a payload store, set `slot->payload`/`payload_length` from `segment.payload->get(docid,...)` (live-buffer hits → the writer payload buffer). NULL/0 otherwise. (Unfiltered result docids/scores/order unchanged → byte-identical.)
- [ ] **Step 5–6:** verify + commit — "feat(filter): payload returned on hits".

---

## Task 9: Filter admit in `scan()` + exact vector gatherer (MILESTONE: filtered vector search)

**Files:** Modify `source/vector_store.{h,cpp}`, `atire/atire_segment_index_vector.cpp`, `atire/atire_segment_index.h`; Test `tests/test_segment_index.cpp`.

- [ ] **Step 1: Failing test** — a crafted set where the vector-nearest doc is filtered out; `search_vector(query, k, filter)` must return only matching docs (and the nearest MATCHING one first):
```cpp
static void test_filtered_vector_scan(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
ANT_attribute_schema s; s.add_field("tenant", ANT_attribute_schema::TYPE_STRING, 0);
CHECK(ix->set_attributes_config(s) == 0);
float qv[4] = {1,0,0,0};
float nearv[4] = {1,0,0,0}, farv[4] = {0.2f,0,0,0};
ANT_attribute_set A(&s); A.set_string(0, "other");  ix->add_document("near","<DOC>x</DOC>", nearv, NULL, 0, &A);
ANT_attribute_set B(&s); B.set_string(0, "acme");   ix->add_document("far","<DOC>y</DOC>", farv, NULL, 0, &B);
CHECK(ix->flush() == 0);
/* unfiltered: near first */
CHECK(ix->search_vector(qv, 2) == 2);
CHECK(strcmp(ix->get_hit(0)->filename, "near") == 0);
/* filter tenant==acme: only "far" qualifies */
ANT_filter *f = ANT_filter::eq_string("tenant", "acme");
CHECK(f->build(ix->attribute_schema()) == 0);
long long n = ix->search_vector(qv, 5, f);
CHECK(n == 1);
CHECK(strcmp(ix->get_hit(0)->filename, "far") == 0);
delete f; delete ix; delete [] dir;
printf("test_filtered_vector_scan OK\n");
}
```
(Add `const ANT_attribute_schema *attribute_schema(void) { return &attribute_schema_current; }` accessor for the test's `build`.)

- [ ] **Step 3: Implement** — add trailing `const unsigned char *filter_bits = NULL` to `ANT_vector_store::scan`; admit iff `!tombstones->is_deleted(docid) && (filter_bits==NULL || (filter_bits[docid>>3] & (1<<(docid&7))))`. Add filtered public overload `search_vector(query, k, const ANT_filter *filter)` → routes to `search_vector_impl(query, k, EXACT, filter)`. In `vector_candidates` (exact gatherer): for each segment, if `filter != NULL` evaluate it into a per-segment bitset (`new unsigned char[(docs+7)/8]`, `filter->evaluate(segments[w].attributes, docs, bits)`; a segment with no `.attr` → all-zero bits so nothing matches) and pass `bits` to `scan`; free after. Live-buffer exact scan: evaluate against the writer's captured sets (a `maxsim_live`-style helper, or skip filtering the live buffer in this task and note it — BUT the test flushes, so disk path suffices; still, wire live-buffer filtering by evaluating the filter against the in-memory captured sets for correctness). Thread `const ANT_filter *filter` through `search_vector_impl` + `vector_candidates`.
- [ ] **Step 5–6:** verify (+ existing vector tests unchanged since `filter==NULL`) + commit — "feat(filter): filtered exact vector scan (filter_bits admit)".

---

## Task 10: HNSW in-traversal admit (MILESTONE: no under-fill)

**Files:** Modify `source/hnsw.{h,cpp}`, `atire/atire_segment_index_vector.cpp`; Test `tests/test_segment_index.cpp`.

- [ ] **Step 1: The under-fill test** — build an HNSW segment where the top-k nearest are all filtered OUT, and ≥k matching docs sit further away; assert `search_vector_hnsw(query, k, filter)` still returns k MATCHING docs:
```cpp
static void test_filtered_hnsw_no_underfill(void)
{
char *dir = make_index_dir();
long long dim = 8, n = 200, i, d;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_hnsw_config(16, 200) == 0);
ANT_attribute_schema s; s.add_field("keep", ANT_attribute_schema::TYPE_BOOL, 0);
CHECK(ix->set_attributes_config(s) == 0);
srand(19);
float *data = new float[n*dim];
for (i=0;i<n*dim;i++) data[i]=(float)(rand()%2000-1000)/500.0f;
/* the 20 vectors NEAREST to a fixed query are marked keep=false; everything else keep=true */
float q[8]; for (d=0;d<dim;d++) q[d]=(float)(rand()%2000-1000)/500.0f;
/* mark keep=false for docs whose index < 20 AND arrange them to be near q by copying q-ish */
for (i=0;i<n;i++)
	{
	int keep = (i >= 20) ? 1 : 0;
	if (i < 20) for (d=0;d<dim;d++) data[i*dim+d] = q[d] + (float)(rand()%100-50)/1000.0f;  /* near q, keep=false */
	ANT_attribute_set A(&s); A.set_bool(0, keep);
	char k[16]; sprintf(k,"d%lld",i); char doc[32]; sprintf(doc,"<DOC>t</DOC>");
	CHECK(ix->add_document(k, doc, data+i*dim, NULL, 0, &A) >= 0);
	}
CHECK(ix->flush() == 0);
ANT_filter *f = ANT_filter::eq_bool("keep", 1);
CHECK(f->build(ix->attribute_schema()) == 0);
long long got = ix->search_vector_hnsw(q, 10, f);
CHECK(got == 10);								/* NOT under-filled despite 20 nearer non-matching docs */
for (i=0;i<got;i++) { /* every returned doc must be keep=true: its key index >= 20 */
	long long idx = atoll(ix->get_hit(i)->filename + 1);
	CHECK(idx >= 20);
	}
delete f; delete [] data; delete ix; delete [] dir;
printf("test_filtered_hnsw_no_underfill OK\n");
}
```

- [ ] **Step 3: Implement** — add trailing `const unsigned char *filter_bits = NULL` to `ANT_hnsw::search`. The admit test at the result-collection points (hnsw.cpp:332/347/361 region) becomes `admit(docid) = (tombstones==NULL || !tombstones->is_deleted(docid)) && (filter_bits==NULL || bit_set(filter_bits,docid))`. CRUCIAL: keep traversing through non-admitted neighbours (push to the candidate queue for connectivity) but only add ADMITTED nodes to the result heap `W` — exactly the tombstone under-fill pattern already there; extend that same predicate with the filter bit. In `vector_candidates_hnsw`, evaluate the filter → per-segment bits and pass to `hnsw_graph->search(...)`; filtered `search_vector_hnsw(query, k, filter)` overload.
- [ ] **Step 5–6:** verify (the under-fill test is the gate) + commit — "feat(filter): HNSW in-traversal filter admit (no under-fill)".

---

## Task 11: Filter threading — approx + hybrid + rerank

**Files:** Modify `atire/atire_segment_index_vector.cpp`, `atire/atire_segment_index.h`; Test `tests/test_segment_index.cpp`.

- [ ] **Step 1: Failing test** — filtered `search_vector_approx`, `search_hybrid`, `search_hybrid_hnsw`, and `search_rerank` each return only matching docs (small crafted set).
- [ ] **Step 3: Implement** — thread `const ANT_filter *filter` into `vector_candidates_approx` (bitset-filter the signature shortlist before insert; fallback scan gets `filter_bits`), `search_hybrid_impl` (pass filter to the vector leg; the lexical leg gets the same bits — see Task 12), and `search_rerank` (evaluate filter per segment and skip non-admitted stage-1 candidates before MaxSim). Add filtered public overloads for `search_vector_approx`, `search_hybrid`, `search_hybrid_approx`, `search_hybrid_hnsw`, `search_rerank`. Reuse a single helper `evaluate_filter_for_segment(w, filter, &bits)` to avoid duplicating the eval+alloc.
- [ ] **Step 5–6:** verify + commit — "feat(filter): filter threading through approx/hybrid/rerank".

---

## Task 12: Lexical search filter admit + payload

**Files:** Modify `atire/atire_segment_index_search.cpp`, `atire/atire_segment_index.h`; Test `tests/test_segment_index.cpp`.

- [ ] **Step 1: Failing test** — `search(text, k, filter)` returns only matching docs; a selective filter still fills k when ≥k lexical matches qualify (over-pull from the ranked accumulators).
- [ ] **Step 3: Implement** — filtered `search(char *query, long long top_k, const ANT_filter *filter)`. In `search_impl`/the lexical top-k publication, when `filter != NULL` evaluate per segment → bits and, during the accumulator→top-k selection, SKIP docids not admitted (continue pulling ranked candidates until `top_k` admitted or exhausted). Compose with the existing tombstone skip. (Lexical is score-ordered, so "keep pulling" = don't stop at the first `top_k` before filtering.)
- [ ] **Step 5–6:** verify + commit — "feat(filter): lexical search filter admit".

---

## Task 13: Compaction merges `.attr` + `.pay` (dict union-remap) + input-free

**Files:** Modify `atire/atire_segment_index_compaction.cpp`, `source/attribute_store.{h,cpp}` (add a `copy`/enumeration accessor if needed); Test `tests/test_segment_index.cpp`.

- [ ] **Step 1: Failing test** — build 2 flushed segments with attributes+payloads, `compact()`, assert `.attr`/`.pay` survive AND a filtered search over the merged segment returns the right docs AND payloads still match (proves dictionary remap + docid alignment).
- [ ] **Step 3: Implement** — mirror the `.mvec` compaction block. For `.attr`: build the merged segment via `ANT_attribute_store_writer` (its `begin/set/end` per surviving output doc, reading each input doc's values via store accessors — for strings, read the input dict string and `set_string` it, so the WRITER rebuilds the merged dictionary automatically = union-remap for free). For `.pay`: `ANT_payload_store_writer` appending each surviving doc's payload slice. Same input-outer/docid-inner/`renumber<0` skip order as `.vec` (docids align). Refresh `output_segment->attributes`/`->payload` BEFORE the step-6 shuffle. Input-free: `delete segments[which].attributes/payload`. (Add store accessors to enumerate a doc's int/string/bool values if not already exposed — e.g. `get_string(field, docid, valueindex, char *out)` + `value_count(field, docid)`.)
- [ ] **Step 5–6:** verify + commit — "feat(filter): compaction merges .attr/.pay with dictionary union-remap".

---

## Task 14: Node binding

**Files:** Modify `nodejs/addon/segment_index.cpp`, `nodejs/segment_index.d.ts`, `nodejs/README.md`; Create `nodejs/test/filter.test.js`.

- [ ] **Step 1: Failing test** (`nodejs/test/filter.test.js`) — construct with `attributes` schema, `addDocument(..., { attributes, payload })`, `searchVector(q, k, { filter })`, assert only matching docs + payload Buffer round-trips.
- [ ] **Step 3: Implement** — constructor `attributes: { name: 'int64'|'string'|'bool'|'int64[]'|'string[]' }` → build an `ANT_attribute_schema`, `set_attributes_config` in `open()` (non-fatal). `addDocument`/`updateDocument` optional 5th options arg `{ attributes: {field:value|[values]}, payload: Buffer|string }` → fill an `ANT_attribute_set` (type per schema; throw TypeError on mismatch) + `set_payload`. Every search method accepts a `{ filter }` option → translate the JSON predicate (`and/or/not/eq/range/in`) into an `ANT_filter`, `build(schema)` (throw on error), pass to the filtered overload. Hits include `payload` (Buffer) when present. Add `.d.ts` + README "Filtered search" section.
- [ ] **Step 5–6:** verify (`node <wt>/nodejs/test/filter.test.js` + existing suites) + commit — "feat(filter): Node attributes schema + addDocument attrs/payload + {filter} predicate + payload Buffer".

---

## Task 15: Coexistence + unfiltered byte-identical + ASan

**Files:** Test `tests/test_segment_index.cpp`.

- [ ] **Step 1: Tests** — (a) coexistence: attributes + quantize(replace) + approximate + hnsw + rerank on ONE index, add/flush/delete/maintain, every search entry point WITH a filter returns sane matching top-k. (b) unfiltered byte-identical: run a fixed query with `filter=NULL` and assert identical top-k (generation,docid,score) to the same index built without any attributes configured. (c) filter+tombstones compose (delete a matching doc → excluded).
- [ ] **Step 2: Normal build gate** — full `test_segment_index` `PASSED`.
- [ ] **Step 3: ASan sweep** — rebuild with `-fsanitize=address -g -fPIC` (CXXFLAGS/CFLAGS/LDFLAGS) + `ASAN_OPTIONS=detect_leaks=0 bin/test_segment_index`; expect `PASSED`, no `ERROR` on the `.attr`/`.pay`/filter lifecycle. The pre-existing `ANT_file::setvbuff` leak is out of scope. Restore the normal build after.
- [ ] **Step 4: Commit** — "test(filter): coexistence + unfiltered byte-identical + ASan lifecycle sweep".

---

## Final review

Dispatch a holistic review over the whole diff (`git diff master...HEAD`) focusing on: `.attr`/`.pay` load validation (validate-before-allocate, per-field size math overflow-safety, all columns freed on error), filter evaluation correctness (three-valued/missing-field semantics, string dict-miss, multi-valued CONTAINS), the HNSW under-fill admit (traverse-through vs admit), no leaks at the `.attr`/`.pay` lifecycle sites incl. compaction dictionary rebuild, filter==NULL byte-identicalness, config immutability, and Node predicate translation (throws on malformed, frees scratch). Fix any Critical/Important finding with a regression test. Then use superpowers:finishing-a-development-branch (merge locally per the standing preference).

# Filtered ANN Design

**Status:** approved 2026-07-08, ready for implementation planning.

**Goal:** Constrain Antelope's vector / hybrid / lexical search by rich structured-metadata
predicates (boolean AND/OR/NOT over equality, range, and set-membership on typed fields), applied
*in-traversal* so a selective filter never under-returns.

**Architecture (one sentence):** Declared typed attribute columns + an opaque payload blob are
stored in per-segment sidecars; a boolean predicate tree is evaluated per segment into a match
bitset that composes with tombstones and is threaded through the exact-scan / HNSW / signature /
rerank paths (reusing the V3 tombstone under-fill traversal), so admitted results are always live
AND matching.

**Tech stack:** C++ engine (`source/`, `atire/`), Node-API addon (`nodejs/`), reusing the segment
lifecycle, the tombstone (`ANT_index_tombstones`) admit mechanism, and the ragged-store pattern
from V5's `.mvec`.

---

## 1. Scope & components

New per-segment sidecars + one index-wide config, riding the same 7-site lifecycle as
`.vec`/`.qvec`/`.vsig`/`.hnsw`/`.mvec`:
- **`attributes.config`** — immutable schema: an ordered list of `(name, type, multi_valued)`
  filterable fields, fixed on first `set_attributes_config` (mirrors `rerank.config`).
- **`seg_G.attr`** — columnar typed attribute store (one column per declared field + per-field
  presence bits; strings dictionary-encoded per segment).
- **`seg_G.pay`** — ragged opaque payload blob (offsets + byte pool), returned with hits, never
  indexed or filtered.
- **`ANT_filter`** — a query-time boolean predicate tree, evaluated per segment into a match bitset.

**Contract:** attributes and payload are **supply-at-index-time** (caller-provided; no backfill,
same as multi-vectors). The unfiltered search path (no `ANT_filter`) MUST be byte-identical to
pre-feature behaviour.

**Out of scope (future):** `double`/float attribute type; geo/spatial predicates; full-text
predicates on attribute strings; attribute updates decoupled from document updates; selectivity-
adaptive brute-force (the in-traversal path is always used in this version).

## 2. Attribute schema & config

`attributes.config` (binary magic `"ANTATTR1"` + version + field count, then per field: name,
type, multi_valued flag). Immutable once written. Field **types**:
- `ATTR_INT64` — dates (epoch), prices (cents), counts, enums-as-int.
- `ATTR_STRING` — dictionary-encoded per segment.
- `ATTR_BOOL`.

`int64` and `string` fields are **single- or multi-valued**; a multi-valued field holds a set per
doc (e.g. `lang = {en, fr}`), and EQ/IN/RANGE match if *any* value satisfies the leaf (`CONTAINS`
semantics). `bool` is **single-valued only** (a set of bools is degenerate) — declaring a
multi-valued bool is a schema build error. `double` is intentionally excluded (int64-scaled covers
dates/prices and avoids float-equality foot-guns); it is a documented future addition.

Setter/queries on `ATIRE_segment_index`:
`set_attributes_config(const ANT_attribute_schema &schema)` (before first flush; immutable),
`attributes_configured()`, and schema introspection (`attribute_field_count()`,
`attribute_field_type(i)`).

## 3. Attribute store (`seg_G.attr`) layout

Header: magic `"ANTATTRS"`, version, `document_count`, `field_count`. Then, per field in schema
order:
- a **presence** bitset (`document_count` bits — does this doc have this field?);
- the **column**:
  - `int64` single → `int64[document_count]`; multi → ragged (`counts[]`, `offsets[]`, `int64` pool)
    — the `.mvec` ragged pattern.
  - `bool` (single-valued only) → a value bitset (paired with the field's presence bitset).
  - `string` single → `int32 dict_id[document_count]`; multi → ragged `int32` id pool. Plus a
    per-segment **string dictionary** for this field: `count`, then length-prefixed UTF-8 strings,
    id = index (ids assigned in first-seen order at write; a lookup miss at query time = "no such
    value in this segment" → leaf false).

Validate-before-allocate load (exact size from header/counts before any `new[]`; corrupt /
truncated / size-bomb → empty store; free all columns on every error path), same discipline as
every V-sidecar. A missing/degraded `.attr` on a filter-configured index degrades that segment to
"no doc matches any field" for FILTERED queries (unfiltered queries never consult `.attr`, so they
are unaffected); documented, and new docs written with attributes are fine.

## 4. Payload store (`seg_G.pay`) layout

Header: magic `"ANTPAY01"`, version, `document_count`, `total_bytes`. Then `offsets[document_count+1]`
(int64 prefix sums) + the byte pool. `payload(docid)` returns `(const unsigned char *, long long len)`;
an absent doc → `(NULL, 0)`. Validate-before-allocate load; degrade to empty on corruption.

## 5. Predicate model & evaluation

`ANT_filter` tree node kinds: `AND(children…)`, `OR(children…)`, `NOT(child)`, `EQ(field, value)`,
`RANGE(field, lo, hi, lo_inclusive, hi_inclusive)`, `IN(field, {values…})`. Values are typed; a
leaf whose value type mismatches the field's declared type is a **build-time error**
(`ANT_filter::build()` returns non-NULL only for a well-typed tree). RANGE is defined for `int64`
and `string` (lexicographic) fields; EQ/IN for all; a RANGE/EQ/IN leaf naming an undeclared field
is a build error.

**Evaluation** is per segment, bottom-up, into a docid bitset over `[0, document_count)`:
- Leaf: scan the field column; for `int64`/`bool` compare directly; for `string` map the literal(s)
  to this segment's dict id(s) first (a literal absent from the dictionary contributes no matches).
  Multi-valued: the leaf is true for a doc iff *any* of its values satisfies the comparison.
- `AND`/`OR`/`NOT`: bitwise combine child bitsets (`NOT` complements over `[0, document_count)`).

**Missing-value (three-valued) semantics — pinned:** a doc lacking a field (presence bit 0) makes
any EQ/RANGE/IN leaf on that field **false** for that doc. `NOT` is a strict complement over all
docids, so `NOT(EQ(f, v))` is **true** for a doc that lacks `f` (it is "not equal"). This is stated
explicitly so callers can reason about absence; documented in the spec and asserted in tests.

The per-segment match bitset composes with the segment's tombstones: **admissible(docid) = live(docid)
AND (filter == NULL OR match_bitset[docid])**.

## 6. Search integration

The unified cores and gatherers take an optional `const ANT_filter *filter = NULL` (NULL =
unfiltered, byte-identical). Before scanning a segment, evaluate the filter once → a match bitset
(cached for that segment for the duration of the query). Then:
- **exact scan** (`ANT_vector_store::scan` / lexical): skip a docid unless admissible.
- **HNSW** (`ANT_hnsw::search`): traverse through inadmissible neighbours for graph connectivity but
  admit only admissible docids to the result heap — the exact mechanism added for the V3 tombstone
  under-fill fix, extended so the "admit?" test is `live AND match`. The `search` signature gains an
  optional match-bitset parameter alongside the tombstones it already takes.
- **approx/signatures**: bitset-filter the shortlist before the exact rerank.
- **rerank** (`search_rerank`): filter the stage-1 candidate set.

Because the bitset is precomputed and the HNSW walk keeps exploring until `top_k` admissible
results are found (bounded by the visited set), a highly selective filter still returns `k` matches
when at least `k` exist — the property post-filtering lacks.

**Payload on hits:** `struct hit` gains `const unsigned char *payload; long long payload_length;`
(both 0/NULL when the segment has no payload for the doc). Populated during result publication from
`segment.payload`.

## 7. API surface

### C++ (`ATIRE_segment_index`)
- Schema: `set_attributes_config(const ANT_attribute_schema &)`, `attributes_configured()`.
- Ingest: an `ANT_attribute_set` builder — `set_int(field, long long)`, `add_int` (multi),
  `set_string(field, const char *)`, `add_string`, `set_bool(field, bool)`, `set_payload(const
  void *, long long)`. Passed as a trailing `const ANT_attribute_set * = NULL` on the add/update
  path (the same defaulted-param extension used for multi-vectors), so existing overloads are
  unchanged.
- Query: an `ANT_filter` builder (static factories `ANT_filter::eq/range/in/and_/or_/not_`, then
  `build(schema)` → validated tree or NULL). Filtered public overloads:
  `search(query, k, filter)`, `search_vector(query, k, filter)`, `search_vector_approx(...)`,
  `search_vector_hnsw(...)`, `search_hybrid(...)`, `search_hybrid_approx(...)`,
  `search_hybrid_hnsw(...)`, `search_rerank(..., filter)` — each mirrors its unfiltered form with a
  trailing `const ANT_filter *filter`.
- Hits: `get_hit(i)->payload` / `->payload_length`.

### Node (`nodejs/addon/segment_index.cpp`)
- Constructor `attributes: { tenant:'string', created:'int64', lang:'string[]', archived:'bool' }`
  (a `'<type>[]'` suffix marks multi-valued).
- `addDocument(key, text, vector, multiVectors, { attributes: {...}, payload: Buffer|string })`.
- Every search method accepts a `{ filter }` option — a JSON predicate tree:
  `{ and:[…] } | { or:[…] } | { not:{…} } | { eq:{field:value} } |
   { range:{field:{gte?,gt?,lte?,lt?}} } | { in:{field:[…]} }`. The addon translates it into an
  `ANT_filter` (type-checked against the schema; a bad predicate throws a `TypeError`).
- Hits include `payload` (Buffer) when present. `.d.ts` + README "Filtered search" section.

## 8. Lifecycle & compaction

`.attr` + `.pay` ride all seven sites like the other sidecars:
1. **flush** — write from NRT writer buffers (per-pending-doc attribute columns + payload,
   parallel to `writer_vector_data`; reset on flush).
2. **append_segment** — load into `segment.attributes` / `segment.payload` when
   `attributes_configured()`.
3. **destructor** — free both.
4. **delete_segment_files** — unlink `seg_G.attr` + `seg_G.pay`.
5. **orphan sweep** — generic `seg_*`, no change.
6. **compaction** — rewrite both in merged-docid order via the existing `ANT_docid_renumberer`
   (same input-outer/docid-inner/`renumber<0`-skip order as `.vec`, so docids align). The string
   **dictionary is rebuilt** as the union of inputs' dictionaries with ids remapped; the refreshed
   stores are loaded into `output_segment` before the step-6 shuffle.
7. **compaction input-free** — free inputs' `.attr`/`.pay`.

**WAL:** attributes + payload are NOT WAL-logged in this version (same documented limitation as
multi-vectors — a durable-mode doc added-but-unflushed loses its attributes/payload on crash-replay;
flushed sidecars are durable). Future follow-up.

## 9. Testing

- **Units:** `.attr` columnar roundtrip per type (int64/string/bool, single + multi, presence /
  missing, dictionary encode/lookup-miss) with validate-before-allocate (corrupt/truncated/size-bomb
  → empty); `.pay` roundtrip; predicate evaluation vs a brute-force reference for each leaf +
  AND/OR/NOT, INCLUDING the pinned missing-field semantics (`NOT(EQ(missing))==true`).
- **Integration:** filtered `search_vector`/`hnsw`/`approx`/`hybrid`/`rerank` return only matching
  docs; **the under-fill test** — a selective filter over an HNSW segment returns `k` matches when
  ≥`k` exist (crafted so the k matches sit past the unfiltered top-k); filter + tombstones compose
  (deleted ∪ non-matching both excluded); compaction preserves attributes + payload + filtering incl.
  dictionary remap; **unfiltered path byte-identical** to pre-feature (a `filter==NULL` run equals
  the current results). Coexistence with quantization + rerank on one index.
- **Node:** filtered searches for each predicate form; payload round-trips as a Buffer; a malformed
  filter throws.

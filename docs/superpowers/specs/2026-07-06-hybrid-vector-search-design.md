# Hybrid Vector Search V1 — Design

**Date:** 2026-07-06
**Status:** Draft for review
**Parent work:** the segmented incremental index
(`2026-07-05-incremental-index-design.md`, Phase 1+2, shipped). This document adds
dense-vector similarity search and hybrid (keyword + vector) retrieval to
`ATIRE_segment_index`, per the approved feasibility recommendation: V1 = exact scan +
RRF fusion, fully integrated with the segment lifecycle. Binary-signature prefiltering
(V2) and ANN graphs (V3) are explicitly out of scope.

**Problem:** AI/RAG workloads retrieve by embedding similarity, ideally fused with
keyword relevance. Antelope has the document lifecycle (add/update/delete, NRT, flush,
tombstones, compaction, crash safety) but no vector storage or similarity search.

**Design principle:** vectors are a per-segment *sidecar* sharing the segment's docid
space, so every lifecycle mechanism — tombstone filtering, updates, compaction
renumbering, orphan sweeping, crash ordering — applies to vectors with almost no new
machinery. The engine is embedding-model-agnostic: callers supply vectors.

---

## 1. Decisions

| Question | Decision | Rationale |
|---|---|---|
| Vectors per document | **Optional** (presence bitmap) | Mixed corpora: re-embedding backlogs and metadata-only docs must not be errors. |
| Similarity metric | **Configurable enum: dot / cosine / L2**, fixed at index creation | Caller choice per Eric. Internally two kernels (dot, L2-as-negative-squared-distance) plus an add-time + query-time normalization flag for cosine — cosine IS the dot kernel on normalized data. |
| Search strategy | **Exact brute-force scan** per segment | At embedded scale (≤ low millions of vectors) exact scan is fast enough and has zero index-build/merge cost. Compiler autovectorization (`-O3`) is the only "SIMD" dependency. |
| Fusion | **Reciprocal Rank Fusion**, constant k=60 | Industry standard; needs no score calibration between BM25 and similarity scales. |
| Vector layout | **Dense**: `doc_count × dimension` float32, absent docs zero-filled | O(1) access by docid, trivial compaction rewrite. Packed-present layouts are a V2 space optimization. |
| Embeddings | Supplied by the caller | No model execution in-engine, ever. |

## 2. On-disk formats

### 2.1 `<dir>/vector.config`
Text, two lines: dimension, metric (0=dot, 1=cosine, 2=L2). Written once (atomically,
temp+rename) by the first `open()` after `set_vector_config()`; never rewritten.
Absent file = vector search disabled — a pure Phase 1/2 index; every existing index
keeps working untouched. `open()` on an index whose config disagrees with a prior
`set_vector_config()` call fails (silent dimension mixups corrupt results; fail loudly).
Defensive parsing per house convention (bounded values, garbage → treat as absent).

### 2.2 `seg_G.vec`
Binary, written atomically (temp+rename) at flush/compaction, before the manifest
references the segment (existing crash contract):

```
uint64  magic        ("ANTVEC01" as 8 bytes)
int64   dimension    (must equal vector.config's)
int64   document_count
byte[]  presence bitmap  ((document_count+7)/8 bytes; bit set = doc has a vector)
float32[] vectors    (document_count * dimension, dense, absent rows zero-filled)
```

Validation on load: magic, dimension match, count match against the segment's engine,
file size arithmetic. Any failure → the segment degrades to lexical-only (presence
treated as all-absent), same forgiving posture as a missing `.del`. A missing `.vec`
for a manifested segment likewise means "no vectors in this segment" (also the state of
segments flushed before this feature existed).

The existing orphan sweep already covers `seg_G.vec` — it parses the generation from any
`seg_`-prefixed name regardless of extension, so a segment's `.aspt`/`.del`/`.vec` share
one fate with zero changes.

In cosine mode, vectors are unit-normalized **once at add time** (stored normalized);
queries are normalized at search time. Zero-magnitude vectors are rejected at add
(`-1`) in cosine mode.

## 3. Components

### 3.1 `ANT_vector_store` (`source/vector_store.h/.cpp`)
Per-segment vector reader/writer, unit-testable in isolation (the
tombstones/manifest/keymap pattern).

Reader: `load(filename, expected_dimension, expected_documents)` (heap-resident, matching
the INDEX_IN_MEMORY convention; returns a degraded empty store on validation failure),
`has(docid)`, `get(docid)`, and the scan:
`scan(query, metric, tombstones, top_k, heap)` — iterates present, non-tombstoned docids,
computes the metric kernel, maintains a top-k min-heap of (score, docid). Exhaustive scan
means tombstones are filtered inline — **no over-fetch needed on the vector side**.

Writer: `create(filename, dimension)`, `append(const float *vector_or_NULL)` (buffered),
`finish()` (atomic temp+rename), `abandon()`. Cosine-mode normalization happens in the
coordinator before append, so the store stays metric-agnostic on the write path.

Score conventions: dot and cosine — higher is better, raw value. L2 — score is
`-(squared distance)` so "higher is better" holds engine-wide and no sqrt is spent.

### 3.2 Memory-segment vector buffer
Inside `ATIRE_segment_index`: a growable `float*` array + presence flags parallel to the
writer's docids (the `doc_list` growth pattern). NRT vector search scans it directly.
Flushed into `seg_G.vec` by `flush()`; discarded with the writer. Lost on crash exactly
like the writer's documents (relaxed durability, unchanged contract).

### 3.3 `ATIRE_segment_index` API additions

```cpp
enum { VECTOR_METRIC_DOT = 0, VECTOR_METRIC_COSINE = 1, VECTOR_METRIC_L2 = 2 };

long set_vector_config(long long dimension, long metric);   // before open(); 0 on success
long long vector_dimension(void);                            // 0 = vectors disabled

long long add_document(const char *key, const char *document, const float *vector);
long long update_document(const char *key, const char *document, const float *vector);
	// vector == NULL -> lexical-only document (presence bit clear)
	// vector on a non-vector-enabled index, or dimension mismatch at the API
	//   boundary (caller passes a vector when dimension is 0) -> -1
	// existing two-argument overloads remain and behave as vector = NULL

long long search_vector(const float *query, long long top_k);
	// exact top-k across all segments + memory buffer; hit.score = similarity
	// (metric convention above); returns 0 if vectors are disabled or query NULL

long long search_hybrid(char *query_text, const float *query_vector, long long top_k);
	// lexical top-k (existing path, including its tombstone over-fetch) and
	// vector top-k, fused by RRF: fused(d) = sum over lists of 1/(60 + rank(d)),
	// ranks 1-based; hit.score = fused score; results are the top_k by fused
	// score with deterministic tie-break (generation, docid).
	// query_text NULL/empty -> pure vector; query_vector NULL -> pure lexical.
```

Search results reuse the existing `hit` array and its lifetime contract (valid until the
next search; filenames deep-copied). All three search methods share the result storage —
a `search_hybrid` invalidates a prior `search`'s hits, as today.

### 3.4 Lifecycle integration
- **flush()**: after `writer->finish()`, write `seg_G.vec` from the memory buffer
  (only if vectors are enabled AND at least one vector was added — an all-lexical batch
  writes no `.vec`); ordering stays segment-files-before-manifest.
- **compact()**: builds its own `ANT_docid_renumberer` from the same inputs (identical
  tombstones and segment order → numbering matches the merger's exactly, both are
  deterministic) and writes `seg_OUT.vec` (merged presence + rows for live docs) right
  after the merger writes `seg_OUT.aspt`, before the marker/remap/manifest steps. A
  failure writing the `.vec` fails the compaction pre-marker (full cleanup path). Inputs
  without `.vec` contribute absent rows.
- **Tombstones/updates/deletes**: nothing new — the vector side reads the same
  tombstone objects at scan time; an updated document's new vector rides the new docid.
- **Keymap, marker recovery, orphan sweep**: untouched; `.vec` files follow their
  segment's fate automatically.

## 4. Error handling summary
- `set_vector_config` after open, dimension < 1, dimension > 65536, or unknown metric → 1.
- `open()` with prior `set_vector_config` disagreeing with an existing `vector.config` → 1.
- Corrupt/missing `.vec` → segment is lexical-only (never fails open, never crashes).
- `add_document` with a vector when disabled → -1; zero vector in cosine mode → -1.
- `search_vector`/vector side of hybrid when disabled → 0 hits / lexical-only fusion.

## 5. Testing
- **Unit (`tests/test_vector_store.cpp`)**: write/read roundtrip incl. absent rows;
  presence bitmap; corrupt header/magic/dimension/truncation → degraded empty store;
  atomic finish (no `.tmp` survivor); top-k scan correctness against a hand-computed
  fixture for all three metrics; tombstone skip; k larger than live count.
- **E2E (`tests/test_segment_index.cpp`)**:
  - NRT: vectors searchable immediately after add, before any flush.
  - Hybrid: a document matching both keyword and vector sides outranks single-side
    matchers (deterministic fixture); NULL-side degradation both ways.
  - Mixed corpus: lexical-only docs excluded from vector results, present in hybrid via
    the lexical list.
  - Update replaces the vector (old similarity unreachable); delete removes from vector
    results immediately.
  - Flush + reopen: vectors persist; segments flushed with no vectors stay lexical.
  - **Compaction equivalence**: after a messy history + `maintain()`, `search_vector`
    results (docids resolved to keys, scores) equal a one-shot index of the surviving
    collection — the vector analog of the postings-level equivalence test.
  - Metric modes: cosine normalization (unnormalized inputs give same ranking as
    pre-normalized), L2 ordering sanity.
  - Dimension mismatch and disabled-index rejections.
  - Backward compat: a pre-vector index opens and searches; enabling vectors on a fresh
    index leaves lexical behavior identical.

## 6. Out of scope (V2/V3, tracked)
- Binary-signature prefilter (TopSig-modernized Hamming stage) — V2.
- int8 quantization; packed-present `.vec` layout — V2.
- HNSW / graph ANN — V3, only on demonstrated need.
- Metadata filtering beyond what keys/keywords give; multi-vector documents.
- SIMD intrinsics (autovectorization only in V1).
- Node.js binding exposure (rides the existing binding work track).

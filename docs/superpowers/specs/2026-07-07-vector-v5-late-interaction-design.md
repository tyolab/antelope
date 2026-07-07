# Vector V5 — Late-Interaction Reranking (MaxSim) Design

**Status:** approved 2026-07-07, ready for implementation planning.

**Goal:** Add an engine-native second-stage reranker to Antelope: store per-document
multi-vectors (`.mvec` sidecar) and rescore a first-stage candidate set with ColBERT-style
**MaxSim** late interaction, raising retrieval precision without any query-time model inference.

**Architecture (one sentence):** A new ragged (variable-length) multi-vector store reusing V4's
per-dimension int8 quantization, plus a two-stage `search_rerank` that takes the existing
first-stage top-N (BM25 / single-vector ANN / hybrid) and re-ranks it by MaxSim over the
candidates' multi-vectors.

**Tech stack:** C++ engine (`source/`, `atire/`), Node-API addon (`nodejs/addon/`), reusing
`ANT_vector_quantize` (V4), the segment lifecycle (`ATIRE_segment_index`), and the existing
`search_*` first-stage retrieval.

---

## 1. Scope & phasing

**Built in V5 (this spec):**
- `ANT_multivector_store` + `seg_G.mvec` sidecar (ragged, int8 by default / float option).
- `maxsim()` scoring kernel + a brute-force reference for tests.
- `search_rerank(...)` two-stage search that reranks the existing first stage.
- `rerank.config` (immutable), config setters, NRT writer buffer, full segment lifecycle.
- Node binding (`rerank` option, `addDocument` multi-vector arg, `searchRerank`).

**Designed-for-later (NOT built in V5):**
- Token-ANN / PLAID-style stage-1 retrieval (build ANN over all token vectors, gather candidate
  docs by per-token nearest-neighbour hits). It will reuse the same `.mvec` store + `maxsim()`
  kernel; V5's interfaces must not preclude it. Tracked as a future Vn.
- Using HNSW/approx as the stage-1 source inside `search_rerank` (default stage 1 is exact/hybrid).

**Principle preserved:** the engine never embeds. The caller supplies the document's multi-vector
matrix at index time and the query's multi-vector matrix at search time. The engine is agnostic to
what the M vectors represent (tokens, chunks, sentences — the caller's granularity choice).

## 2. Data model & storage

New store `ANT_multivector_store` / `ANT_multivector_store_writer`
(`source/multivector_store.{h,cpp}`), one per-segment sidecar `seg_G.mvec`. Each document has a
**variable** count `M_d ≥ 0` of `D`-dimensional vectors (0 = no multi-vectors for that doc), so the
layout is ragged (unlike V1–V4's one-fixed-vector-per-doc).

### File layout (`.mvec`)
```
magic          uint64   "ANTMVEC1"
dimension      int64    D
document_count int64    number of docs this sidecar covers
total_vectors  int64    Σ M_d
quant_flag     int32    0 = float32 pool, 1 = int8 pool
counts[document_count]   int32    M_d per doc (0 = absent)
offsets[document_count+1] int64   prefix sums into the pool (offsets[0]=0, offsets[n]=total_vectors)
pool           total_vectors × D    int8 (quant_flag=1) or float32 (quant_flag=0)
qmin[D], qmax[D]  float32           present ONLY when quant_flag=1 (per-dimension ranges)
```

### Decisions
- **int8 by default**, reusing V4 `ANT_vector_quantize` (per-dimension ranges computed over the
  whole pool, exactly like `.qvec`). Multi-vectors are 10–100× the vector count of `.vec`, so float
  would be prohibitive — this is the V4 payoff. `float` is a config option for exact MaxSim.
- **Independent dimension `D`** from the doc-level vector dimension (ColBERT token vectors are often
  128-dim while a doc vector is 768). `D` lives in `rerank.config`.
- **Always normalized on ingest + query** (not configurable), so MaxSim's per-pair similarity is a
  plain dot product. The writer L2-normalizes each supplied vector before quantizing;
  `search_rerank` L2-normalizes each query vector before scoring. A zero vector among the M is
  allowed and stored as zeros (contributes 0 similarity), unlike the single-vector cosine path
  which rejects an all-zero doc vector. (Rationale: one dead token shouldn't reject a document.)
- **Validate-before-allocate load** (same discipline as every V-sidecar): the fixed header is
  **36 bytes** (`8+8+8+8+4`); from it compute the exact expected file size
  `36 + document_count*4 + (document_count+1)*8 + total_vectors*D*elem + (quant?D*4*2:0)`
  (elem = 1 for int8, 4 for float) and verify it against the real file size BEFORE any allocation;
  also validate `D == expected`, `document_count == expected`, `total_vectors == offsets[n]`, and
  monotonic non-negative offsets. Any mismatch → degrade to an empty store (never crash / never
  over-allocate), matching the `.vec`/`.qvec` corruption + size-bomb tests.
- **Separate `.mvec` store** (not crammed into `.vec`/`.qvec`): different shape (ragged vs fixed),
  different dimension, rerank-only. Separation keeps V1–V4 byte-identical and untouched.

### Store interface
```cpp
class ANT_multivector_store {
public:
    static ANT_multivector_store *load(const char *filename, long long dimension,
                                       long long expected_documents);   // forgiving, validated
    long long get_dimension(void);
    long long document_count(void);
    long has(long long docid);                     // M_d > 0
    long long vector_count(long long docid);       // M_d
    // MaxSim of a query matrix (num_query_vecs × dimension, normalized) against this doc's M_d
    // stored vectors; reconstructs int8 to float internally. 0 if the doc has no multi-vectors.
    double maxsim(long long docid, const float *query_vecs, long long num_query_vecs);
    ~ANT_multivector_store();                       // frees pool/counts/offsets/qmin/qmax
};

class ANT_multivector_store_writer {              // buffered, atomic finish (temp + rename)
public:
    enum { QUANT_OFF = 0, QUANT_INT8 = 1 };
    long create(const char *filename, long long dimension);
    void set_quantization(int mode);
    long append(const float *vectors, long long num_vectors);  // one doc's M_d×D; NULL/0 = absent
    long finish(void);
    void abandon(void);
};
```

## 3. MaxSim kernel

For a candidate doc with `M_d` stored vectors and a query of `N` vectors (all `D`-dim, normalized):
```
MaxSim(Q, doc) = Σ_{i=1..N}  max_{j=1..M_d}  dot(q_i, v_j)
```
- int8 pool: reconstruct each `v_j` to float, then dot with each `q_i`. Rerank runs over only
  `first_stage_n` candidates, so reconstruct cost is negligible; `float` config gives exact scores.
- Cost `O(N · M_d · D)` per candidate; `first_stage_n` bounds total latency (predictable).
- A standalone brute-force float `maxsim` reference (no quantization, no store) backs the unit test.

## 4. Two-stage retrieval flow & config

```cpp
long long search_rerank(char *query_text, const float *query_docvec,
                        const float *query_multivec, long long num_query_vecs,
                        long long first_stage_n, long long top_k);
```
1. **Stage 1** dispatches existing retrieval by which inputs are present:
   `query_text && query_docvec` → `search_hybrid`; else `query_docvec` → `search_vector`; else
   `query_text` → `search`. Produces up to `first_stage_n` candidates (across the live memory
   buffer + all disk segments, exactly as today).
2. **Stage 2** computes `maxsim` for each candidate that has multi-vectors (segment store or the
   live writer buffer), sorts by MaxSim descending, publishes `top_k` through the shared `results[]`
   plumbing. **Graceful degradation:** candidates with `M_d == 0` retain their stage-1 relative
   order and are appended AFTER all reranked candidates, so a partially-populated index still
   returns sensible results. `top_k` is clamped to `first_stage_n`.

- `rerank.config` (magic `"ANTRR001"`, version, dimension `D`, quant mode) — written once on first
  `set_rerank_config`, IMMUTABLE thereafter (same pattern as `quantization.config`/`hnsw.config`);
  defensive parse on load (garbage → rerank disabled). Normalization is always on and therefore not
  stored.
- Setters/queries on `ATIRE_segment_index`: `set_rerank_config(long long dimension, long quant_mode)`
  — requires the index open (a `directory`), set before the first flush; does **not** require the
  single-vector `set_vector_config` (rerank can rescore a lexical-only first stage). `D` is the
  multi-vector dimension and is independent of any doc-level vector dimension. `rerank_configured()`.
- **No backfill method** (contrast `build_quantized`): multi-vectors cannot be reconstructed
  without re-embedding, so they must be supplied at index time. Documented explicitly; enabling
  rerank on an index whose existing segments have no `.mvec` simply reranks nothing until new docs
  carry multi-vectors (graceful degradation applies).

## 5. API surface

### C++ (`ATIRE_segment_index`)
- `long long add_document(const char *key, const char *document, const float *docvec,
   const float *multivec, long long num_vecs)` — new overload; `multivec` is `num_vecs·D` flat,
   normalized on ingest; `num_vecs == 0` / `multivec == NULL` means the doc has no multi-vectors.
- Matching `update_document(...)` overload (upsert).
- `search_rerank(...)` as in §4; results read via `get_hit(i)` like every other search.
- `set_rerank_config(...)`, `rerank_configured()`.

### Node (`nodejs/addon/segment_index.cpp`)
- Constructor option `rerank: { dimension: number, quantize?: 'int8' | 'float' }` — applied in
  `open()` before first flush via `set_rerank_config` (non-fatal on failure, mirroring the
  hnsw/quantize options).
- `addDocument(key, text, docVector, multiVectors)` where `multiVectors` is `Array<Float32Array>`
  (ragged, `M × D`); the addon validates each row length == `D` and flattens. `updateDocument`
  overload likewise.
- `searchRerank(queryMultiVectors, { text?, vector?, firstStageN = 100, topK = 10 })` →
  `[{ key, score, generation, docid }]` (same hit shape as other searches; `score` is the MaxSim).
- `.d.ts` additions and a "Late-interaction reranking (V5)" README section modeled on the V2/V3/V4
  sections (what it does, `Array<Float32Array>` multi-vector shape, `searchRerank`, that
  multi-vectors are supply-at-index-time with no backfill).

## 6. Lifecycle & compaction

`.mvec` rides all seven sidecar lifecycle sites like `.vec`/`.vsig`/`.hnsw`/`.qvec`:
1. **flush** — write `seg_G.mvec` from a new NRT writer buffer (`writer_multivector_data` +
   `writer_multivector_counts`, parallel to `writer_vector_data`; grows geometrically; reset on
   flush), quantized per `rerank.config`.
2. **append_segment (load)** — load into `segment.multivectors` when `rerank_configured()`.
3. **destructor** — `delete segment.multivectors` in the teardown loop.
4. **delete_segment_files** — unlink `seg_G.mvec`.
5. **orphan sweep** — already extension-agnostic (`seg_*`), no change.
6. **compaction rewrite** — the variable-length case: allocate a merged writer; for each surviving
   output document in output order, resolve its `(input, src_docid)` via the existing
   `ANT_docid_renumberer`, read that doc's `M_src` vectors from the input `.mvec`, and `append`
   them; tombstoned/absent docs contribute nothing. Offsets/counts are rebuilt by the writer. The
   merged `.mvec` is then loaded into the output segment's cache before the step-6 shuffle (mirrors
   how `.vsig`/`.hnsw` refresh `output_segment` before it is invalidated).
7. **compaction input-free** — `delete inputs[i]->multivectors` at the input-drop step.

NRT search: `search_rerank` must also MaxSim-score live-buffer candidates using the writer
multi-vector buffer (a small helper mirroring `scan_live_buffer_exact`), so reranking works before
the first flush.

## 7. Testing

- **Unit (`tests/test_multivector_store.cpp`):** ragged round-trip (varying `M_d`, incl. `M_d=0`
  docs) with int8 error bound; `maxsim` vs the brute-force float reference; load validation —
  corrupt magic, truncated file, and a size-bomb header (huge `total_vectors` with nothing behind
  it) all degrade to empty; destructor/ASan clean.
- **Integration (`tests/test_segment_index.cpp`):**
  - A crafted case where stage-1 ranks a target doc low but its multi-vectors make MaxSim rank it
    top — proving rerank actually changes the ordering (not a pass-through).
  - Rerank across the live buffer + multiple disk segments.
  - Delete + `compact()` preserves multi-vectors: `search_rerank` returns the same top-k before and
    after compaction (score-level equality where the pool is float; membership where int8).
  - Coexistence: hybrid stage 1 (BM25 + single-vector ANN) feeding MaxSim rerank, alongside V2/V3/V4
    configured on the same index.
  - float vs int8 parity (float pool exact; int8 within error bound) and determinism.
- **Node (`nodejs/test/rerank.test.js`):** `searchRerank` end-to-end with `Array<Float32Array>`
  multi-vectors; ragged rows; rerank changes order vs plain `searchVector`.

## 8. Out of scope / future (Vn)

- Token-ANN / PLAID stage-1 retrieval over the `.mvec` store (opt-in future mode; shares store +
  kernel).
- HNSW/approx as the `search_rerank` stage-1 source.
- Product quantization (PQ) for the multi-vector pool (V4 scalar int8 is the V5 default; PQ would
  cut memory further and is a natural follow-up given multi-vector volume).
- Any server/MCP exposure — separate effort.

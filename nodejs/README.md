
# antelope-search

## For Developer

### SWIG

New node versions are supported.


## Usage

### Disk-based search (standard)

Index files to disk, then open the index file for searching:

```javascript
var antelope = require('antelope-search');

// Index
var indexer = new antelope.ATIRE_indexer();
antelope.initialize(indexer, { nologo: null, "output-index": "myindex" }, ["file1.xml", "file2.xml"]);
indexer.index();
indexer.finish();  // writes index to disk

// Search
var engine = antelope.create_engine("-nologo -findex myindex");
engine.start();

var hits = engine.search("my query");
engine.next_result();
console.log(engine.result_to_json());

engine.finish();
```

### In-memory search (no disk I/O)

Index documents and search without ever writing the index to disk.
This is the preferred mode for long-running Node.js servers that build
their index at startup.

```javascript
var antelope = require('antelope-search');

// 1. Build the index in memory
var indexer = new antelope.ATIRE_indexer();
antelope.initialize(indexer, { nologo: null });  // no output-index option

// Index individual documents programmatically:
//   indexer.index_document(filename, content)
// Or point at files/folders and call:
//   indexer.index();

// 2. Open a search engine directly from the in-memory index.
//    Do NOT call indexer.finish() — that would write to disk.
var engine = antelope.create_engine_from_indexer(indexer);

// 3. Search as usual
var hits = engine.search("my query");
while (engine.next_result()) {
    console.log(engine.result_to_json());
}

engine.finish();
```

`create_engine_from_indexer(indexer, opts)` transfers ownership of the
in-memory index from the indexer to the search engine.  The indexer
must not be used for further indexing after this call.

#### Low-level API

If you need direct control:

```javascript
var engine = new antelope.ATIRE_API_server();
engine.set_params("-nologo");
var rc = engine.open_from_indexer(indexer);  // 0 = success
if (rc !== 0) throw new Error("open_from_indexer failed: " + rc);
```


## Prebuilt Library

In the `bin/` folder there are prebuilt shared libraries for Linux
(`libantelope_core.so`, `libantelope_api.so`, etc.).  The Node.js
addon (`antelope_api.node`) must be compiled against these with
node-gyp for your Node version.


### Building the addon

```bash
cd nodejs
./build.sh       # release build (requires node-gyp and Node ≤ 14)
```

To regenerate the SWIG wrapper after C++ API changes:

```bash
cd nodejs
./swig.sh        # produces antelope_wrap.cxx
```


### Tools

- `indexer.js` — command-line indexer
- `server.js`  — example REST search server (disk-based)
- `antelope.js` — interactive command-line search client


## SegmentIndex (modern Node-API binding)

`SegmentIndex` is a self-contained Node-API (N-API) addon that combines
lexical (BM25-style) and vector search over a segment-based, mutable index.
Unlike the legacy `antelope_api.node` addon above, it does not depend on the
prebuilt `.so` files under `bin/` and does not require Node ≤ 14 — it targets
any Node ≥ 12.22 (any release with stable N-API support).

Because `nodejs/index.js` (the package's legacy `main` entry) still throws
synchronously on Node versions newer than 14 (a version guard predating this
binding), `require('antelope-search')` itself is currently unusable on
modern Node. Until that legacy guard is relaxed, load the addon directly:

```js
const { SegmentIndex } = require('antelope-search/build/Release/antelope_segment.node');
```

`nodejs/index.js` also exposes a lazy `SegmentIndex` export, defined (via
`Object.defineProperty`) on the final `module.exports` object at the bottom
of the file. It takes effect whenever the file evaluates to completion —
i.e. on legacy Node (≤ 14), where `require('antelope-search').SegmentIndex`
works alongside the legacy API without loading the segment addon until first
access. On modern Node the version guard throws before that point, so use
the direct `build/Release/antelope_segment.node` require shown above.

### Building

```bash
make -C .. all        # build the engine once (from the repo root)
cd nodejs
npm install
npm run build:segment
```

### Usage

```js
const { SegmentIndex } = require('antelope-search');

const index = new SegmentIndex({ dimension: 768, metric: 'cosine' });
index.open('/var/data/myindex');

index.addDocument('page-1', '<DOC>how to feed a quokka</DOC>', embedding1);
index.addDocument('page-2', '<DOC>quokka habitat</DOC>');          // lexical-only

const hits = index.searchHybrid('quokka food', queryEmbedding, 10);
// [{ key: 'page-1', score: ..., generation: 1, docid: 0 }, ...]

await index.flush();      // persist
await index.maintain();   // compact
index.close();
```

### Options

| Option | Type | Default | Notes |
| --- | --- | --- | --- |
| `dimension` | `number` | vectors disabled | enables vector search; 1-65536 |
| `metric` | `'dot' \| 'cosine' \| 'l2'` | `'dot'` | vector similarity metric |
| `flushThreshold` | `number` | engine default | docs per auto-flush; `0` = manual only |
| `mergeFactor` | `number` | engine default | segments per compaction merge |
| `tombstoneRatio` | `number` | engine default | dead-doc ratio that triggers compaction |
| `autoMaintain` | `boolean` | `false` | run `maintain()` opportunistically |
| `durable` | `boolean` | `false` | opt-in write-ahead log: writes since the last `flush()` survive a crash (process exit without `flush()`), replayed automatically on the next `open()` |
| `walFsync` | `boolean` | `false` | `fsync()` every WAL append (not just `fflush()`) for stronger durability at the cost of write latency; only meaningful with `durable: true` |
| `globalStats` | `boolean` | `true` | cross-segment N / mean-document-length ranking statistics; set `false` to rank each segment on its own local statistics instead |
| `approximate` | `{ bits?, multiplier? }` | disabled | opt-in approximate vector search (see below) |
| `hnsw` | `{ M?, efConstruction?, efSearch? }` | disabled | opt-in HNSW graph vector search (see below) |
| `quantize` | `'int8' \| { mode }` | disabled | opt-in int8 vector quantization (see below) |
| `rerank` | `{ dimension, quantize? }` | disabled | opt-in late-interaction (MaxSim) reranking (see below) |

### Approximate vector search (V2)

For large vector indexes you can trade a little recall for a lot of speed with
SimHash signatures: each document vector is reduced to a compact bit-signature,
and a search prefilters candidates by cheap Hamming distance before an exact
rerank of the survivors.

Enable it with the `approximate` constructor option:

```js
const index = new SegmentIndex({
  dimension: 768,
  metric: 'cosine',
  approximate: { bits: 256, multiplier: 4 }
});
index.open('/var/data/myindex');
```

- `bits` — signature width in bits (default `256`). Persisted on first enable and immutable afterwards.
- `multiplier` — recall/speed knob (default `4`): the prefilter keeps `multiplier * k` candidates to rerank. Higher = better recall, slower.

`await index.buildSignatures()` backfills signature sidecars for any existing
segments that don't have them yet (idempotent) — call it once after enabling
approximate on an index that already has data.

Then query with the approximate variants, which mirror `searchVector` /
`searchHybrid` exactly:

```js
const hits  = index.searchVectorApprox(queryEmbedding, 10);
const mixed = index.searchHybridApprox('quokka food', queryEmbedding, 10);
```

Approximate search only applies to the `cosine` and `dot` metrics; an `l2`
index (or one where `approximate` was never configured) transparently falls
back to exact results, so these methods are always safe to call.

### HNSW graph search (V3)

For higher-recall approximate search, each segment can maintain a Hierarchical
Navigable Small World (HNSW) proximity graph and answer nearest-neighbour
queries by greedily traversing it instead of scanning every vector.

Enable it with the `hnsw` constructor option:

```js
const index = new SegmentIndex({
  dimension: 768,
  metric: 'cosine',
  hnsw: { M: 16, efConstruction: 200, efSearch: 64 }
});
index.open('/var/data/myindex');
```

- `M` — graph connectivity, neighbours per node (default `16`). Persisted on first enable and immutable afterwards.
- `efConstruction` — build-time candidate list size (default `200`); higher = better graph quality, slower build.
- `efSearch` — query-time candidate list size (default `64`); a recall/speed knob, higher = better recall, slower.

`await index.buildHnsw()` backfills the HNSW graph for any existing segments
that don't have one yet (idempotent) — call it once after enabling `hnsw` on an
index that already has data.

Then query with the HNSW variants, which mirror `searchVector` /
`searchHybrid` exactly:

```js
const hits  = index.searchVectorHnsw(queryEmbedding, 10);
const mixed = index.searchHybridHnsw('quokka food', queryEmbedding, 10);
```

HNSW search only applies to the `cosine` and `l2` metrics; a `dot` index (or
one where `hnsw` was never configured) transparently falls back to exact
results, so these methods are always safe to call.

### Quantized vectors (V4)

To shrink the on-disk (and page-cache) footprint of vector segments, each
document vector's float32 components can be quantized to int8, giving
roughly a 4x reduction in vector storage at the cost of some precision.

Enable it with the `quantize` constructor option:

```js
const index = new SegmentIndex({
  dimension: 768,
  metric: 'cosine',
  quantize: 'int8'
});
index.open('/var/data/myindex');
```

- `quantize: 'int8'` (equivalently `{ mode: 'replace' }`) — replace mode: only
  the int8 `.qvec` sidecar is written; the float32 `.vec` is never persisted.
  Lossy, smallest footprint.
- `quantize: { mode: 'exact' }` — keeps the float32 `.vec` alongside the int8
  `.qvec` so searches can rerank against exact vectors; costs roughly 1.25x
  the RAM/disk of an unquantized index instead of the ~4x reduction of
  replace mode, in exchange for no precision loss.

Quantization mode is index-wide and must be set before the first `flush()`;
like `hnsw`/`approximate` it is persisted on first enable and immutable
afterwards (attempting to change it later is a non-fatal no-op — quantization
just stays at whatever mode was already persisted).

`await index.buildQuantized()` backfills the int8 `.qvec` sidecar for any
existing float `.vec` segments that don't have one yet (idempotent) — call it
once after enabling `quantize` on an index that already has data.

Quantization needs **no new search methods** — `searchVector`,
`searchVectorApprox`, `searchVectorHnsw`, and their `Hybrid` counterparts all
work transparently against quantized segments exactly as they do against
unquantized ones.

### Late-interaction reranking (V5)

For the highest-fidelity vector relevance, each document can carry several
per-token (or per-chunk) embeddings — a "multi-vector" — instead of just one
whole-document vector. A rerank query supplies its own multi-vector and scores
each candidate document by MaxSim: for every query vector, take its best
matching document vector, then sum those best-matches across all query
vectors. This captures fine-grained term/phrase-level matches that a single
pooled document vector washes out, at the cost of being expensive enough that
it's only run as a second-stage rerank over a first-stage candidate pool
(lexical, vector, or hybrid), not as the primary retrieval step.

Enable it with the `rerank` constructor option:

```js
const index = new SegmentIndex({
  dimension: 768,
  metric: 'cosine',
  rerank: { dimension: 128, quantize: 'int8' }
});
index.open('/var/data/myindex');
```

- `dimension` — the width of each row in a document's multi-vector (independent
  of the top-level `dimension`, which is the single pooled vector's width).
  Persisted on first enable and immutable afterwards.
- `quantize` — `'int8'` (default) or `'float'`: whether the multi-vector rows
  are stored quantized or as full float32. Persisted on first enable and
  immutable afterwards.

Supply multi-vectors **at index time**, as a ragged-friendly array of rows —
each row a `Float32Array` (or `number[]`) of exactly `rerank.dimension`
elements — via an optional 4th argument to `addDocument`/`updateDocument`:

```js
index.addDocument('doc-1', '<DOC>...</DOC>', pooledVector, [
  new Float32Array(128 /* token 1 embedding */),
  new Float32Array(128 /* token 2 embedding */),
  // ... one row per token/chunk
]);
```

There is **no backfill** for multi-vectors — unlike `approximate`/`hnsw`/
`quantize`, which have `buildSignatures`/`buildHnsw`/`buildQuantized` to
retrofit existing documents, a document indexed without multi-vectors stays
without them; `searchRerank` keeps such candidates in their stage-1 order
after the reranked ones rather than scoring them.

Query with `searchRerank(queryMultiVectors, options)`:

```js
const hits = index.searchRerank(
  [ new Float32Array(128 /* query token 1 */), new Float32Array(128 /* query token 2 */) ],
  { vector: queryPooledVector, firstStageN: 100, topK: 10 }
);
```

- `queryMultiVectors` — the query's own multi-vector, same row shape as at
  index time.
- `options.text` / `options.vector` — optional stage-1 retrieval inputs
  (lexical / vector; both may be given for hybrid stage-1, mirroring
  `searchHybrid`).
- `options.firstStageN` — how many stage-1 candidates to rerank (default `100`).
- `options.topK` — how many reranked hits to return (default `10`).

### Filtered search + payloads

Attach structured attributes to documents and constrain any search to the
documents whose attributes satisfy a JSON predicate. You can also stash an
opaque per-document payload blob that is returned on every matching hit.

Declare the attribute schema up front with the `attributes` constructor option.
Each value is a type token: `'int64'`, `'string'`, or `'bool'` (scalar), or
`'int64[]'` / `'string[]'` (multi-valued). `'bool[]'` is not allowed — bool is
scalar only. The schema is immutable once the index is opened.

```js
const index = new SegmentIndex({
  dimension: 128,
  metric: 'cosine',
  attributes: { tenant: 'string', lang: 'string[]', rank: 'int64', keep: 'bool' }
});
index.open(dir);
```

Supply attributes and/or a payload via a trailing options object on
`addDocument` / `updateDocument` (the argument after `multiVectors`):

```js
index.addDocument('doc-1', '<DOC>hello world</DOC>', vec, null, {
  attributes: { tenant: 'acme', lang: ['en', 'fr'], rank: 5, keep: true },
  payload: 'arbitrary bytes'          // Buffer or string
});
```

Multi-valued fields take an array; scalar fields take a single value. The
`payload` is a `Buffer` or a `string`; it is copied verbatim and returned on
hits (see below).

Pass a `filter` in the options object of any search method. The predicate is a
JSON tree with exactly one operator key per node:

| Operator | Shape | Meaning |
| --- | --- | --- |
| `eq` | `{ eq: { field: value } }` | field equals value (int64/string/bool; CONTAINS for multi-valued) |
| `in` | `{ in: { field: [v1, v2, …] } }` | field matches any listed value (int64 or string) |
| `range` | `{ range: { field: { gte?, gt?, lte?, lt? } } }` | int64 field within bounds (at most one lower + one upper) |
| `and` | `{ and: [ … ] }` | all sub-predicates match |
| `or` | `{ or: [ … ] }` | any sub-predicate matches |
| `not` | `{ not: … }` | sub-predicate does not match |

```js
// one tenant only
index.searchVector(qvec, 10, { filter: { eq: { tenant: 'acme' } } });

// keep === true AND (lang contains 'fr')
index.search('report', 10, {
  filter: { and: [ { eq: { keep: true } }, { in: { lang: ['fr'] } } ] }
});

// rank >= 3
index.searchHybrid('report', qvec, 10, { filter: { range: { rank: { gte: 3 } } } });

// negation
index.searchVector(qvec, 10, { filter: { not: { eq: { tenant: 'acme' } } } });
```

The `filter` option is accepted by `search`, `searchVector`, `searchHybrid`,
`searchVectorApprox`, `searchHybridApprox`, `searchVectorHnsw`,
`searchHybridHnsw`, and `searchRerank`. A filter requires an attribute schema
(else the call throws), and a field/type mismatch — e.g. a number for a string
field, or a `range` on a non-int64 field — throws a `TypeError`.

Every hit that has a payload carries it as a `Buffer`:

```js
const hits = index.searchVector(qvec, 10, { filter: { eq: { tenant: 'acme' } } });
for (const h of hits)
  if (h.payload) console.log(h.key, h.payload.toString('utf8'));
```

TypeScript definitions are provided in `segment_index.d.ts`.

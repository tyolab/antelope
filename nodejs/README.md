
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

TypeScript definitions are provided in `segment_index.d.ts`.

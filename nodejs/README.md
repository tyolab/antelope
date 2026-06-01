
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

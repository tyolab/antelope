# antelope (Python binding)

Python binding for the [Antelope/ATIRE](https://github.com/tyolab/antelope) search
engine, plus an MCP server (added in a later layer).

The extension (`antelope._core`) is a pybind11 module that links directly against
the engine static archive (`lib/libantelope_engine.a`) and the bundled external
archives (zlib, bzip2, lzo, snappy, snowball).

## Requirements

- Python 3.10+
- A C++ toolchain (the engine and its externals are compiled from source)
- GNU make (the build driver invokes `make all` + `make engine_lib` in the repo
  root automatically — you do not need to run it yourself)

## Install (build from source)

There is no PyPI release. Build the extension from a checkout of the repo:

```sh
pip install ./python
# or, for development:
pip install -e ./python
```

The first build compiles the whole engine and its external archives, so expect it
to take several minutes.

## Test

```sh
python -m pytest python/tests -v
```

## API reference

`import antelope` exposes two names: `antelope.SegmentIndex` (the engine handle)
and `antelope.Hit` (a search-result row). Full signatures live in
`antelope/_core.pyi` (this package ships `py.typed`, so type checkers pick up
the stub automatically).

### Opening an index

All engine options (dimension, metric, attribute schema, approximate/HNSW/
quantization/rerank config, durability, merge policy, …) are passed as keyword
arguments to the constructor and applied when `open()` is called:

```python
import antelope

ix = antelope.SegmentIndex(dimension=4, metric="cosine")
ix.open("/path/to/index")      # creates the directory layout if new, reopens if existing
...
ix.close()
```

`SegmentIndex` is also a context manager — `close()` is called automatically on
`__exit__`:

```python
with antelope.SegmentIndex(dimension=4, metric="cosine") as ix:
    ix.open("/path/to/index")
    ...
```

Common constructor options:

| option | type | meaning |
|---|---|---|
| `dimension` | `int` | vector dimension (1–65536); omit for lexical-only |
| `metric` | `"dot"` \| `"cosine"` \| `"l2"` | vector distance metric |
| `attributes` | `dict[str, str]` | structured-attribute schema, e.g. `{"tenant": "string", "lang": "string[]", "rank": "int64", "keep": "bool"}` — append `"[]"` to a type for a multi-valued field |
| `approximate` | `dict` | `{"bits": int, "multiplier": int}` — SimHash prefilter config |
| `hnsw` | `dict` | `{"M": int, "ef_construction": int, "ef_search": int}` — HNSW graph config |
| `quantize` | `str` \| `dict` | `"int8"`/`"replace"`/`"exact"`, or `{"mode": ...}` — int8 vector quantization |
| `rerank` | `dict` | `{"dimension": int, "quantize": "float"\|"int8"}` — late-interaction multi-vector config |
| `flush_threshold`, `merge_factor`, `tombstone_ratio`, `auto_maintain`, `durable`, `wal_fsync`, `global_stats` | — | write-path / durability tuning |

Unknown keyword arguments are silently ignored.

### Writing documents

```python
handle = ix.add_document(
    "doc1",
    "<DOC>hello world</DOC>",
    vector=[0.1, 0.2, 0.3, 0.4],          # optional: list / tuple / array.array / numpy ndarray
    multi_vectors=[[0.1, 0.2, 0.3, 0.4]], # optional: per-token vectors, for search_rerank
    attributes={"tenant": "acme"},        # optional: requires an `attributes` schema
    payload=b"raw bytes",                 # optional: bytes or str, returned verbatim on Hit.payload
)
# handle == {"generation": int, "docid": int}

ix.update_document("doc1", "<DOC>hello world v2</DOC>", vector=[0.1, 0.2, 0.3, 0.5])  # upsert
ix.delete_document("doc1")   # -> bool: True if it existed

ix.flush()      # force the in-memory segment to disk (returns None; raises on failure)
ix.maintain()   # run the tiered merge/compaction policy to quiescence
```

Vectors accept anything satisfying the buffer/sequence protocol — a plain
`list`/`tuple`, `array.array('f', ...)`, or a `numpy.ndarray` all work; numpy
is **not** a hard dependency, it's just one accepted input shape.

### Backfill builders

These are idempotent — safe to call any time after `flush()`; each writes the
missing sidecar for every disk segment that needs it and no-ops otherwise.

```python
ix.build_signatures()  # .vsig SimHash sidecars, needed by search_vector_approx / search_hybrid_approx
ix.build_hnsw()        # .hnsw graph sidecars, needed by search_vector_hnsw / search_hybrid_hnsw
ix.build_quantized()   # .qvec int8 sidecars, needed when `quantize` is configured
```

### Search modes

Every search method returns `list[Hit]`, ordered best-first, and accepts an
optional `filter=` predicate (see below). `k` is the number of hits to return.

```python
ix.search("hello world", 10)                          # lexical (BM25/DFR)
ix.search_vector([0.1, 0.2, 0.3, 0.4], 10)             # exact vector, brute force
ix.search_vector_approx([0.1, 0.2, 0.3, 0.4], 10)      # SimHash-prefiltered approximate vector
ix.search_vector_hnsw([0.1, 0.2, 0.3, 0.4], 10)        # HNSW graph approximate vector
ix.search_hybrid("hello", [0.1, 0.2, 0.3, 0.4], 10)          # RRF fusion of lexical + exact vector
ix.search_hybrid_approx("hello", [0.1, 0.2, 0.3, 0.4], 10)   # RRF fusion of lexical + approx vector
ix.search_hybrid_hnsw("hello", [0.1, 0.2, 0.3, 0.4], 10)     # RRF fusion of lexical + HNSW vector

# late-interaction MaxSim rerank: text and/or vector select stage-1 candidates
# (first_stage_n of them), then query_multi_vectors reranks them via MaxSim to
# produce the final top k. At least one of text/vector is required.
ix.search_rerank(
    "hello", [0.1, 0.2, 0.3, 0.4], [[0.1, 0.2, 0.3, 0.4]],
    first_stage_n=50, k=10,
)
```

`Hit` fields:

| field | type | meaning |
|---|---|---|
| `key` | `str` | the document key passed to `add_document`/`update_document` |
| `score` | `float` | ranking score (semantics depend on search mode) |
| `generation` | `int` | segment generation the document currently lives in |
| `docid` | `int` | document id within that segment |
| `payload` | `Optional[bytes]` | the stored payload, or `None` if none was set |

### Filtered search

Every search method accepts `filter=` — a JSON-like predicate tree evaluated
against the index's `attributes` schema (configured at construction). A filter
requires an `attributes` schema; passing one on a schema-less index raises
`TypeError`.

Grammar (each node is a single-key dict `{operator: operand}`):

| operator | operand | meaning |
|---|---|---|
| `eq` | `{field: value}` | field equals value (on a multi-valued field: value is a member) |
| `in` | `{field: [values]}` | field equals one of the given values |
| `range` | `{field: {gte?, gt?, lte?, lt?}}` | int64 field falls within the bounds (at least one bound required; at most one of `gte`/`gt`, at most one of `lte`/`lt`) |
| `and` | `[node, ...]` | all sub-nodes match |
| `or` | `[node, ...]` | any sub-node matches |
| `not` | `node` | sub-node does not match |

Example:

```python
ix = antelope.SegmentIndex(
    dimension=4, metric="dot",
    attributes={"tenant": "string", "lang": "string[]", "rank": "int64", "keep": "bool"},
)
ix.open(tempdir)
ix.add_document("a", "<DOC>alpha</DOC>", vector=[1, 0, 0, 0],
                 attributes={"tenant": "acme", "lang": ["en"], "rank": 10, "keep": True})
ix.flush()

ix.search("alpha", 10, filter={
    "and": [
        {"eq": {"tenant": "acme"}},
        {"range": {"rank": {"gte": 5, "lt": 100}}},
        {"not": {"eq": {"keep": False}}},
    ],
})
```

### Error mapping

| condition | exception |
|---|---|
| bad constructor option (bad `dimension`/`metric`/`attributes` spec/`quantize` mode/…) | `ValueError` |
| `open()` failure (bad/corrupt directory, vector config mismatch) | `RuntimeError` |
| any operation before `open()` or after `close()` | `RuntimeError` ("index is not open") |
| wrong vector length, or a zero vector under the `cosine` metric | `ValueError` |
| malformed/mis-typed `filter` (unknown field, wrong operand type, unknown operator, filter given without an `attributes` schema) | `TypeError` |
| `search_rerank` called with neither `text` nor `vector` | `ValueError` |
| mis-typed `attributes`/`payload` on `add_document`/`update_document` (unknown field, wrong value type for the field's declared type) | `TypeError` |
| `attributes`/`payload` given but the index has no `attributes` schema | `ValueError` |

`python/tests/test_errors.py` is the regression lock for this table.

## MCP server

`antelope.mcp` is a [FastMCP](https://github.com/modelcontextprotocol/python-sdk)
stdio server that exposes a single open index to LLM agents/clients over the
[Model Context Protocol](https://modelcontextprotocol.io). Install the `mcp`
extra to get it:

```sh
pip install "./python[mcp]"
```

### Launching

```sh
antelope-mcp --index /path/to/index              # read-only
antelope-mcp --index /path/to/index --writable   # + mutation tools
# or:
python -m antelope.mcp --index /path/to/index
```

The index directory can also come from the `ANTELOPE_INDEX` environment
variable instead of `--index`. The server speaks stdio, so it's meant to be
launched as a subprocess by an MCP client (Claude Desktop, an agent framework,
the `mcp` Python SDK, …) — it does not listen on a network port.

### Tools and resources (read-only, default)

| tool | signature | returns |
|---|---|---|
| `search` | `(query: str, k: int = 10, filter: dict \| None = None)` | `list[{key, score, payload?}]` — lexical search, same `filter` grammar as `SegmentIndex.search` |
| `document_count` | `()` | `int` |
| `index_info` | `()` | `{attributes, config}` — the attribute schema plus `{dimension, metric, document_count}` |

| resource | contents |
|---|---|
| `antelope://index/schema` | `{attributes, config}` (same shape as `index_info`) |

### Mutation tools (`--writable` only)

`add_document`, `update_document`, `delete_document`, `flush`, `maintain` —
lexical-only (no `vector`/`multi_vectors` parameters exposed), mirroring
`SegmentIndex`'s write surface. Absent entirely unless the server is started
with `--writable`.

Payloads come back on `search` hits as UTF-8 text under `payload` when the
stored bytes decode cleanly, or base64 under `payload_b64` otherwise.

### Client example

```python
import asyncio, sys
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

params = StdioServerParameters(
    command=sys.executable, args=["-m", "antelope.mcp", "--index", "/path/to/index"],
)

async def main():
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = await session.list_tools()
            result = await session.call_tool("search", {"query": "apple", "k": 5})
            hits = result.structuredContent["result"]  # [{key, score, ...}, ...]

asyncio.run(main())
```

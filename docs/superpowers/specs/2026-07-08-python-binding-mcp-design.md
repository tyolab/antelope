# Python Binding + MCP Server Design

**Status:** approved 2026-07-08, ready for implementation planning.

**Goal:** A first-class, `pip`-installable Python binding for the Antelope/ATIRE search engine
(`ATIRE_segment_index`) mirroring the Node addon's surface, plus a thin MCP (Model Context
Protocol) server built on top that exposes lexical + filtered search to LLM agents.

**Architecture (one sentence):** A hand-written **pybind11** extension (`antelope._core`) wraps the
C++ `ATIRE_segment_index`, statically linked against the existing `lib/libantelope_engine.a` + the
external archives (the same objects the Node addon links); a small `antelope.mcp` server using the
official `mcp` Python SDK depends only on that binding, opening one index at startup and exposing
read-only search tools by default with mutation tools behind `--writable`.

**Tech stack:** C++ engine (`source/`, `atire/`), pybind11 (build-time dep), setuptools, the `mcp`
Python SDK (FastMCP), pytest. Python 3.10+.

---

## 1. Scope, layers & decomposition

Two layers, built and tested in order (binding first, MCP on top):

- **Layer 1 — `antelope` package (pybind11 extension).** First-class standalone: full engine
  surface, usable without MCP. Compiled extension `antelope._core` + a thin pure-Python
  `antelope/__init__.py` re-export with docstrings and type stubs.
- **Layer 2 — `antelope.mcp` server.** Uses Layer 1 only (never touches C++ directly). Opens one
  index, exposes MCP tools/resources over stdio.

**Boundary contract:** Layer 1 has no knowledge of MCP; Layer 2 calls only the public `antelope`
API. Each is independently testable.

**Out of scope (future):** PyPI wheels / prebuilt binaries (build-from-source only, mirroring the
Node binding's npm-publish decision); an embedding step in the MCP server (vector/hybrid search from
plain text — deferred, see §4); HTTP/SSE MCP transport (stdio only); multiple named indexes per MCP
server (one index at startup); async-native binding methods (sync + GIL-release is the idiom).

## 2. Python binding surface (`SegmentIndex`)

Mirrors the Node `SegmentIndex`, Pythonic (snake_case). Constructor takes keyword options:

```
SegmentIndex(
    dimension=None, metric='cosine'|'dot'|'l2',
    flush_threshold=None, merge_factor=None, tombstone_ratio=None, auto_maintain=None,
    approximate={'bits':…, 'multiplier':…},
    hnsw={'M':…, 'ef_construction':…, 'ef_search':…},
    quantize='int8'|'replace'|'exact' | {'mode':…},
    rerank={'dimension':…, 'quantize':'float'|'int8'},
    attributes={'tenant':'string', 'lang':'string[]', 'rank':'int64', 'keep':'bool'},
    durable=None, wal_fsync=None, global_stats=None,
)
```

- **Lifecycle:** `open(directory)`, `close()`, context manager (`__enter__`/`__exit__` → open/close),
  `document_count() -> int`, `vector_dimension() -> int`.
- **Write:** `add_document(key, text, vector=None, multi_vectors=None, attributes=None,
  payload=None)` → a handle dict `{'generation': int, 'docid': int}` (as the Node addon returns),
  `update_document(...)` (same return), `delete_document(key) -> bool`, `flush()`, `maintain()`,
  `build_signatures()`, `build_hnsw()`, `build_quantized()`.
- **Search (all six modes):** `search(text, k, filter=None)`, `search_vector(vector, k,
  filter=None)`, `search_vector_approx(...)`, `search_vector_hnsw(...)`,
  `search_hybrid(text, vector, k, filter=None)`, `search_hybrid_approx(...)`,
  `search_hybrid_hnsw(...)`, `search_rerank(text, vector, query_multi_vectors, first_stage_n, k,
  filter=None)`. Each returns `list[Hit]`.
- **Vectors:** accept a `list[float]` **or** any contiguous float32 buffer (numpy `ndarray`,
  `array.array`) via the buffer protocol. numpy is an optional convenience, never a hard dependency.
  Wrong length → `ValueError`. Multi-vectors: a sequence of such rows.
- **Filter:** a plain `dict` in the Node JSON grammar —
  `{'and':[…]} | {'or':[…]} | {'not':{…}} | {'eq':{field:value}} |
   {'range':{field:{'gte'|'gt'?:…, 'lte'|'lt'?:…}}} | {'in':{field:[…]}}` — translated to
  `ANT_filter` in C++ (a direct port of the Node `json_node_to_filter`, type-checked against the
  index schema). Unknown field / type mismatch / malformed → `TypeError`.
- **Hits:** a lightweight `Hit` (a `NamedTuple`/dataclass): `key: str`, `score: float`,
  `generation: int`, `docid: int`, `payload: bytes | None`.
- **Threading:** the long C++ calls (search/flush/maintain/build) release the GIL
  (`py::gil_scoped_release`), so Python threads / `asyncio.to_thread` stay responsive. All methods
  are synchronous.

## 3. Packaging & build

Location `python/` (sibling to `nodejs/`):

```
python/
  pyproject.toml            # build-system: setuptools + pybind11; console script antelope-mcp
  src/antelope_core.cpp     # hand-written pybind11 glue (SegmentIndex wrap + dict->ANT_filter)
  antelope/
    __init__.py             # re-export SegmentIndex, Hit; docstrings
    _core.pyi               # type stubs; py.typed marker
    mcp/                     # Layer 2 (added in the MCP tasks)
      __init__.py
      server.py             # FastMCP server + tools/resources
      __main__.py           # `antelope-mcp` entry (argparse: --index, --writable)
  tests/                    # pytest (binding + MCP)
```

- **Build:** `setuptools` + `Pybind11Extension`. A custom `build_ext` first runs `make engine_lib`
  (and the external static libs) in the repo root — mirroring the Node addon's dependency on the
  `make`-produced archive — then compiles `antelope_core.cpp` and links `lib/libantelope_engine.a`
  plus `{z, bz2, lzo2, snappy, stemmer}` via `extra_objects`, with include dirs `source/`, `atire/`
  (+ external headers). The whole repo already compiles `-fPIC`, so the archive links straight into
  the extension `.so`.
- **Install:** `pip install ./python` (or `-e ./python`). PyPI/wheels out of scope. Python 3.10+.
- **Dependencies:** build-time `pybind11`; runtime none required (numpy optional). MCP extra:
  `pip install ./python[mcp]` pulls the `mcp` SDK.

## 4. MCP server (`antelope.mcp`)

- **SDK/transport:** official `mcp` Python SDK (FastMCP), **stdio** transport.
- **Launch:** `antelope-mcp --index <dir> [--writable]` (index path also via `ANTELOPE_INDEX`).
  Opens the one index at startup, reading its on-disk config (`vector.config`, `attributes.config`,
  …). Search-only means the engine reads existing config from disk; the server needs only the
  directory.
- **Read-only tools (always registered):**
  - `search(query: str, k: int = 10, filter: dict | None = None) -> list[dict]` — lexical + optional
    structured filter; returns `[{key, score, payload?}]`. The primary tool.
  - `document_count() -> int`.
- **Resource `antelope://index/schema`** (mirrored as an `index_info` tool for tool-only clients):
  the attribute schema (field names, types, multi-valued flags) + index config (dimension, metric,
  which vector tiers are present), so the agent knows **which fields it can filter on and how to
  shape a filter**.
- **Writable tools (registered only with `--writable`):** `add_document(key, text, attributes=None,
  payload=None)` (no vector — an LLM agent has no embeddings), `update_document(...)`,
  `delete_document(key)`, `flush()`, `maintain()`. In read-only mode these are **absent** from the
  tool list (not erroring stubs).
- **Concurrency:** async handlers call the sync binding via `asyncio.to_thread` (GIL released in
  C++), keeping the event loop responsive on larger operations.
- **Result payloads:** returned as a UTF-8 string when the opaque payload decodes cleanly, else a
  base64 field; hits stay concise (`key`, `score`, optional `payload`).

## 5. Error handling

- **Binding:** `open()` failure (bad/corrupt dir, config mismatch) → `RuntimeError`; bad constructor
  options / metric / wrong vector length → `ValueError`; malformed or mis-typed `filter` (unknown
  field, type mismatch) → `TypeError`; `search_rerank` with no first-stage inputs → `ValueError`
  (the engine's both-NULL SIGSEGV guard is already in place); any op before `open()` / after
  `close()` → `RuntimeError` ("index is not open", mirroring the Node `require_open`).
- **MCP:** binding exceptions become MCP tool errors with a clear message (a bad filter names the
  offending field); writable tools are simply absent in read-only mode.

## 6. Testing

- **Binding (`python/tests/`, pytest — mirrors the Node `test/` suite):** construct + open in a
  tmpdir; add docs (lexical / vector / attributes / payload / multi-vectors); exercise all six
  search modes; every filter predicate form; payload round-trip as `bytes`; flush + reopen;
  maintain/compaction; delete/tombstone; filter type-errors raise; the rerank guard; buffer-protocol
  vector input (`list` + numpy-if-present). Plus an import/link smoke test. Assertions mirror the
  C++/JS suites.
- **MCP (`python/tests/`):** unit tests calling the tool functions directly (search returns hits;
  schema resource lists fields; mutation tools present iff `--writable`); an integration test
  driving the server over stdio with the `mcp` client SDK (list tools, call `search`, read the schema
  resource, assert the `--writable` gating).
- **Build/doc note:** `make engine_lib` is a prerequisite; `pip install ./python && pytest
  python/tests` is the developer loop; a `python/README.md` documents install + usage + the MCP
  launch.

## 7. Implementation sequencing

One spec; the plan builds **Layer 1 first** (binding compiles, imports, full pytest green) **then
Layer 2** (MCP server + its tests). Layer 1 is a shippable milestone on its own.

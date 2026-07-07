# Python Binding + MCP Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a first-class `pip`-installable `antelope` Python binding (pybind11 wrap of `ATIRE_segment_index`, full engine surface) and a thin `antelope.mcp` server exposing lexical + filtered search over MCP.

**Architecture:** A hand-written pybind11 extension `antelope._core` links the existing `lib/libantelope_engine.a` + external archives (the same objects the Node addon links; whole repo is `-fPIC`). `antelope/__init__.py` re-exports it. A `antelope.mcp` FastMCP server (stdio) depends only on the binding — read-only search + schema by default, mutation tools behind `--writable`.

**Tech Stack:** C++ engine (`source/`, `atire/`), pybind11 (build dep), setuptools, `mcp` Python SDK, pytest. Python 3.10+.

**Reference authority:** `nodejs/addon/segment_index.cpp` is the authoritative surface being mirrored — it already contains the constructor option handling, the six search methods, and the C++ translators `json_node_to_filter` (line 531), `parse_filter_option` (762), `build_attribute_set` (794). Port those to pybind11 (`py::object`/`py::dict` instead of `Napi::Value`). Engine C++ method signatures are as declared in `atire/atire_segment_index.h`.

**Milestones:** binding imports + links after Task 1; full binding surface + pytest-green after Task 10 (Layer 1 shippable); MCP server after Task 14.

**Build discipline:** `make engine_lib` (repo root) is the prerequisite that produces `lib/libantelope_engine.a`; the extension's `build_ext` runs it. Whole repo is `-fPIC`. NO PyPI/wheels (build-from-source, like the Node addon). numpy optional (buffer protocol), never required.

---

## Option-name mapping (Node camelCase → Python snake_case)

| Node option | Python kwarg | applied via (see `Open()` in the Node addon for order) |
|---|---|---|
| `dimension`,`metric` | `dimension`,`metric` | `set_vector_config(dim, metric)` **before** `open()` |
| `flushThreshold` | `flush_threshold` | `set_flush_threshold` |
| `mergeFactor` | `merge_factor` | `set_merge_factor` |
| `tombstoneRatio` | `tombstone_ratio` | `set_tombstone_compact_ratio` |
| `autoMaintain` | `auto_maintain` | `set_auto_maintain` |
| `approximate={bits,multiplier}` | `approximate={'bits','multiplier'}` | `set_approximate_config`,`set_candidate_multiplier` (after open) |
| `hnsw={M,efConstruction,efSearch}` | `hnsw={'M','ef_construction','ef_search'}` | `set_hnsw_config`,`set_ef_search` (after open) |
| `quantize` | `quantize` | `set_quantization` (after open) |
| `rerank={dimension,quantize}` | `rerank={'dimension','quantize'}` | `set_rerank_config` (after open) |
| `attributes={field:'type'}` | `attributes={field:'type'}` | `set_attributes_config` (after open) |
| `durable`,`walFsync`,`globalStats` | `durable`,`wal_fsync`,`global_stats` | `set_durable` (before open), `set_wal_fsync`,`set_global_stats` |

`metric` ∈ `'dot'|'cosine'|'l2'` → `VECTOR_METRIC_DOT/COSINE/L2`. `quantize` ∈ `'int8'|'replace'|'exact'` or `{'mode':…}`. Metric/quant enum values: read `atire/atire_segment_index.h`.

---

# LAYER 1 — the `antelope` pybind11 binding

## Task 1: package scaffold + build that imports (link smoke)

**Files:**
- Create: `python/pyproject.toml`, `python/setup.py`, `python/src/antelope_core.cpp`, `python/antelope/__init__.py`, `python/tests/test_import.py`, `python/README.md`

- [ ] **Step 1: Failing test** — `python/tests/test_import.py`:
```python
def test_import_and_version():
    import antelope
    assert isinstance(antelope.__version__, str)
    from antelope import SegmentIndex
    assert SegmentIndex is not None
```

- [ ] **Step 2: `python/pyproject.toml`**
```toml
[build-system]
requires = ["setuptools>=64", "pybind11>=2.11"]
build-backend = "setuptools.build_meta"

[project]
name = "antelope"
version = "0.1.0"
description = "Python binding for the Antelope/ATIRE search engine + an MCP server"
requires-python = ">=3.10"
dependencies = []

[project.optional-dependencies]
mcp = ["mcp>=1.0"]

[project.scripts]
antelope-mcp = "antelope.mcp.__main__:main"

[tool.setuptools]
packages = ["antelope", "antelope.mcp"]
```

- [ ] **Step 3: `python/setup.py`** — build_ext that builds the engine archive then links it. `REPO_ROOT` is the parent of `python/`.
```python
import os, subprocess, sys
from setuptools import setup
from pybind11.setup_helpers import Pybind11Extension, build_ext

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LIBDIR = os.path.join(REPO, "lib")
EXTERNALS = [  # same static archives the Node binding links; paths per GNUmakefile
    os.path.join(LIBDIR, "libantelope_engine.a"),
]
# external compression/stemmer archives (discover concrete paths from GNUmakefile EXTRA_OBJS)
for rel in [
    "external/gpl/zlib-1.2.11/libz.a",
    "external/gpl/bzip/bzip2-1.0.6/libbz2.a",
    "external/gpl/lzo/lzo-2.06/src/.libs/liblzo2.a",
    "external/gpl/snappy/snappy-1.1.0/.libs/libsnappy.a",
    "external/gpl/snowball/libstemmer.a",
]:
    p = os.path.join(REPO, rel)
    EXTERNALS.append(p)

class make_then_build(build_ext):
    def run(self):
        subprocess.check_call(["make", "engine_lib"], cwd=REPO)  # produces lib/libantelope_engine.a + externals
        super().run()

ext = Pybind11Extension(
    "antelope._core",
    ["src/antelope_core.cpp"],
    include_dirs=[os.path.join(REPO, "source"), os.path.join(REPO, "atire")],
    extra_objects=[p for p in EXTERNALS],   # archives resolved at build time
    cxx_std=11,
)
setup(cmdclass={"build_ext": make_then_build}, ext_modules=[ext])
```
NOTE: the exact external archive relative paths must be confirmed against `GNUmakefile` (`grep -n 'EXTRA_OBJS +=' GNUmakefile` shows `$(ZLIB_DIR)/libz.a` etc. and their `*_DIR` definitions). Use whatever the Node `binding.gyp` links — read `nodejs/binding.gyp` `libraries`/`ldflags` for the authoritative archive list, and mirror it exactly.

- [ ] **Step 4: `python/src/antelope_core.cpp`** — minimal module proving the link:
```cpp
#include <pybind11/pybind11.h>
#include "atire_segment_index.h"   // pulls the engine header (via -I atire)
namespace py = pybind11;

PYBIND11_MODULE(_core, m) {
    m.attr("__doc__") = "Antelope engine binding (pybind11)";
    // link smoke: instantiate + destroy the engine class so the archive symbols must resolve
    m.def("_link_check", []() { ATIRE_segment_index ix; (void)ix; return true; });
}
```

- [ ] **Step 5: `python/antelope/__init__.py`**
```python
from ._core import _link_check  # noqa: F401  (real SegmentIndex added in later tasks)
__version__ = "0.1.0"

class SegmentIndex:  # placeholder replaced in Task 2 by the bound class
    pass
```

- [ ] **Step 6: build + verify** — `pip install -e ./python` (from repo root) then `pytest python/tests/test_import.py -v` → PASS. (`-e` triggers `make engine_lib` + compiles+links the extension; a link failure here means the archive/externals list is wrong — fix Step 3.)

- [ ] **Step 7: `python/README.md`** — a stub: install (`make engine_lib && pip install ./python`), Python 3.10+, build-from-source.

- [ ] **Step 8: Commit** — `feat(py): package scaffold + pybind11 link smoke`.

## Task 2: `SegmentIndex` construct / open / close / context manager / counts

**Files:** Modify `python/src/antelope_core.cpp`, `python/antelope/__init__.py`; Test `python/tests/test_lifecycle.py`.

- [ ] **Step 1: Failing test** `python/tests/test_lifecycle.py`:
```python
import tempfile, antelope

def test_open_close_counts():
    d = tempfile.mkdtemp()
    ix = antelope.SegmentIndex()
    assert ix.open(d) is None or ix.open(d) is not False  # open() returns None on success
    assert ix.document_count() == 0
    assert ix.vector_dimension() == 0
    ix.close()

def test_context_manager():
    d = tempfile.mkdtemp()
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(d)
        assert ix.vector_dimension() == 4

def test_op_before_open_raises():
    ix = antelope.SegmentIndex()
    import pytest
    with pytest.raises(RuntimeError):
        ix.document_count()
```

- [ ] **Step 2: verify fail** — `pytest python/tests/test_lifecycle.py -v` → FAIL.

- [ ] **Step 3: Implement** — replace the module with a real `SegmentIndex` `py::class_`. Store the constructor kwargs on the C++ object (an options struct), apply them in `open()` in the SAME order as the Node addon's `Open()` (`set_vector_config` pre-open; the rest post-open — read `nodejs/addon/segment_index.cpp` `Open()` verbatim for ordering). Bind:
```cpp
py::class_<PySegmentIndex>(m, "SegmentIndex")
  .def(py::init([](py::kwargs kw){ return new PySegmentIndex(kw); }))
  .def("open", &PySegmentIndex::open, py::arg("directory"))
  .def("close", &PySegmentIndex::close)
  .def("document_count", &PySegmentIndex::document_count)
  .def("vector_dimension", &PySegmentIndex::vector_dimension)
  .def("__enter__", [](PySegmentIndex &s){ return &s; })
  .def("__exit__", [](PySegmentIndex &s, py::object, py::object, py::object){ s.close(); return false; });
```
`PySegmentIndex` wraps a `ATIRE_segment_index *engine` (heap). A `require_open()` helper throws `py::value_error`/`std::runtime_error` — map the Node `require_open` ("index is not open") to `throw std::runtime_error(...)` (pybind → `RuntimeError`). Constructor parses kwargs into an options struct (accept `dimension`, `metric`, `flush_threshold`, `merge_factor`, `tombstone_ratio`, `auto_maintain`, `approximate`, `hnsw`, `quantize`, `rerank`, `attributes`, `durable`, `wal_fsync`, `global_stats`); a bad `metric` string → `std::invalid_argument` (→ `ValueError`). `open()` returns `None` on success, raises `RuntimeError` on engine `open()!=0`.

- [ ] **Step 4: `python/antelope/__init__.py`** — `from ._core import SegmentIndex` (remove the placeholder class).

- [ ] **Step 5: verify pass** — rebuild (`pip install -e ./python`) + `pytest python/tests/test_lifecycle.py -v` → PASS.

- [ ] **Step 6: Commit** — `feat(py): SegmentIndex construct/open/close/context-manager`.

## Task 3: lexical `add_document` + `search` + `Hit`

**Files:** Modify `python/src/antelope_core.cpp`, `python/antelope/__init__.py`; Test `python/tests/test_lexical.py`.

- [ ] **Step 1: Failing test**:
```python
import tempfile, antelope
def test_lexical_add_search():
    with antelope.SegmentIndex() as ix:
        ix.open(tempfile.mkdtemp())
        h = ix.add_document("doc1", "<DOC>alpha beta</DOC>")
        assert h["generation"] >= 1 and h["docid"] >= 0
        ix.add_document("doc2", "<DOC>gamma</DOC>")
        hits = ix.search("alpha", 10)
        assert len(hits) == 1
        assert hits[0].key == "doc1"
        assert isinstance(hits[0].score, float)
        assert hits[0].payload is None
```

- [ ] **Step 2: verify fail.**

- [ ] **Step 3: Implement** — a `Hit` `collections.namedtuple`-equivalent exposed from C++ via a small `py::class_<Hit>` OR a Python `namedtuple` populated in a `hits_to_list` helper. Simplest: define `Hit` as a Python `namedtuple("Hit", ["key","score","generation","docid","payload"])` in `__init__.py`, and have the C++ `search` return a `py::list` of `py::dict`? NO — to keep the namedtuple, bind C++ methods to return a `std::vector` of a small struct exposed as `Hit` via pybind. Chosen: bind a `Hit` struct:
```cpp
struct Hit { std::string key; double score; long long generation, docid; py::object payload; };
py::class_<Hit>(m, "Hit")
  .def_readonly("key", &Hit::key).def_readonly("score", &Hit::score)
  .def_readonly("generation", &Hit::generation).def_readonly("docid", &Hit::docid)
  .def_readonly("payload", &Hit::payload)
  .def("__repr__", [](const Hit&h){ return "<Hit "+h.key+">"; });
```
`hits_to_list(engine)` walks `engine->get_hit(i)` for `i<count`, building `Hit`s; `payload` = `py::bytes((const char*)hit->payload, hit->payload_length)` when `payload_length>0` else `py::none()`. `add_document(key, text)` → `engine->add_document(key.c_str(), text.c_str())`, returns `{"generation":..,"docid":..}` by decoding the handle (`gen = handle>>40`, `docid = handle & ((1LL<<40)-1)`) — or expose the raw handle; match the Node addon's returned shape. `search(text, k)` copies text to a writable `std::string` (engine mutates), releases the GIL around `engine->search(&buf[0], k)`, returns `hits_to_list`.

- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(py): lexical add_document + search + Hit`.

## Task 4: vectors — `add_document(vector=)`, `search_vector`, `search_hybrid`, buffer protocol

**Files:** Modify `python/src/antelope_core.cpp`; Test `python/tests/test_vectors.py`.

- [ ] **Step 1: Failing test**:
```python
import array, tempfile, antelope
def test_vector_and_hybrid():
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("near", "<DOC>x</DOC>", vector=[1,0,0,0])
        ix.add_document("far",  "<DOC>y</DOC>", vector=array.array("f",[0.1,0,0,0]))
        hv = ix.search_vector([1,0,0,0], 2)
        assert hv[0].key == "near"
        hh = ix.search_hybrid("x", [1,0,0,0], 2)
        assert len(hh) >= 1
```

- [ ] **Step 3: Implement** — a `extract_vector(py::object, dim)` helper accepting: a `list`/`tuple` of numbers → build `std::vector<float>`; any object exposing the buffer protocol (`py::buffer`, incl. numpy `float32` and `array.array('f')`) → `request()` and copy `dim` floats (validate length == dim → `std::invalid_argument`/`ValueError`; a non-float32 buffer is copied element-wise from a `double` view or rejected — accept float32 fast-path, else convert via `py::cast<std::vector<float>>`). `add_document(key, text, vector=None, multi_vectors=None, attributes=None, payload=None)` — this task wires `vector` only (multi_vectors/attributes/payload default None, added in Tasks 6/7). `search_vector(vector, k, filter=None)` and `search_hybrid(text, vector, k, filter=None)` — filter param accepted but wired in Task 8; call `engine->search_vector(vec, k)` / `engine->search_hybrid(text, vec, k)` with the GIL released. Mirror the Node `extract_vector` cosine-zero-vector rejection.

- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(py): vector + hybrid search, buffer-protocol vectors`.

## Task 5: approximate + HNSW modes + backfill builders

**Files:** Modify `python/src/antelope_core.cpp`; Test `python/tests/test_approx_hnsw.py`.

- [ ] **Step 1: Failing test** — construct with `approximate={'bits':64}` and (separately) `hnsw={'M':16,'ef_construction':100}` on a cosine index; add ~30 vectors; assert `search_vector_approx(q,10)` and `search_vector_hnsw(q,10)` and `search_hybrid_approx`/`search_hybrid_hnsw` each return ≥1 hit; assert `build_signatures()`/`build_hnsw()` run without error.
- [ ] **Step 3: Implement** — bind `search_vector_approx`, `search_vector_hnsw`, `search_hybrid_approx`, `search_hybrid_hnsw` (each `(…, k, filter=None)`, GIL-released, mapping to the same-named engine methods) and `build_signatures()`, `build_hnsw()`. Ensure the constructor applies `approximate`/`hnsw` options in `open()` (Task 2 wired the option struct; here confirm `set_approximate_config`/`set_candidate_multiplier`/`set_hnsw_config`/`set_ef_search` are called post-open per the Node `Open()`).
- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(py): approximate + HNSW search modes + backfill builders`.

## Task 6: rerank + multi-vectors + quantization

**Files:** Modify `python/src/antelope_core.cpp`; Test `python/tests/test_rerank.py`.

- [ ] **Step 1: Failing test** — construct with `rerank={'dimension':4}` (+ `quantize='exact'`) on a cosine index; `add_document(..., vector=v, multi_vectors=[v])`; assert `search_rerank("text", q, [q], first_stage_n=20, k=5)` returns ≥1 hit; `build_quantized()` runs.
- [ ] **Step 3: Implement** — `extract_multivectors(py::object, dim)` (a sequence of rows, each an `extract_vector`) → flat `std::vector<float>` + count. Wire `multi_vectors` in `add_document`/`update_document`. Bind `search_rerank(text, vector, query_multi_vectors, first_stage_n, k, filter=None)` (GIL-released) → `engine->search_rerank(text_ptr, vec, qmv, num_qv, first_stage_n, k [, filter])`; guard both-text-and-vector-None per the Node addon (`ValueError`). Bind `build_quantized()`. Confirm `rerank`/`quantize` options applied in `open()`.
- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(py): rerank/MaxSim + multi-vectors + quantization`.

## Task 7: attributes schema + `add_document(attributes=, payload=)` + payload on hits

**Files:** Modify `python/src/antelope_core.cpp`; Test `python/tests/test_attributes.py`.

- [ ] **Step 1: Failing test**:
```python
import tempfile, antelope
def test_attributes_and_payload():
    with antelope.SegmentIndex(dimension=4, metric="dot",
                               attributes={"tenant":"string","lang":"string[]","rank":"int64","keep":"bool"}) as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("d0","<DOC>x</DOC>", vector=[1,0,0,0],
                        attributes={"tenant":"acme","lang":["en","fr"],"rank":5,"keep":True},
                        payload=b"hello")
        ix.flush()
        hits = ix.search_vector([1,0,0,0], 5)
        assert hits[0].payload == b"hello"
```
- [ ] **Step 3: Implement** — port the Node `build_attribute_set` (segment_index.cpp:794) to pybind: from a `py::dict` `attributes` + a `payload` (`bytes` or `str`), build an `ANT_attribute_set` against `engine->attribute_schema()` (int/string/bool + `[]`-multi per the schema; a JS→py type mismatch → `TypeError`). The constructor's `attributes` kwarg builds the `ANT_attribute_schema` and `set_attributes_config` runs in `open()` (a `'bool[]'` or bad type → `ValueError`). Thread the `ANT_attribute_set*` into the 6-arg `add_document`/`update_document`. `hits_to_list` already emits `payload` bytes (Task 3).
- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(py): attributes schema + attribute/payload ingest + payload hits`.

## Task 8: dict → `ANT_filter` translator + `filter=` on all searches

**Files:** Modify `python/src/antelope_core.cpp`; Test `python/tests/test_filter.py`.

- [ ] **Step 1: Failing test**:
```python
import tempfile, pytest, antelope
def test_filtered_search_and_errors():
    with antelope.SegmentIndex(dimension=4, metric="dot",
                               attributes={"tenant":"string","rank":"int64"}) as ix:
        ix.open(tempfile.mkdtemp())
        for i,(t,r) in enumerate([("acme",1),("beta",2),("acme",3)]):
            ix.add_document(f"d{i}","<DOC>apple</DOC>", vector=[1,0,0,0],
                            attributes={"tenant":t,"rank":r})
        ix.flush()
        h = ix.search_vector([1,0,0,0], 10, filter={"eq":{"tenant":"acme"}})
        assert {x.key for x in h} == {"d0","d2"}
        h2 = ix.search("apple", 10, filter={"and":[{"eq":{"tenant":"acme"}},{"range":{"rank":{"gte":2}}}]})
        assert {x.key for x in h2} == {"d2"}
        with pytest.raises(TypeError):
            ix.search("apple", 10, filter={"eq":{"tenant":123}})   # number for string field
        with pytest.raises(TypeError):
            ix.search("apple", 10, filter={"range":{"tenant":{"gte":1}}})  # range on string
```
- [ ] **Step 3: Implement** — port the Node `json_node_to_filter` (segment_index.cpp:531) + `parse_filter_option` (762) to pybind, walking a `py::dict`/`py::list` instead of `Napi::Value`, using the SAME `and_list`/`or_list`/`not_`/`eq_*`/`range_int`/`in_*` factories + `build(schema)` validation. A malformed/mis-typed predicate → `throw py::type_error(...)` (→ `TypeError`); an unknown field → `TypeError`. Thread the resulting `ANT_filter*` (built or NULL) into EVERY search method's `filter=None` param (`search`, `search_vector`, `search_vector_approx`, `search_vector_hnsw`, `search_hybrid`, `search_hybrid_approx`, `search_hybrid_hnsw`, `search_rerank`); `filter=None` → the unfiltered overload. `delete` the filter after the search (RAII/guard on the C++ side).
- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(py): dict->ANT_filter translator + filtered search everywhere`.

## Task 9: `update_document` / `delete_document` / `flush` / `maintain` + reopen

**Files:** Modify `python/src/antelope_core.cpp`; Test `python/tests/test_mutation.py`.

- [ ] **Step 1: Failing test** — add docs across two flushes; `update_document` changes a body (search reflects it); `delete_document` returns `True` and drops the doc; `maintain()` runs; close + reopen a new `SegmentIndex` on the same dir sees the persisted state.
- [ ] **Step 3: Implement** — bind `update_document(key, text, vector=None, multi_vectors=None, attributes=None, payload=None)` (6-arg engine `update_document`), `delete_document(key) -> bool` (`engine->delete_document(key)==0`), `flush()` and `maintain()` (GIL-released, `RuntimeError` on nonzero engine return). Reopen works because engine reads config from disk.
- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(py): update/delete/flush/maintain + reopen`.

## Task 10: type stubs, docstrings, error-mapping polish, README (Layer 1 milestone)

**Files:** Create `python/antelope/_core.pyi`, `python/antelope/py.typed`; Modify `python/antelope/__init__.py`, `python/src/antelope_core.cpp`, `python/README.md`; Test `python/tests/test_errors.py`.

- [ ] **Step 1: Failing test** `test_errors.py` — assert: bad `metric="xyz"` → `ValueError`; `open("/nonexistent/\0bad")` or a corrupt dir → `RuntimeError`; wrong-length vector → `ValueError`; op after `close()` → `RuntimeError`; `search_rerank(None, None, ...)` → `ValueError`.
- [ ] **Step 3: Implement** — audit every binding's error path to the mapping (`RuntimeError`/`ValueError`/`TypeError`) from spec §5. Add `_core.pyi` stubs for `SegmentIndex` (all methods, `Hit`) + `py.typed` marker; `__init__.py` exports `SegmentIndex, Hit, __version__` with a module docstring. Fill `python/README.md`: install, the full API with a short example per search mode, filtered search, attributes/payload.
- [ ] **Step 4: verify** — `pip install -e ./python && pytest python/tests -v` → ALL green (Layer 1 complete). **Step 5: Commit** — `feat(py): error mapping, type stubs, docs (Layer 1 milestone)`.

---

# LAYER 2 — the `antelope.mcp` server

## Task 11: MCP server scaffold + read-only `search` + `document_count`

**Files:** Create `python/antelope/mcp/__init__.py`, `python/antelope/mcp/server.py`, `python/antelope/mcp/__main__.py`, `python/tests/test_mcp_tools.py`.

- [ ] **Step 1: Failing test** `test_mcp_tools.py` (unit — call the tool coroutine directly):
```python
import asyncio, tempfile, antelope
from antelope.mcp.server import build_server

def _seed(d):
    with antelope.SegmentIndex() as ix:
        ix.open(d); ix.add_document("d0","<DOC>apple</DOC>"); ix.flush()

def test_search_tool_readonly():
    d = tempfile.mkdtemp(); _seed(d)
    srv = build_server(index_dir=d, writable=False)
    hits = asyncio.run(srv.call_tool_for_test("search", {"query":"apple","k":5}))
    assert hits and hits[0]["key"] == "d0"
    assert asyncio.run(srv.call_tool_for_test("document_count", {})) == 1
```
(`build_server` returns a thin object exposing the FastMCP instance + a `call_tool_for_test` helper that invokes a registered tool's function; if FastMCP's API differs, adapt the helper to call the underlying function object directly.)

- [ ] **Step 3: Implement** `server.py` — `build_server(index_dir, writable)`:
```python
import asyncio, base64
from mcp.server.fastmcp import FastMCP
import antelope

def build_server(index_dir: str, writable: bool = False):
    ix = antelope.SegmentIndex()
    ix.open(index_dir)
    mcp = FastMCP("antelope")

    def _hit_json(h):
        out = {"key": h.key, "score": h.score}
        if h.payload is not None:
            try: out["payload"] = h.payload.decode("utf-8")
            except UnicodeDecodeError: out["payload_b64"] = base64.b64encode(h.payload).decode("ascii")
        return out

    @mcp.tool()
    async def search(query: str, k: int = 10, filter: dict | None = None) -> list[dict]:
        """Lexical + optional structured-filter search over the index."""
        hits = await asyncio.to_thread(ix.search, query, k, filter)
        return [_hit_json(h) for h in hits]

    @mcp.tool()
    async def document_count() -> int:
        """Number of live documents in the index."""
        return await asyncio.to_thread(ix.document_count)

    # writable tools + schema resource added in Tasks 12/13
    return _wrap(mcp, ix, writable, index_dir)
```
`_wrap` returns an object holding `mcp`, `ix`, `writable`, plus `call_tool_for_test(name, args)` (looks up the registered tool and awaits its function). `__main__.py`:
```python
import argparse, os
from .server import build_server
def main():
    p = argparse.ArgumentParser(prog="antelope-mcp")
    p.add_argument("--index", default=os.environ.get("ANTELOPE_INDEX"))
    p.add_argument("--writable", action="store_true")
    a = p.parse_args()
    if not a.index: p.error("--index (or ANTELOPE_INDEX) is required")
    srv = build_server(a.index, a.writable)
    srv.mcp.run()   # stdio transport
```

- [ ] **Step 4: verify** — `pip install -e "./python[mcp]" && pytest python/tests/test_mcp_tools.py -v` → PASS. **Step 5: Commit** — `feat(mcp): FastMCP server + read-only search/document_count`.

## Task 12: `antelope://index/schema` resource + `index_info` tool

**Files:** Modify `python/antelope/mcp/server.py`, `python/src/antelope_core.cpp` (add a `schema()` introspection method if absent); Test `python/tests/test_mcp_schema.py`.

- [ ] **Step 1: Failing test** — build a server on an index created with `attributes={"tenant":"string","lang":"string[]"}`; assert reading the `antelope://index/schema` resource (and/or the `index_info` tool) returns the field list with names/types/multi and the index config (dimension, metric).
- [ ] **Step 3: Implement** — the binding needs an introspection method: bind `SegmentIndex.schema() -> list[dict]` returning `[{"name","type","multi"}]` from `engine->attribute_schema()` (`attributes_configured()` false → `[]`), and `SegmentIndex.info() -> dict` (`{"dimension","metric","document_count", ...}`). In `server.py` register:
```python
@mcp.resource("antelope://index/schema")
def schema() -> dict:
    return {"attributes": ix.schema(), "config": ix.info()}
@mcp.tool()
async def index_info() -> dict:
    return {"attributes": ix.schema(), "config": ix.info()}
```
- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(mcp): schema resource + index_info tool`.

## Task 13: writable tools gated on `--writable`

**Files:** Modify `python/antelope/mcp/server.py`; Test `python/tests/test_mcp_writable.py`.

- [ ] **Step 1: Failing test** — `build_server(writable=False)`: the tool registry does NOT contain `add_document`/`delete_document`/`flush`/`maintain`. `build_server(writable=True)`: it DOES, and calling `add_document`+`flush` then `search` reflects the new doc; `document_count` increments.
- [ ] **Step 3: Implement** — inside `build_server`, `if writable:` register `add_document(key, text, attributes=None, payload=None)` (no vector — MCP is lexical), `update_document(...)`, `delete_document(key) -> bool`, `flush()`, `maintain()`, each `await asyncio.to_thread(ix.<method>, ...)`. When not writable they are simply not registered. `call_tool_for_test` returns a clear KeyError/None for an unregistered tool so the test can assert absence.
- [ ] **Step 4: verify pass. Step 5: Commit** — `feat(mcp): writable mutation tools behind --writable`.

## Task 14: stdio integration test + MCP README section

**Files:** Create `python/tests/test_mcp_stdio.py`; Modify `python/README.md`.

- [ ] **Step 1: Failing test** `test_mcp_stdio.py` — launch `antelope-mcp --index <seeded dir>` as a subprocess over stdio and drive it with the `mcp` client SDK (`mcp.client.stdio`): list tools (assert `search`, `document_count`, `index_info` present; mutation tools ABSENT), call `search` (assert the seeded hit), read the `antelope://index/schema` resource. A second case launches with `--writable` and asserts the mutation tools are listed. Use `pytest.importorskip("mcp")` and skip cleanly if the SDK/loop is unavailable in the environment (report rather than fake).
- [ ] **Step 3: Implement** — no product code expected; the test exercises the real entry point. If it surfaces a bug (e.g., a tool signature the SDK rejects), fix `server.py`.
- [ ] **Step 4: verify** — `pytest python/tests/test_mcp_stdio.py -v` → PASS. Add a "MCP server" section to `python/README.md` (launch, `--writable`, tool list, an example client snippet). **Step 5: Commit** — `test(mcp): stdio integration + README`.

---

## Final review + finish
After Task 14: dispatch a holistic code review over the whole `python/` tree (binding memory-safety: GIL handling, `extract_vector`/`extract_multivectors` buffer bounds, `ANT_filter`/`ANT_attribute_set` ownership + free-on-error, `Hit.payload` bytes-copy lifetime; MCP: writable gating, error surfacing, `asyncio.to_thread` correctness). Fix Critical/Important with regression tests. Then finishing-a-development-branch (merge locally after `pytest python/tests` fully green on a clean `pip install ./python`).

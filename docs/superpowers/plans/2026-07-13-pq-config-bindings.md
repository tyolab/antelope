# Expose PQ Config in Node + Python Bindings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface the engine's dense + token product-quantization config in the Python (`python/src/antelope_core.cpp`) and Node (`nodejs/addon/segment_index.cpp`) bindings via constructor option bags `pq` / `multivectorPq` plus backfill build methods, at parity with the existing `rerank`/`quantize` surface.

**Architecture:** Each binding parses two optional constructor bags into `option_*` members, applies them non-fatally in the existing post-open config block (dense after `set_quantization`, token after `set_rerank_config`), and adds two build methods (`build_pq`/`build_multivector_pq` sync in Python; async `buildPq`/`buildMultivectorPq` via `MaintenanceWorker` in Node).

**Tech Stack:** pybind11 (`setup.py`, `pip install ./python`) + N-API/node-gyp (`npm run build:segment`). Engine setters in `atire/atire_segment_index.h`.

---

## Environment notes (read first)

- **Python** builds here (proven by the #13/#14/#15 work). Build: `pip3 install --force-reinstall ./python`. Run tests with the **`pytest` executable** from `python/` (NOT `python3 -m pytest` — cwd shadows the installed pkg with the `python/antelope/` source dir).
- **Node**: toolchain present (`node` v24, `npm`, `node-gyp`). Rebuild the addon with `cd nodejs && npm run build:segment` (runs `make -C .. engine_lib` + `node-gyp configure` + builds `antelope_segment.node`). Run a test file with `node --test nodejs/test/<file>.test.js` (uses `node:test`). If the addon cannot build/configure in this environment, implement + self-review the Node changes and report the Node test run as **environment-blocked** — do NOT fabricate a pass.
- No engine headers change here, so no `rm obj/*.o` dance for these edits.

## String → engine enum mappings (both bindings)

- `posture`: `"replace"`→`ATIRE_segment_index::PQ_POSTURE_REPLACE` (0), `"rerank"`→`PQ_POSTURE_RERANK` (1)
- `rerankQuant`/`rerank_quant`: `"float"`→`RERANK_QUANT_FLOAT` (0), `"int8"`→`RERANK_QUANT_INT8` (1)
- dense `residentTier`/`resident_tier`: `"float"`→`PQ_TIER_FLOAT` (0), `"int8"`→`PQ_TIER_INT8` (1), `"none"`→`PQ_TIER_NONE` (2)
- token `residentTier`/`resident_tier`: `"float"`→`MV_TIER_FLOAT` (0), `"none"`→`MV_TIER_NONE` (1)
- `m`: int, `0` ⇒ engine picks `default_pq_m`. `eager`: bool → `set_pq_policy(1|0)` / `set_multivector_pq_policy(1|0)`.

Defaults when the bag is present but a key is omitted: `m=0`, `posture="replace"`, `rerankQuant="float"`, `eager=false`, `residentTier` unset (skip the tier setter → engine FLOAT default). A bad enum string throws (`ValueError` Python / `TypeError` Node) at parse.

---

## Task 1: Python — `pq` / `multivectorPq` options + build methods

**Files:**
- Modify: `python/src/antelope_core.cpp` (members ~447-452, defaults ~479-484, ctor parse ~519-563, apply ~685-703, build defs ~1183, `.def` ~1233-1236)
- Test: `python/tests/test_pq_config.py` (new)

- [ ] **Step 1: Write the failing test**

Create `python/tests/test_pq_config.py`:

```python
import tempfile, pytest, antelope


def _dense_docs(ix, dim, n=40):
    import random
    random.seed(1)
    for i in range(n):
        v = [random.uniform(-1, 1) for _ in range(dim)]
        ix.add_document(f"d{i}", "<DOC>apple</DOC>", vector=v)


def test_pq_replace_build_and_search():
    dim = 8
    with antelope.SegmentIndex(dimension=dim, metric="cosine", pq={"m": 0, "posture": "replace"}) as ix:
        ix.open(tempfile.mkdtemp())
        _dense_docs(ix, dim)
        ix.flush()
        ix.build_pq()                      # backfill .pq
        q = [1.0] + [0.0] * (dim - 1)
        assert len(ix.search_vector(q, 5)) >= 1


def test_pq_rerank_tier_int8():
    dim = 8
    with antelope.SegmentIndex(dimension=dim, metric="cosine",
                               pq={"posture": "rerank", "resident_tier": "int8"}) as ix:
        ix.open(tempfile.mkdtemp())
        _dense_docs(ix, dim)
        ix.flush()
        ix.build_pq()
        assert len(ix.search_vector([1.0] + [0.0] * (dim - 1), 5)) >= 1


def test_multivector_pq_build_and_rerank():
    dim = 4
    with antelope.SegmentIndex(dimension=dim, metric="cosine",
                               rerank={"dimension": dim}, multivectorPq={"m": 0}) as ix:
        ix.open(tempfile.mkdtemp())
        for i in range(12):
            v = [i + 1, 1, 0, 0]
            ix.add_document(f"d{i}", "<DOC>apple</DOC>", vector=v, multi_vectors=[v])
        ix.flush()
        ix.build_multivector_pq()
        q = [1, 1, 0, 0]
        assert len(ix.search_rerank("apple", q, [q], first_stage_n=20, k=5)) >= 1


def test_pq_config_persists_on_reopen():
    dim = 8
    d = tempfile.mkdtemp()
    with antelope.SegmentIndex(dimension=dim, metric="cosine", pq={"m": 0, "posture": "replace"}) as ix:
        ix.open(d)
        _dense_docs(ix, dim)
        ix.flush()
        ix.build_pq()
    # reopen with the same bag; persisted pq.config honored, search still works
    with antelope.SegmentIndex(dimension=dim, metric="cosine", pq={"m": 0, "posture": "replace"}) as ix2:
        ix2.open(d)
        assert len(ix2.search_vector([1.0] + [0.0] * (dim - 1), 5)) >= 1


def test_pq_mutually_exclusive_with_int8_quantize_no_throw():
    dim = 8
    # quantize int8 + pq: PQ stays off (mutually exclusive) but construction/open must NOT throw
    with antelope.SegmentIndex(dimension=dim, metric="cosine", quantize="int8", pq={"posture": "replace"}) as ix:
        ix.open(tempfile.mkdtemp())
        _dense_docs(ix, dim)
        ix.flush()
        assert len(ix.search_vector([1.0] + [0.0] * (dim - 1), 5)) >= 1


def test_pq_bad_posture_raises():
    with pytest.raises(ValueError):
        antelope.SegmentIndex(dimension=8, metric="cosine", pq={"posture": "bogus"})
```

- [ ] **Step 2: Run it to watch it fail**

Run:
```bash
cd /data/tyolab/code/antelope && pip3 install --force-reinstall ./python >/dev/null 2>&1
cd python && pytest tests/test_pq_config.py -v
```
Expected: FAIL — `pq`/`multivectorPq` kwargs are silently ignored today (pybind accepts unknown kwargs? no — the ctor reads specific keys and ignores others), so `build_pq()` raises `AttributeError` (method doesn't exist) / the bad-posture test does NOT raise. (If unknown kwargs are simply ignored, `build_pq` missing is the first failure.)

- [ ] **Step 3: Add option_* members**

In `python/src/antelope_core.cpp`, after the existing option members (~line 452, after `option_rerank_quant`), add:
```cpp
	// #23 dense PQ
	bool option_pq_requested;
	long long option_pq_m;
	long option_pq_posture;
	long option_pq_rerank_quant;
	bool option_pq_tier_requested;
	long option_pq_tier;
	long option_pq_eager;
	// #23 token (multivector) PQ
	bool option_mvpq_requested;
	long long option_mvpq_m;
	long option_mvpq_posture;
	long option_mvpq_rerank_quant;
	bool option_mvpq_tier_requested;
	long option_mvpq_tier;
	long option_mvpq_eager;
```

- [ ] **Step 4: Add defaults**

In the ctor's default-init block (~line 484, after `option_rerank_quant = ...`), add:
```cpp
	option_pq_requested = false;
	option_pq_m = 0;
	option_pq_posture = ATIRE_segment_index::PQ_POSTURE_REPLACE;
	option_pq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_FLOAT;
	option_pq_tier_requested = false;
	option_pq_tier = ATIRE_segment_index::PQ_TIER_FLOAT;
	option_pq_eager = 0;
	option_mvpq_requested = false;
	option_mvpq_m = 0;
	option_mvpq_posture = ATIRE_segment_index::PQ_POSTURE_REPLACE;
	option_mvpq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_FLOAT;
	option_mvpq_tier_requested = false;
	option_mvpq_tier = ATIRE_segment_index::MV_TIER_FLOAT;
	option_mvpq_eager = 0;
```

- [ ] **Step 5: Parse the two bags**

In the ctor kwarg-parse block, after the `rerank` parse (~line 563), add. This uses a small local lambda-free inline parse (pybind dicts). Posture/quant/tier string→enum with a throw on bad value:
```cpp
	if (kw.contains("pq") && !kw["pq"].is_none())
		{
		py::dict p = kw["pq"].cast<py::dict>();
		option_pq_requested = true;
		if (p.contains("m")) option_pq_m = p["m"].cast<long long>();
		if (p.contains("posture"))
			{
			std::string s = p["posture"].cast<std::string>();
			if (s == "replace") option_pq_posture = ATIRE_segment_index::PQ_POSTURE_REPLACE;
			else if (s == "rerank") option_pq_posture = ATIRE_segment_index::PQ_POSTURE_RERANK;
			else throw py::value_error("pq.posture must be 'replace' or 'rerank'");
			}
		if (p.contains("rerank_quant"))
			{
			std::string s = p["rerank_quant"].cast<std::string>();
			if (s == "float") option_pq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_FLOAT;
			else if (s == "int8") option_pq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_INT8;
			else throw py::value_error("pq.rerank_quant must be 'float' or 'int8'");
			}
		if (p.contains("resident_tier"))
			{
			std::string s = p["resident_tier"].cast<std::string>();
			option_pq_tier_requested = true;
			if (s == "float") option_pq_tier = ATIRE_segment_index::PQ_TIER_FLOAT;
			else if (s == "int8") option_pq_tier = ATIRE_segment_index::PQ_TIER_INT8;
			else if (s == "none") option_pq_tier = ATIRE_segment_index::PQ_TIER_NONE;
			else throw py::value_error("pq.resident_tier must be 'float', 'int8', or 'none'");
			}
		if (p.contains("eager")) option_pq_eager = p["eager"].cast<bool>() ? 1 : 0;
		}
	if (kw.contains("multivectorPq") && !kw["multivectorPq"].is_none())
		{
		py::dict p = kw["multivectorPq"].cast<py::dict>();
		option_mvpq_requested = true;
		if (p.contains("m")) option_mvpq_m = p["m"].cast<long long>();
		if (p.contains("posture"))
			{
			std::string s = p["posture"].cast<std::string>();
			if (s == "replace") option_mvpq_posture = ATIRE_segment_index::PQ_POSTURE_REPLACE;
			else if (s == "rerank") option_mvpq_posture = ATIRE_segment_index::PQ_POSTURE_RERANK;
			else throw py::value_error("multivectorPq.posture must be 'replace' or 'rerank'");
			}
		if (p.contains("rerank_quant"))
			{
			std::string s = p["rerank_quant"].cast<std::string>();
			if (s == "float") option_mvpq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_FLOAT;
			else if (s == "int8") option_mvpq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_INT8;
			else throw py::value_error("multivectorPq.rerank_quant must be 'float' or 'int8'");
			}
		if (p.contains("resident_tier"))
			{
			std::string s = p["resident_tier"].cast<std::string>();
			option_mvpq_tier_requested = true;
			if (s == "float") option_mvpq_tier = ATIRE_segment_index::MV_TIER_FLOAT;
			else if (s == "none") option_mvpq_tier = ATIRE_segment_index::MV_TIER_NONE;
			else throw py::value_error("multivectorPq.resident_tier must be 'float' or 'none'");
			}
		if (p.contains("eager")) option_mvpq_eager = p["eager"].cast<bool>() ? 1 : 0;
		}
```

- [ ] **Step 6: Apply at open (non-fatal)**

In `open()`, after `set_rerank_config` (~line 701, before the attributes apply at 702), add:
```cpp
	if (option_pq_requested)
		{
		if (engine->set_pq_config(option_pq_m, option_pq_posture, option_pq_rerank_quant) == 0)
			{
			if (option_pq_tier_requested)
				engine->set_pq_resident_tier(option_pq_tier);
			engine->set_pq_policy(option_pq_eager);
			}
		}
	if (option_mvpq_requested)
		{
		if (engine->set_multivector_pq_config(option_mvpq_m, option_mvpq_posture, option_mvpq_rerank_quant) == 0)
			{
			if (option_mvpq_tier_requested)
				engine->set_multivector_resident_tier(option_mvpq_tier);
			engine->set_multivector_pq_policy(option_mvpq_eager);
			}
		}
```

- [ ] **Step 7: Add the two build methods**

Beside `build_quantized` (~line 1183) add:
```cpp
	void build_pq()
	{
	require_open();
	long rc;
	{
	py::gil_scoped_release release;
	rc = engine->build_pq();
	}
	if (rc != 0)
		throw std::runtime_error("build_pq failed (PQ not configured or no dense vectors)");
	}

	void build_multivector_pq()
	{
	require_open();
	long rc;
	{
	py::gil_scoped_release release;
	rc = engine->build_multivector_pq();
	}
	if (rc != 0)
		throw std::runtime_error("build_multivector_pq failed (token PQ not configured or no multi-vectors)");
	}
```
And register them beside `.def("build_quantized", ...)` (~line 1236):
```cpp
		.def("build_pq", &PySegmentIndex::build_pq)
		.def("build_multivector_pq", &PySegmentIndex::build_multivector_pq)
```

- [ ] **Step 8: Rebuild and run the test (now PASSES)**

Run:
```bash
cd /data/tyolab/code/antelope && pip3 install --force-reinstall ./python >/dev/null 2>&1
cd python && pytest tests/test_pq_config.py tests/test_vectors.py tests/test_rerank.py -v
```
Expected: PASS — all `test_pq_config.py` cases green; existing vector/rerank suites unchanged. (If the extension can't build, report environment-blocked — but it builds here.)

- [ ] **Step 9: Commit**

```bash
git add python/src/antelope_core.cpp python/tests/test_pq_config.py
git commit -m "feat(python): expose PQ config (pq/multivectorPq bags + build_pq) (#23)"
```

---

## Task 2: Node — `pq` / `multivectorPq` options + async build methods

**Files:**
- Modify: `nodejs/addon/segment_index.cpp` (members ~39-44, defaults ~114-119, ctor parse ~168-219, apply ~331-360, `MaintenanceWorker::Operation` enum ~1626, Execute switch ~1644-1650, OnError ~1660, build methods ~1776-1790, `InstanceMethod` ~1818-1820)
- Test: `nodejs/test/pq.test.js` (new)

- [ ] **Step 1: Write the failing test**

Create `nodejs/test/pq.test.js`:

```javascript
const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

function denseDocs(idx, dim, n = 40) {
  for (let i = 0; i < n; i++) {
    const v = new Float32Array(dim);
    for (let d = 0; d < dim; d++) v[d] = ((i * 16 + d) % 97) / 50 - 1;
    idx.addDocument('d' + i, '<DOC>apple ' + i + '</DOC>', v);
  }
}

test('pq: replace posture builds and searches', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_pq_'));
  const dim = 8;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', pq: { m: 0, posture: 'replace' } });
  idx.open(dir);
  denseDocs(idx, dim);
  await idx.flush();
  await idx.buildPq();
  const q = new Float32Array(dim); q[0] = 1;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

test('pq: rerank posture + int8 resident tier', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_pqr_'));
  const dim = 8;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', pq: { posture: 'rerank', residentTier: 'int8' } });
  idx.open(dir);
  denseDocs(idx, dim);
  await idx.flush();
  await idx.buildPq();
  const q = new Float32Array(dim); q[0] = 1;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

test('multivectorPq: builds and reranks', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_mvpq_'));
  const dim = 4;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', rerank: { dimension: dim }, multivectorPq: { m: 0 } });
  idx.open(dir);
  for (let i = 0; i < 12; i++) {
    const v = new Float32Array([i + 1, 1, 0, 0]);
    idx.addDocument('d' + i, '<DOC>apple</DOC>', v, [v]);
  }
  await idx.flush();
  await idx.buildMultivectorPq();
  const q = new Float32Array([1, 1, 0, 0]);
  assert.ok(idx.searchRerank('apple', q, [q], 20, 5).length >= 1);
  idx.close();
});

test('pq: config persists on reopen', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_pqp_'));
  const dim = 8;
  let idx = new SegmentIndex({ dimension: dim, metric: 'cosine', pq: { m: 0, posture: 'replace' } });
  idx.open(dir);
  denseDocs(idx, dim);
  await idx.flush();
  await idx.buildPq();
  idx.close();
  idx = new SegmentIndex({ dimension: dim, metric: 'cosine', pq: { m: 0, posture: 'replace' } });
  idx.open(dir);
  const q = new Float32Array(dim); q[0] = 1;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

test('pq: mutually exclusive with int8 quantize (stays off, no throw)', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_pqx_'));
  const dim = 8;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', quantize: 'int8', pq: { posture: 'replace' } });
  idx.open(dir);
  denseDocs(idx, dim);
  await idx.flush();
  const q = new Float32Array(dim); q[0] = 1;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

test('pq: bad posture throws', () => {
  assert.throws(() => new SegmentIndex({ dimension: 8, metric: 'cosine', pq: { posture: 'bogus' } }));
});
```

- [ ] **Step 2: Build the addon and run the test to watch it FAIL**

Run:
```bash
cd /data/tyolab/code/antelope/nodejs && npm run build:segment 2>&1 | tail -5
node --test test/pq.test.js
```
Expected: FAIL — `buildPq`/`buildMultivectorPq` are not functions, and the bad-posture test does not throw. (If the addon cannot build in this environment, STOP and report the Node build as environment-blocked — capture the exact `node-gyp`/`make` error; the changes are still to be written + self-reviewed for correctness.)

- [ ] **Step 3: Add option_* members**

In `nodejs/addon/segment_index.cpp`, after `option_rerank_quant` (~line 44), add the same 14 members as Python Task 1 Step 3 (identical C++ types — `bool option_pq_requested; long long option_pq_m; long option_pq_posture; ...` through `option_mvpq_eager`).

- [ ] **Step 4: Add defaults**

In the ctor default-init (~line 119, after `option_rerank_quant = ...`), add the same 14 default assignments as Python Task 1 Step 4 (identical enum constants).

- [ ] **Step 5: Parse the two bags (N-API)**

After the `rerank` parse (~line 219), add. N-API idiom (`options.Has("pq") && options.Get("pq").IsObject()`, `Napi::Object`, `.Get(key)`, `.ToString().Utf8Value()`, throw via `Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); return;`):
```cpp
if (options.Has("pq") && options.Get("pq").IsObject())
	{
	Napi::Object p = options.Get("pq").As<Napi::Object>();
	option_pq_requested = true;
	if (p.Has("m")) option_pq_m = (long long)p.Get("m").As<Napi::Number>().Int64Value();
	if (p.Has("posture"))
		{
		std::string s = p.Get("posture").ToString().Utf8Value();
		if (s == "replace") option_pq_posture = ATIRE_segment_index::PQ_POSTURE_REPLACE;
		else if (s == "rerank") option_pq_posture = ATIRE_segment_index::PQ_POSTURE_RERANK;
		else { Napi::TypeError::New(env, "pq.posture must be 'replace' or 'rerank'").ThrowAsJavaScriptException(); return; }
		}
	if (p.Has("rerankQuant"))
		{
		std::string s = p.Get("rerankQuant").ToString().Utf8Value();
		if (s == "float") option_pq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_FLOAT;
		else if (s == "int8") option_pq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_INT8;
		else { Napi::TypeError::New(env, "pq.rerankQuant must be 'float' or 'int8'").ThrowAsJavaScriptException(); return; }
		}
	if (p.Has("residentTier"))
		{
		std::string s = p.Get("residentTier").ToString().Utf8Value();
		option_pq_tier_requested = true;
		if (s == "float") option_pq_tier = ATIRE_segment_index::PQ_TIER_FLOAT;
		else if (s == "int8") option_pq_tier = ATIRE_segment_index::PQ_TIER_INT8;
		else if (s == "none") option_pq_tier = ATIRE_segment_index::PQ_TIER_NONE;
		else { Napi::TypeError::New(env, "pq.residentTier must be 'float', 'int8', or 'none'").ThrowAsJavaScriptException(); return; }
		}
	if (p.Has("eager")) option_pq_eager = p.Get("eager").ToBoolean().Value() ? 1 : 0;
	}
if (options.Has("multivectorPq") && options.Get("multivectorPq").IsObject())
	{
	Napi::Object p = options.Get("multivectorPq").As<Napi::Object>();
	option_mvpq_requested = true;
	if (p.Has("m")) option_mvpq_m = (long long)p.Get("m").As<Napi::Number>().Int64Value();
	if (p.Has("posture"))
		{
		std::string s = p.Get("posture").ToString().Utf8Value();
		if (s == "replace") option_mvpq_posture = ATIRE_segment_index::PQ_POSTURE_REPLACE;
		else if (s == "rerank") option_mvpq_posture = ATIRE_segment_index::PQ_POSTURE_RERANK;
		else { Napi::TypeError::New(env, "multivectorPq.posture must be 'replace' or 'rerank'").ThrowAsJavaScriptException(); return; }
		}
	if (p.Has("rerankQuant"))
		{
		std::string s = p.Get("rerankQuant").ToString().Utf8Value();
		if (s == "float") option_mvpq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_FLOAT;
		else if (s == "int8") option_mvpq_rerank_quant = ATIRE_segment_index::RERANK_QUANT_INT8;
		else { Napi::TypeError::New(env, "multivectorPq.rerankQuant must be 'float' or 'int8'").ThrowAsJavaScriptException(); return; }
		}
	if (p.Has("residentTier"))
		{
		std::string s = p.Get("residentTier").ToString().Utf8Value();
		option_mvpq_tier_requested = true;
		if (s == "float") option_mvpq_tier = ATIRE_segment_index::MV_TIER_FLOAT;
		else if (s == "none") option_mvpq_tier = ATIRE_segment_index::MV_TIER_NONE;
		else { Napi::TypeError::New(env, "multivectorPq.residentTier must be 'float' or 'none'").ThrowAsJavaScriptException(); return; }
		}
	if (p.Has("eager")) option_mvpq_eager = p.Get("eager").ToBoolean().Value() ? 1 : 0;
	}
```
(Confirm the surrounding ctor returns `void`/uses `return;` on throw — match the existing `quantize` parse's throw-and-return pattern at ~line 197/206.)

- [ ] **Step 6: Apply at open (non-fatal)**

In the open-apply block, after `set_rerank_config` (~line 359, before the attributes apply), add the SAME non-fatal block as Python Task 1 Step 6 (identical engine calls: `set_pq_config`/`set_pq_resident_tier`/`set_pq_policy` and the mvpq trio).

- [ ] **Step 7: Wire the async build ops**

In `MaintenanceWorker::Operation` (line 1626), extend the enum:
```cpp
	enum Operation { FLUSH, MAINTAIN, BUILD, BUILD_HNSW, BUILD_QUANTIZED, BUILD_PQ, BUILD_MULTIVECTOR_PQ };
```
In the Execute switch (after `case BUILD_QUANTIZED:` ~line 1650):
```cpp
		case BUILD_PQ:				result = wrapper->engine->build_pq(); break;
		case BUILD_MULTIVECTOR_PQ:	result = wrapper->engine->build_multivector_pq(); break;
```
In the OnError reject-message ternary (~line 1660), extend it to name the new ops, e.g. append before the final `: "maintain failed"`:
```cpp
			operation == BUILD_PQ ? "build_pq failed" :
			operation == BUILD_MULTIVECTOR_PQ ? "build_multivector_pq failed" :
```
(Insert these two ternary arms alongside the existing `BUILD_QUANTIZED` arm so each op maps to its message.)

- [ ] **Step 8: Add the two build methods + declarations + registration**

Declare beside `BuildQuantized` (~line 88): `Napi::Value BuildPq(const Napi::CallbackInfo &info);` and `Napi::Value BuildMultivectorPq(const Napi::CallbackInfo &info);`. Define them mirroring `BuildQuantized` (~line 1776) with the new op tags:
```cpp
Napi::Value SegmentIndexWrap::BuildPq(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (state != OPEN)
	{
	Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
	deferred.Reject(Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is not open").Value());
	return deferred.Promise();
	}
state = MAINTENANCE;
MaintenanceWorker *worker = new MaintenanceWorker(env, this, info.This().As<Napi::Object>(), MaintenanceWorker::BUILD_PQ);
worker->Queue();
return worker->Promise();
}

Napi::Value SegmentIndexWrap::BuildMultivectorPq(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (state != OPEN)
	{
	Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
	deferred.Reject(Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is not open").Value());
	return deferred.Promise();
	}
state = MAINTENANCE;
MaintenanceWorker *worker = new MaintenanceWorker(env, this, info.This().As<Napi::Object>(), MaintenanceWorker::BUILD_MULTIVECTOR_PQ);
worker->Queue();
return worker->Promise();
}
```
Register beside `InstanceMethod("buildQuantized", ...)` (~line 1820):
```cpp
		InstanceMethod("buildPq", &SegmentIndexWrap::BuildPq),
		InstanceMethod("buildMultivectorPq", &SegmentIndexWrap::BuildMultivectorPq),
```

- [ ] **Step 9: Rebuild and run the test (now PASSES)**

Run:
```bash
cd /data/tyolab/code/antelope/nodejs && npm run build:segment 2>&1 | tail -5
node --test test/pq.test.js
node --test test/quantize.test.js test/rerank.test.js
```
Expected: PASS — `pq.test.js` green; existing quantize/rerank suites unchanged. (If the addon build is environment-blocked, report it and rely on the self-review of the diff.)

- [ ] **Step 10: Commit**

```bash
git add nodejs/addon/segment_index.cpp nodejs/test/pq.test.js
git commit -m "feat(node): expose PQ config (pq/multivectorPq + buildPq/buildMultivectorPq) (#23)"
```

---

## Self-review notes

- **Spec coverage:** dense `pq` (config/tier/policy/build) + token `multivectorPq` in both bindings → Tasks 1 (Python) + 2 (Node). Build methods, reopen-persistence, mutual-exclusion-non-fatal, bad-enum-throw all tested per binding (spec §4).
- **Type consistency:** the 14 `option_*` names + enum constants are identical across Python (Task 1) and Node (Task 2); the apply block is identical engine calls; build-method names are `build_pq`/`build_multivector_pq` (Python snake) and `buildPq`/`buildMultivectorPq` (Node camel) consistently in defs + registration.
- **Non-fatal apply:** both bindings guard the tier/policy calls behind `set_*_pq_config(...) == 0`; a rejected config (mutual exclusion, mismatch) leaves PQ off without throwing — matching `quantize`/`rerank`.
- **Sequencing:** Python first (verifiable here); Node second (report environment-blocked if `node-gyp` can't build). Independent commits so a Node blocker can't hold the Python half.
- **Line numbers indicative** — implementer confirms by grep before editing.

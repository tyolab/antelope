# Node-API Segment Index Binding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `antelope_segment.node` — a self-contained Node-API addon exposing `ATIRE_segment_index` to JavaScript per spec `docs/superpowers/specs/2026-07-06-nodejs-segment-index-binding-design.md`, with sync fast-path methods, Promise-based `flush()`/`maintain()`, and `Float32Array` vectors.

**Architecture:** One C++ translation unit (`nodejs/addon/segment_index.cpp`, `Napi::ObjectWrap<SegmentIndexWrap>`) statically linked against a new GNUmakefile-produced engine archive (`lib/libantelope_engine.a` = the same non-main object set every test binary links) plus the bundled external `.a` libraries. **No engine source is recompiled by gyp** — `atire/atire_segment_index.h` has zero includes and forward-declares everything, so the addon consumes pre-compiled canonical objects and feature-define divergence is impossible by construction (this satisfies, and strengthens, spec §2's define requirement). The legacy SWIG target stays in `binding.gyp` untouched; because it cannot compile on modern Node, the build script compiles ONLY the new target via the generated per-target makefile (`make -C build antelope_segment`).

**Tech Stack:** node-addon-api (NAPI_VERSION 8, `NAPI_DISABLE_CPP_EXCEPTIONS` — explicit throw-and-return control flow, no C++ exceptions leaking near engine code), node-gyp, `node --test` for the JS suite. Node v24 + npm 11 + node-gyp verified present in the dev environment.

**Engine surface consumed (verified against `atire/atire_segment_index.h` on master):** ctor/dtor; `open(dir)`; `set_vector_config(dim, metric)` (BEFORE open); `set_flush_threshold/set_merge_factor/set_tombstone_compact_ratio/set_auto_maintain`; `add_document(key, text)` / `add_document(key, text, vector)` / `update_document(...)` (same shapes) returning `long long` handle (`(generation<<40)|docid`) or −1; `delete_document(key)` → 0/1; `search(char*, k)` / `search_vector(const float*, k)` / `search_hybrid(char*, const float*, k)` → hit count; `hit *get_hit(i)` with `{long long generation, docid; char *filename; double score}` (valid until next search — the binding deep-copies before returning to JS); `flush()`/`maintain()` → 0 on success; `get_document_count()`; `vector_dimension()`; `VECTOR_METRIC_DOT=0 / _COSINE=1 / _L2=2`. NOTE: `search()`'s query is `char*` and MAY BE MUTATED — the binding must pass a writable copy, never a const buffer.

**Worktree:** `.worktrees/segment-binding`, branch `feature/segment-binding`. Baseline: `mkdir -p obj bin && make all && make engine_lib` won't exist until Task 1 — baseline is `make all` + the seven test binaries green.

---

### Task 1: Engine archive target + addon scaffold that builds and loads

**Files:**
- Modify: `GNUmakefile` (one new target)
- Create: `nodejs/addon/segment_index.cpp` (scaffold; grows in Tasks 2–4)
- Modify: `nodejs/binding.gyp` (add target; DO NOT touch the existing `antelope_api` target)
- Modify: `nodejs/package.json` (devDependency + scripts)
- Test: `nodejs/test/smoke.test.js`

- [ ] **Step 1: the failing test**

`nodejs/test/smoke.test.js`:
```js
'use strict';
const test = require('node:test');
const assert = require('node:assert');

test('addon loads and reports a version', () => {
	const addon = require('../build/Release/antelope_segment.node');
	assert.strictEqual(typeof addon.version, 'string');
	assert.ok(addon.version.length > 0);
});
```

- [ ] **Step 2:** `cd nodejs && node --test test/smoke.test.js` → FAIL (module not found).

- [ ] **Step 3: GNUmakefile engine archive.** Near the other convenience targets (after `internal:`):

```make
ENGINE_LIB = $(LIB_DIR)/libantelope_engine.a

engine_lib: directories $(SOURCES_OBJECTS)
	@mkdir -p $(LIB_DIR)
	ar rcs $(ENGINE_LIB) $(SOURCES_OBJECTS)
```
(`SOURCES_OBJECTS` is the existing non-main object set — the same objects every test binary links. `LIB_DIR = lib` already defined at the top of the file. Verify `directories` creates only obj/bin — the target mkdirs lib itself.)

- [ ] **Step 4: scaffold addon.** `nodejs/addon/segment_index.cpp`:

```cpp
/*
	SEGMENT_INDEX.CPP  (Node-API binding)
	-------------------------------------
	Node-API addon exposing ATIRE_segment_index to JavaScript.  See
	docs/superpowers/specs/2026-07-06-nodejs-segment-index-binding-design.md.

	Statically linked against lib/libantelope_engine.a -- no engine source is
	compiled here, and atire_segment_index.h is include-free, so the addon is
	immune to engine feature-define drift by construction.
*/
#include <napi.h>
#include "../../atire/atire_segment_index.h"

static Napi::Object Init(Napi::Env env, Napi::Object exports)
{
exports.Set("version", Napi::String::New(env, "1.0.0"));
return exports;
}

NODE_API_MODULE(antelope_segment, Init)
```

- [ ] **Step 5: gyp target.** In `nodejs/binding.gyp`, add a SECOND entry to the existing `"targets"` array (legacy `antelope_api` entry byte-untouched):

```python
    {
      "target_name": "antelope_segment",
      "sources": [ "addon/segment_index.cpp" ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [ "NAPI_VERSION=8", "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "cflags_cc": [ "-std=c++17", "-fno-exceptions" ],
      "libraries": [
        "<(module_root_dir)/../lib/libantelope_engine.a",
        "<(module_root_dir)/../external/unencumbered/zlib/libz.a",
        "<(module_root_dir)/../external/unencumbered/bzip/libbz2.a",
        "<(module_root_dir)/../external/gpl/lzo/liblzo2.a",
        "<(module_root_dir)/../external/unencumbered/snappy/libsnappy.a",
        "<(module_root_dir)/../external/unencumbered/snowball/libstemmer.a",
        "-lpthread", "-ldl"
      ]
    }
```
Verify the external `.a` paths against the GNUmakefile's `ZLIB_DIR`/`BZIP_DIR`/`LZO_DIR`/`SNAPPY_DIR`/`SNOWBALL_DIR` variables and correct any that differ. If gyp's default flags fight `-fno-exceptions` (node-addon-api with `NAPI_DISABLE_CPP_EXCEPTIONS` supports it), or the archive has link-order issues (undefined symbols from inter-archive deps), fixes in order of preference: list `libantelope_engine.a` twice, or wrap in `-Wl,--start-group ... -Wl,--end-group` via `ldflags`.

- [ ] **Step 6: package.json.** Add `"node-addon-api": "^8.0.0"` to `devDependencies` (create the section if absent) and scripts:
```json
    "build:segment": "make -C .. engine_lib && node-gyp configure && make -C build antelope_segment && mkdir -p build/Release && cp -f build/Release/antelope_segment.node build/Release/ 2>/dev/null || true",
    "test:segment": "node --test test/"
```
Note the single-target make: `node-gyp build` would also build the legacy SWIG target, which cannot compile on Node 24 — the generated `build/Makefile` supports per-target invocation. Verify the built artifact lands at `build/Release/antelope_segment.node` (gyp's default `BUILDTYPE=Release`); adjust the script if the generated makefile needs `BUILDTYPE=Release` explicitly. Do NOT touch the legacy `install` script.

- [ ] **Step 7:** `cd nodejs && npm install --no-save node-addon-api && npm run build:segment && node --test test/smoke.test.js` → smoke test PASS. Also `cd .. && make internal` still exit 0 and the seven C++ test binaries still PASS (GNUmakefile change is additive).

- [ ] **Step 8: Commit** — `feat: engine static archive and Node-API addon scaffold`

---

### Task 2: `SegmentIndex` class skeleton — options, open/close, trivial accessors

**Files:**
- Modify: `nodejs/addon/segment_index.cpp`
- Test: `nodejs/test/segment_index.test.js` (new; grows through Task 4)

- [ ] **Step 1: the failing test**

```js
'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

function freshDir() { return fs.mkdtempSync(path.join(os.tmpdir(), 'ant-binding-')); }

test('open/close lifecycle and accessors', () => {
	const idx = new SegmentIndex();
	assert.throws(() => idx.documentCount(), /not open/);
	idx.open(freshDir());
	assert.strictEqual(idx.documentCount(), 0);
	assert.strictEqual(idx.vectorDimension(), 0);
	idx.close();
	assert.throws(() => idx.documentCount(), /not open/);
	assert.throws(() => idx.open(freshDir()), /closed/);	// a closed instance is done
});

test('vector options are applied and validated', () => {
	const dir = freshDir();
	const idx = new SegmentIndex({ dimension: 4, metric: 'cosine' });
	idx.open(dir);
	assert.strictEqual(idx.vectorDimension(), 4);
	idx.close();

	// config mismatch on reopen throws
	const wrong = new SegmentIndex({ dimension: 8, metric: 'cosine' });
	assert.throws(() => wrong.open(dir), /open failed/);

	// bad option values throw at construction
	assert.throws(() => new SegmentIndex({ dimension: 4, metric: 'euclideanish' }), /metric/);
	assert.throws(() => new SegmentIndex({ dimension: 0 }), /dimension/);
});

test('open failure leaves instance reusable', () => {
	const idx = new SegmentIndex();
	assert.throws(() => idx.open('/nonexistent-parent-zzz/sub'), /open failed/);
	idx.open(freshDir());		// still usable after a failed open
	assert.strictEqual(idx.documentCount(), 0);
	idx.close();
});
```

- [ ] **Step 2:** run → FAIL (`SegmentIndex` undefined on exports).

- [ ] **Step 3: implement the wrapper skeleton** (replaces the scaffold's `Init`; keep `version`):

```cpp
#include <napi.h>
#include <string.h>
#include "../../atire/atire_segment_index.h"

class SegmentIndexWrap : public Napi::ObjectWrap<SegmentIndexWrap>
{
public:
	enum State { CONSTRUCTED, OPEN, MAINTENANCE, CLOSED };

private:
	ATIRE_segment_index *engine;
	State state;
	/* options captured at construction, applied at open() */
	long long option_dimension;
	long option_metric;
	long long option_flush_threshold;	// -1 = leave engine default
	long option_merge_factor;			// -1 = default
	double option_tombstone_ratio;		// < 0 = default
	long option_auto_maintain;			// 0/1

	friend class MaintenanceWorker;		// Task 4

	/* guards; each returns false after throwing */
	bool require_open(Napi::Env env)
	{
	if (state == OPEN)
		return true;
	if (state == MAINTENANCE)
		Napi::Error::New(env, "maintenance in progress").ThrowAsJavaScriptException();
	else
		Napi::Error::New(env, "index is not open").ThrowAsJavaScriptException();
	return false;
	}

public:
	static Napi::Object Register(Napi::Env env, Napi::Object exports);
	SegmentIndexWrap(const Napi::CallbackInfo &info);
	~SegmentIndexWrap() { delete engine; }

	Napi::Value Open(const Napi::CallbackInfo &info);
	Napi::Value Close(const Napi::CallbackInfo &info);
	Napi::Value DocumentCount(const Napi::CallbackInfo &info);
	Napi::Value VectorDimension(const Napi::CallbackInfo &info);
	/* Task 3 */
	Napi::Value AddDocument(const Napi::CallbackInfo &info);
	Napi::Value UpdateDocument(const Napi::CallbackInfo &info);
	Napi::Value DeleteDocument(const Napi::CallbackInfo &info);
	Napi::Value Search(const Napi::CallbackInfo &info);
	Napi::Value SearchVector(const Napi::CallbackInfo &info);
	Napi::Value SearchHybrid(const Napi::CallbackInfo &info);
	/* Task 4 */
	Napi::Value Flush(const Napi::CallbackInfo &info);
	Napi::Value Maintain(const Napi::CallbackInfo &info);
};

SegmentIndexWrap::SegmentIndexWrap(const Napi::CallbackInfo &info) : Napi::ObjectWrap<SegmentIndexWrap>(info)
{
Napi::Env env = info.Env();

engine = NULL;
state = CONSTRUCTED;
option_dimension = 0;
option_metric = ATIRE_segment_index::VECTOR_METRIC_DOT;
option_flush_threshold = -1;
option_merge_factor = -1;
option_tombstone_ratio = -1.0;
option_auto_maintain = 0;

if (info.Length() >= 1 && !info[0].IsUndefined() && !info[0].IsNull())
	{
	if (!info[0].IsObject())
		{
		Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
		return;
		}
	Napi::Object options = info[0].As<Napi::Object>();
	if (options.Has("dimension"))
		{
		option_dimension = options.Get("dimension").ToNumber().Int64Value();
		if (option_dimension < 1 || option_dimension > 65536)
			{
			Napi::TypeError::New(env, "dimension must be between 1 and 65536").ThrowAsJavaScriptException();
			return;
			}
		}
	if (options.Has("metric"))
		{
		std::string metric = options.Get("metric").ToString().Utf8Value();
		if (metric == "dot")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_DOT;
		else if (metric == "cosine")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_COSINE;
		else if (metric == "l2")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_L2;
		else
			{
			Napi::TypeError::New(env, "metric must be 'dot', 'cosine' or 'l2'").ThrowAsJavaScriptException();
			return;
			}
		}
	if (options.Has("flushThreshold"))
		option_flush_threshold = options.Get("flushThreshold").ToNumber().Int64Value();
	if (options.Has("mergeFactor"))
		option_merge_factor = (long)options.Get("mergeFactor").ToNumber().Int64Value();
	if (options.Has("tombstoneRatio"))
		option_tombstone_ratio = options.Get("tombstoneRatio").ToNumber().DoubleValue();
	if (options.Has("autoMaintain"))
		option_auto_maintain = options.Get("autoMaintain").ToBoolean().Value() ? 1 : 0;
	}
}

Napi::Value SegmentIndexWrap::Open(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state == CLOSED)
	{
	Napi::Error::New(env, "index is closed").ThrowAsJavaScriptException();
	return env.Undefined();
	}
if (state != CONSTRUCTED)
	{
	Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is already open").ThrowAsJavaScriptException();
	return env.Undefined();
	}
if (info.Length() < 1 || !info[0].IsString())
	{
	Napi::TypeError::New(env, "open(directory) requires a string").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string directory = info[0].As<Napi::String>().Utf8Value();

engine = new ATIRE_segment_index();
if (option_dimension > 0 && engine->set_vector_config(option_dimension, option_metric) != 0)
	{
	delete engine;
	engine = NULL;
	Napi::Error::New(env, "invalid vector configuration").ThrowAsJavaScriptException();
	return env.Undefined();
	}
if (option_flush_threshold >= 0)
	engine->set_flush_threshold(option_flush_threshold);
if (option_merge_factor > 0)
	engine->set_merge_factor(option_merge_factor);
if (option_tombstone_ratio >= 0.0)
	engine->set_tombstone_compact_ratio(option_tombstone_ratio);
engine->set_auto_maintain(option_auto_maintain);

if (engine->open(directory.c_str()) != 0)
	{
	delete engine;
	engine = NULL;
	Napi::Error::New(env, "open failed: bad directory, corrupt index, or vector config mismatch").ThrowAsJavaScriptException();
	return env.Undefined();
	}
state = OPEN;
return env.Undefined();
}
```
NOTE: check `ATIRE_segment_index::open`'s actual parameter type (`const char *`) — it is; pass `.c_str()`. If `open` mutates state on failure such that reuse of a fresh engine is required, the delete/NULL above already guarantees a fresh engine per attempt.

`Close`, `DocumentCount`, `VectorDimension`:

```cpp
Napi::Value SegmentIndexWrap::Close(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state == MAINTENANCE)
	{
	Napi::Error::New(env, "maintenance in progress").ThrowAsJavaScriptException();
	return env.Undefined();
	}
delete engine;
engine = NULL;
state = CLOSED;
return env.Undefined();
}

Napi::Value SegmentIndexWrap::DocumentCount(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
return Napi::Number::New(env, (double)engine->get_document_count());
}

Napi::Value SegmentIndexWrap::VectorDimension(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
return Napi::Number::New(env, (double)engine->vector_dimension());
}
```

`Register` + `Init` (Task 3/4 methods registered now, defined as stubs throwing "not implemented" so the class shape is final — replaced in their tasks):

```cpp
Napi::Object SegmentIndexWrap::Register(Napi::Env env, Napi::Object exports)
{
Napi::Function ctor = DefineClass(env, "SegmentIndex", {
	InstanceMethod("open", &SegmentIndexWrap::Open),
	InstanceMethod("close", &SegmentIndexWrap::Close),
	InstanceMethod("documentCount", &SegmentIndexWrap::DocumentCount),
	InstanceMethod("vectorDimension", &SegmentIndexWrap::VectorDimension),
	InstanceMethod("addDocument", &SegmentIndexWrap::AddDocument),
	InstanceMethod("updateDocument", &SegmentIndexWrap::UpdateDocument),
	InstanceMethod("deleteDocument", &SegmentIndexWrap::DeleteDocument),
	InstanceMethod("search", &SegmentIndexWrap::Search),
	InstanceMethod("searchVector", &SegmentIndexWrap::SearchVector),
	InstanceMethod("searchHybrid", &SegmentIndexWrap::SearchHybrid),
	InstanceMethod("flush", &SegmentIndexWrap::Flush),
	InstanceMethod("maintain", &SegmentIndexWrap::Maintain),
});
exports.Set("SegmentIndex", ctor);
return exports;
}
```
Stubs for the Task 3/4 methods:
```cpp
/* replaced in Task 3 / Task 4 */
Napi::Value SegmentIndexWrap::AddDocument(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }
```
(same one-liner for UpdateDocument, DeleteDocument, Search, SearchVector, SearchHybrid, Flush, Maintain.)

- [ ] **Step 4:** `npm run build:segment && node --test test/` → smoke + the three new tests PASS.

- [ ] **Step 5: Commit** — `feat: SegmentIndex wrapper skeleton with options and lifecycle`

---

### Task 3: Write path + synchronous searches

**Files:**
- Modify: `nodejs/addon/segment_index.cpp` (replace six stubs; add helpers)
- Test: `nodejs/test/segment_index.test.js` (append)

- [ ] **Step 1: the failing tests — append:**

```js
test('add, search, update, delete — lexical', () => {
	const idx = new SegmentIndex();
	idx.open(freshDir());
	const ref = idx.addDocument('doc-1', '<DOC>aardvark zebra</DOC>');
	assert.strictEqual(typeof ref.generation, 'number');
	assert.strictEqual(typeof ref.docid, 'number');
	idx.addDocument('doc-2', '<DOC>zebra quokka</DOC>');

	let hits = idx.search('zebra', 10);
	assert.strictEqual(hits.length, 2);
	assert.ok(hits[0].key === 'doc-1' || hits[0].key === 'doc-2');
	assert.strictEqual(typeof hits[0].score, 'number');

	idx.updateDocument('doc-1', '<DOC>wombat only</DOC>');
	assert.strictEqual(idx.search('aardvark', 10).length, 0);
	assert.strictEqual(idx.search('wombat', 10).length, 1);

	assert.strictEqual(idx.deleteDocument('doc-2'), true);
	assert.strictEqual(idx.deleteDocument('no-such-key'), false);
	assert.strictEqual(idx.search('quokka', 10).length, 0);

	assert.throws(() => idx.addDocument('empty', '<DOC></DOC>'), /rejected/);
	idx.close();
});

test('vectors: typed arrays, plain arrays, hybrid, type errors', () => {
	const idx = new SegmentIndex({ dimension: 4, metric: 'dot' });
	idx.open(freshDir());
	idx.addDocument('doc-a', '<DOC>alpha content</DOC>', Float32Array.from([1, 0, 0, 0]));
	idx.addDocument('doc-b', '<DOC>beta content</DOC>', [0.9, 0.1, 0, 0]);	// plain array accepted
	idx.addDocument('doc-c', '<DOC>gamma lexical only</DOC>');

	const vhits = idx.searchVector(Float32Array.from([1, 0, 0, 0]), 10);
	assert.strictEqual(vhits.length, 2);						// doc-c has no vector
	assert.strictEqual(vhits[0].key, 'doc-a');
	assert.ok(vhits[0].score > vhits[1].score);

	// hybrid: doc-a matches both sides, must rank first
	const hhits = idx.searchHybrid('alpha', Float32Array.from([1, 0, 0, 0]), 3);
	assert.strictEqual(hhits[0].key, 'doc-a');
	// degradations
	assert.strictEqual(idx.searchHybrid(null, Float32Array.from([1, 0, 0, 0]), 3).length, 2);
	assert.strictEqual(idx.searchHybrid('gamma', null, 3).length, 1);
	assert.deepStrictEqual(idx.searchHybrid(null, null, 3), []);

	assert.throws(() => idx.addDocument('bad', '<DOC>x y</DOC>', Float32Array.from([1, 0])), /dimension/);
	assert.throws(() => idx.addDocument('bad', '<DOC>x y</DOC>', 'not a vector'), /TypeError|vector/);
	assert.throws(() => idx.searchVector(Float32Array.from([1, 0]), 5), /dimension/);
	idx.close();

	const plain = new SegmentIndex();
	plain.open(freshDir());
	assert.throws(() => plain.addDocument('k', '<DOC>a b</DOC>', Float32Array.from([1])), /not.*enabled|vector/i);
	assert.deepStrictEqual(plain.searchVector(Float32Array.from([1]), 5), []);
	plain.close();
});

test('cosine zero-vector rejected', () => {
	const idx = new SegmentIndex({ dimension: 3, metric: 'cosine' });
	idx.open(freshDir());
	assert.throws(() => idx.addDocument('z', '<DOC>a b</DOC>', Float32Array.from([0, 0, 0])), /zero/);
	idx.close();
});
```

- [ ] **Step 2:** build + run → FAIL at the stubs.

- [ ] **Step 3: helpers + implementations.** Helpers (file-scope, above the class methods):

```cpp
/*
	EXTRACT_VECTOR()
	----------------
	Accepts Float32Array (zero-copy pointer) or number[] (converted into
	scratch, which the caller owns).  Returns NULL and throws on anything
	else or on dimension mismatch.  *scratch is set non-NULL only when a
	conversion allocated; caller delete[]s it.
*/
static const float *extract_vector(Napi::Env env, Napi::Value value, long long dimension, float **scratch)
{
*scratch = NULL;
if (dimension < 1)
	{
	Napi::TypeError::New(env, "vectors are not enabled on this index").ThrowAsJavaScriptException();
	return NULL;
	}
if (value.IsTypedArray())
	{
	Napi::TypedArray typed = value.As<Napi::TypedArray>();
	if (typed.TypedArrayType() != napi_float32_array)
		{
		Napi::TypeError::New(env, "vector must be a Float32Array or number[]").ThrowAsJavaScriptException();
		return NULL;
		}
	if ((long long)typed.ElementLength() != dimension)
		{
		Napi::TypeError::New(env, "vector dimension mismatch").ThrowAsJavaScriptException();
		return NULL;
		}
	return static_cast<const float *>(typed.As<Napi::Float32Array>().Data());
	}
if (value.IsArray())
	{
	Napi::Array array = value.As<Napi::Array>();
	if ((long long)array.Length() != dimension)
		{
		Napi::TypeError::New(env, "vector dimension mismatch").ThrowAsJavaScriptException();
		return NULL;
		}
	*scratch = new float[dimension];
	for (long long which = 0; which < dimension; which++)
		(*scratch)[which] = (float)array.Get((uint32_t)which).ToNumber().DoubleValue();
	return *scratch;
	}
Napi::TypeError::New(env, "vector must be a Float32Array or number[]").ThrowAsJavaScriptException();
return NULL;
}

/*
	HITS_TO_ARRAY()
	---------------
	Deep-copies the engine's hit list (valid only until the next search)
	into a JS array of { key, score, generation, docid }.
*/
static Napi::Array hits_to_array(Napi::Env env, ATIRE_segment_index *engine, long long count)
{
Napi::Array result = Napi::Array::New(env, (size_t)count);
for (long long which = 0; which < count; which++)
	{
	ATIRE_segment_index::hit *hit = engine->get_hit(which);
	Napi::Object entry = Napi::Object::New(env);
	entry.Set("key", Napi::String::New(env, hit->filename == NULL ? "" : hit->filename));
	entry.Set("score", Napi::Number::New(env, hit->score));
	entry.Set("generation", Napi::Number::New(env, (double)hit->generation));
	entry.Set("docid", Napi::Number::New(env, (double)hit->docid));
	result.Set((uint32_t)which, entry);
	}
return result;
}
```

`AddDocument` (UpdateDocument is IDENTICAL except it calls `engine->update_document(...)` and its banner comment says upsert — write both out in full in the source):

```cpp
Napi::Value SegmentIndexWrap::AddDocument(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
float *scratch = NULL;
const float *vector = NULL;

if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString())
	{
	Napi::TypeError::New(env, "addDocument(key, text[, vector])").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string key = info[0].As<Napi::String>().Utf8Value();
std::string text = info[1].As<Napi::String>().Utf8Value();
if (info.Length() >= 3 && !info[2].IsUndefined() && !info[2].IsNull())
	{
	vector = extract_vector(env, info[2], engine->vector_dimension(), &scratch);
	if (vector == NULL)
		return env.Undefined();		// TypeError already thrown
	}

long long handle = vector != NULL
	? engine->add_document(key.c_str(), text.c_str(), vector)
	: engine->add_document(key.c_str(), text.c_str());
delete [] scratch;

if (handle < 0)
	{
	Napi::Error::New(env, vector != NULL && option_metric == ATIRE_segment_index::VECTOR_METRIC_COSINE
		? "document rejected: empty/unparseable text, zero vector under cosine, or degraded index"
		: "document rejected: empty or unparseable text, or index is in a degraded read-only state").ThrowAsJavaScriptException();
	return env.Undefined();
	}
Napi::Object ref = Napi::Object::New(env);
ref.Set("generation", Napi::Number::New(env, (double)(handle >> 40)));
ref.Set("docid", Napi::Number::New(env, (double)(handle & ((1LL << 40) - 1))));
return ref;
}
```
(The handle decomposition mirrors `make_handle(generation, docid) = (generation << 40) | docid` — cite `atire/atire_segment_index.h`.)

`DeleteDocument`:
```cpp
Napi::Value SegmentIndexWrap::DeleteDocument(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 1 || !info[0].IsString())
	{
	Napi::TypeError::New(env, "deleteDocument(key)").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string key = info[0].As<Napi::String>().Utf8Value();
return Napi::Boolean::New(env, engine->delete_document(key.c_str()) == 0);
}
```

`Search` (writable query copy — the engine mutates it):
```cpp
Napi::Value SegmentIndexWrap::Search(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber())
	{
	Napi::TypeError::New(env, "search(text, k)").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string query = info[0].As<Napi::String>().Utf8Value();
long long top_k = info[1].As<Napi::Number>().Int64Value();
if (top_k < 1)
	return Napi::Array::New(env, 0);
std::string mutable_query = query;		// engine may modify the buffer
long long count = engine->search(&mutable_query[0], top_k);
return hits_to_array(env, engine, count);
}
```
`SearchVector`: extract_vector (return `[]` — an empty array, NOT a throw — when `vector_dimension() == 0`, per spec §3.3; still TypeError on wrong type/dimension when enabled), then `engine->search_vector(vector, top_k)`, `hits_to_array`, `delete [] scratch`. `SearchHybrid`: both arguments individually optional (null/undefined → that side NULL); text side uses the writable-copy pattern; when BOTH are absent return `Napi::Array::New(env, 0)` without calling the engine; the engine call is `engine->search_hybrid(text_ptr, vector, top_k)` where `text_ptr` is `&mutable_query[0]` or NULL. Write both out fully in the source following the shown patterns; every parameter validation mirrors the ones above.

Also verify against `atire/atire_segment_index.h`: `delete_document`'s parameter constness and `search`'s exact signature (`char *`) — adjust `const_cast`/copies accordingly, never pass `.c_str()` to a mutating `char*` parameter.

- [ ] **Step 4:** build + `node --test test/` → all PASS.

- [ ] **Step 5: Commit** — `feat: document write path and synchronous searches in the binding`

---

### Task 4: Async `flush()`/`maintain()` with busy-guard

**Files:**
- Modify: `nodejs/addon/segment_index.cpp`
- Test: `nodejs/test/segment_index.test.js` (append)

- [ ] **Step 1: the failing tests — append:**

```js
test('flush persists; maintain compacts; both are Promises', async () => {
	const dir = freshDir();
	const idx = new SegmentIndex({ mergeFactor: 2, tombstoneRatio: 0.2 });
	idx.open(dir);
	for (let i = 0; i < 8; i++)
		idx.addDocument(`doc-${i}`, `<DOC>common filler${'x'.repeat(i)}</DOC>`);
	await idx.flush();
	for (let i = 0; i < 5; i++)
		assert.strictEqual(idx.deleteDocument(`doc-${i}`), true);
	assert.strictEqual(idx.documentCount(), 3);
	await idx.maintain();				// tombstone ratio 5/8 > 0.2 -> compaction
	assert.strictEqual(idx.documentCount(), 3);
	assert.strictEqual(idx.search('common', 100).length, 3);
	idx.close();

	const reopened = new SegmentIndex();
	reopened.open(dir);
	assert.strictEqual(reopened.documentCount(), 3);
	reopened.close();
});

test('busy-guard: engine calls during maintenance throw', async () => {
	const idx = new SegmentIndex();
	idx.open(freshDir());
	for (let i = 0; i < 50; i++)
		idx.addDocument(`doc-${i}`, `<DOC>word${i} common</DOC>`);
	const pending = idx.flush();		// state -> MAINTENANCE until settled
	assert.throws(() => idx.addDocument('during', '<DOC>x y</DOC>'), /maintenance in progress/);
	assert.throws(() => idx.search('common', 5), /maintenance in progress/);
	assert.throws(() => idx.close(), /maintenance in progress/);
	await assert.rejects(Promise.race([idx.maintain()]), /maintenance in progress/);
	await pending;
	assert.strictEqual(idx.search('common', 100).length, 50);	// usable again
	idx.close();
});
```
(If the 50-doc flush completes so fast the sync throws race the settle: the guard is set synchronously at dispatch and cleared only in the Promise's completion callbacks, which cannot run until this synchronous test block yields — so the asserts between `idx.flush()` and the first `await` are deterministic regardless of engine speed. State that reasoning in a test comment.)

- [ ] **Step 2:** build + run → FAIL at the stubs.

- [ ] **Step 3: implement.** The worker:

```cpp
/*
	class MAINTENANCE_WORKER
	------------------------
	Runs flush() or maintain() off the event loop.  Holds a reference to the
	wrapper so GC cannot finalize the engine mid-operation; restores IDLE and
	settles the Promise on completion.
*/
class MaintenanceWorker : public Napi::AsyncWorker
{
public:
	enum Operation { FLUSH, MAINTAIN };

private:
	SegmentIndexWrap *wrapper;
	Napi::ObjectReference self;
	Napi::Promise::Deferred deferred;
	Operation operation;
	long result;

public:
	MaintenanceWorker(Napi::Env env, SegmentIndexWrap *wrapper, Napi::Object js_object, Operation operation)
		: Napi::AsyncWorker(env), wrapper(wrapper), self(Napi::Persistent(js_object)),
		  deferred(Napi::Promise::Deferred::New(env)), operation(operation), result(1) {}

	Napi::Promise Promise() { return deferred.Promise(); }

	void Execute()		/* worker thread: NO JS access */
	{
	result = operation == FLUSH ? wrapper->engine->flush() : wrapper->engine->maintain();
	}

	void OnOK()
	{
	wrapper->state = SegmentIndexWrap::OPEN;
	if (result == 0)
		deferred.Resolve(Env().Undefined());
	else
		deferred.Reject(Napi::Error::New(Env(), operation == FLUSH ? "flush failed; index degraded to read-only" : "maintain failed").Value());
	}

	void OnError(const Napi::Error &error)
	{
	wrapper->state = SegmentIndexWrap::OPEN;
	deferred.Reject(error.Value());
	}
};
```
(`wrapper->engine` and `wrapper->state` need `MaintenanceWorker` as a friend of `SegmentIndexWrap` — the skeleton declared it.)

`Flush`/`Maintain`:
```cpp
Napi::Value SegmentIndexWrap::Flush(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state != OPEN)
	{
	Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
	deferred.Reject(Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is not open").Value());
	return deferred.Promise();
	}
state = MAINTENANCE;
MaintenanceWorker *worker = new MaintenanceWorker(env, this, info.This().As<Napi::Object>(), MaintenanceWorker::FLUSH);
worker->Queue();
return worker->Promise();
}
```
(`Maintain` identical with `MaintenanceWorker::MAINTAIN` — write it out.) Async-path errors REJECT rather than throw (Promise-returning methods must never throw synchronously — the test's `assert.rejects` on the busy `maintain()` depends on this).

- [ ] **Step 4:** build + `node --test test/` → all PASS. Then the C++ regression: `cd .. && make internal` exit 0 and the seven binaries PASS (nothing engine-side changed, but the archive gets relinked — confirm).

- [ ] **Step 5: Commit** — `feat: Promise-based flush and maintain with busy-guard`

---

### Task 5: Exports, TypeScript definitions, README, full sweep

**Files:**
- Modify: `nodejs/index.js` (lazy export ONLY — see warning)
- Create: `nodejs/segment_index.d.ts`
- Modify: `nodejs/README.md` (new section)
- Modify: `nodejs/package.json` (types entry if absent: `"types": "segment_index.d.ts"` — only if package.json has no existing `types`)

- [ ] **Step 1: lazy export.** `nodejs/index.js` currently `require`s the LEGACY addon at module top level; that addon cannot load on modern Node, so `SegmentIndex` must be reachable WITHOUT evaluating the legacy path. Inspect index.js first: if its top level already requires the legacy `.node`, add the new export via `Object.defineProperty` at the TOP of the file (before any legacy require) AND wrap nothing else:

```js
Object.defineProperty(exports, 'SegmentIndex', {
	enumerable: true,
	get() { return require('./build/Release/antelope_segment.node').SegmentIndex; }
});
```
If top-level legacy requires would still break `require('antelope-search')` entirely on modern Node, note that in the report but change nothing further — consumers on modern Node can `require('antelope-search/build/Release/antelope_segment.node')` directly, and the README documents both paths. (Restructuring legacy loading is explicitly out of scope.)

- [ ] **Step 2: `nodejs/segment_index.d.ts`** — exactly the spec §3 TypeScript block, wrapped as a module declaration:

```ts
export type Metric = 'dot' | 'cosine' | 'l2';

export interface SegmentIndexOptions {
	dimension?: number;
	metric?: Metric;
	flushThreshold?: number;
	mergeFactor?: number;
	tombstoneRatio?: number;
	autoMaintain?: boolean;
}

export interface DocRef { generation: number; docid: number; }
export interface Hit extends DocRef { key: string; score: number; }

export class SegmentIndex {
	constructor(options?: SegmentIndexOptions);
	open(directory: string): void;
	close(): void;
	addDocument(key: string, text: string, vector?: Float32Array | number[]): DocRef;
	updateDocument(key: string, text: string, vector?: Float32Array | number[]): DocRef;
	deleteDocument(key: string): boolean;
	search(text: string, k: number): Hit[];
	searchVector(vector: Float32Array | number[], k: number): Hit[];
	searchHybrid(text: string | null, vector: Float32Array | number[] | null, k: number): Hit[];
	flush(): Promise<void>;
	maintain(): Promise<void>;
	documentCount(): number;
	vectorDimension(): number;
}
```

- [ ] **Step 3: README section.** Append to `nodejs/README.md` a "SegmentIndex (modern Node-API binding)" section: one-paragraph intro (self-contained addon, Node ≥ 12.22, no .so install), build (`npm run build:segment` — needs the repo's engine built once: `make -C .. all` first), and this usage example:

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

- [ ] **Step 4: full sweep, paste outputs:** `npm run build:segment` clean; `node --test test/` all tests PASS (run twice); `node -e "const {SegmentIndex} = require('./index.js'); console.log(typeof SegmentIndex)"` prints `function` (or documented as unreachable if legacy top-level requires break index.js — report which); `make internal` exit 0; all seven C++ binaries PASS; `git status --short` clean after commit.

- [ ] **Step 5: Commit** — `feat: SegmentIndex export, TypeScript definitions, and docs`

---

## Self-review record

- **Spec coverage:** §1 decisions (Node-API/NAPI 8, static link, sync/async split, DocRef objects, Float32Array + number[], same package, legacy frozen) → Tasks 1–5; §2 layout + the define-divergence requirement (resolved stronger: no engine recompilation; include-free header verified) → Task 1; §3 full API incl. handle decomposition, degradations, metric strings, k<1 → Tasks 2–4; §4 threading (state machine, AsyncWorker, ObjectReference GC guard, close-during-maintenance) → Tasks 2/4; §5 error table → every row has an assertion (Tasks 2–4 tests); §6 testing incl. compaction-observable, busy-guard determinism reasoning, reopen persistence, C++ regression → Tasks 4/5; §7 out-of-scope respected (no prebuilds, no legacy changes beyond the lazy one-liner, no WASM).
- **Known risks flagged in-task:** external `.a` paths vs GNUmakefile vars (Task 1), archive link-order (Task 1, with the two fixes ranked), gyp single-target build mechanics (Task 1), `search()`'s mutating `char*` (Task 3), legacy index.js top-level requires possibly breaking package-level import on modern Node (Task 5, with the documented fallback).
- **Type consistency:** `SegmentIndexWrap` state enum/members declared once (Task 2) and used by Task 4's worker via the friend declaration made in Task 2; `extract_vector`/`hits_to_array` signatures consistent across Task 3 uses; JS API names identical across tests, d.ts, and DefineClass list.

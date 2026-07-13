# Expose PQ Config in Node + Python Bindings Design

**Status:** proposed 2026-07-13, pending user review. Issue **#23**.

**Goal:** Surface the engine's product-quantization configuration — dense `set_pq_config`/`set_pq_resident_tier`/`set_pq_policy`/`build_pq` and the token-pool `set_multivector_pq_config`/`set_multivector_resident_tier`/`set_multivector_pq_policy`/`build_multivector_pq` — in both the Node addon (`nodejs/addon/segment_index.cpp`) and the Python pybind11 binding (`python/src/antelope_core.cpp`), at parity with how `rerank`/`quantize`/`hnsw`/`approximate` are already exposed.

**Architecture (one sentence):** Add two constructor option bags — `pq` and `multivectorPq` (dense + token PQ) — parsed into `option_*` members and applied post-open (non-fatal, in the existing config-apply block), plus two backfill build methods per binding (`buildPq`/`buildMultivectorPq` in Node — async via `MaintenanceWorker`; `build_pq`/`build_multivector_pq` in Python — sync, mirroring `build_quantized`).

**Tech stack:** C++ N-API addon (`nodejs/addon/segment_index.cpp`, node-gyp) + C++ pybind11 (`python/src/antelope_core.cpp`, setup.py). Engine API (`atire/atire_segment_index.h`): `set_pq_config(m, posture, rerank_quant)`, `set_pq_resident_tier(tier)`, `set_pq_policy(eager)`, `build_pq()`; `set_multivector_pq_config(...)`, `set_multivector_resident_tier(tier)`, `set_multivector_pq_policy(eager)`, `build_multivector_pq()`.

**Scope:** binding-only, both bindings in one branch (tasks split Node vs Python). No engine changes.

**Surface decision:** constructor option bags + build methods (the established pattern — config is set-once at open, immutable, persisted), NOT post-open setter methods. PQ config is immutable in the engine anyway; resident-tier is realized at load, so a caller reclaims RAM by reopening with `residentTier:"none"`. The build methods cover the only genuine runtime need (on-demand backfill).

---

## 1. Option bags (both bindings)

Two optional constructor options, each a dict/object. Keys use each binding's existing convention: **camelCase** for Node, **snake_case** for Python. String-enum values, matching the existing `quantize`/`rerank.quantize` style. All fields optional with the noted defaults.

**Dense — `pq`:**
| key (Node / Python) | type | values → engine | default |
|---|---|---|---|
| `m` / `m` | int | subvector count; `0` ⇒ `default_pq_m(dim)` | `0` |
| `posture` / `posture` | string | `"replace"`→`PQ_POSTURE_REPLACE`, `"rerank"`→`PQ_POSTURE_RERANK` | `"replace"` |
| `rerankQuant` / `rerank_quant` | string | `"float"`→`RERANK_QUANT_FLOAT`, `"int8"`→`RERANK_QUANT_INT8` | `"float"` |
| `residentTier` / `resident_tier` | string | `"float"`→`PQ_TIER_FLOAT`, `"int8"`→`PQ_TIER_INT8`, `"none"`→`PQ_TIER_NONE` | unset ⇒ skip `set_pq_resident_tier` (engine default FLOAT) |
| `eager` / `eager` | bool | `set_pq_policy(1|0)` | `false` |

**Token — `multivectorPq`:** same shape, except `residentTier`/`resident_tier` accepts only `"float"`→`MV_TIER_FLOAT` / `"none"`→`MV_TIER_NONE` (token tier has no int8); routes to `set_multivector_pq_config` / `set_multivector_resident_tier` / `set_multivector_pq_policy`.

Presence sentinel: a `long option_pq_m` etc. isn't enough to know "requested", so gate each bag on a `bool option_pq_requested` / `option_mvpq_requested` set when the option key is present (mirrors how `option_rerank_dim > 0` gates rerank, but PQ has no natural "off" numeric — use an explicit bool).

An **invalid string value** (e.g. `posture:"bogus"`) throws at parse time — `TypeError` (Node `Napi::TypeError`) / `ValueError` (Python `py::value_error`) — mirroring the existing `quantize mode must be …` throw. A missing key uses the default.

## 2. Apply at open (both bindings)

In the existing post-open config-apply block (Node ~`open()` after `set_rerank_config`; Python `open()` after line 701), append — **non-fatal**, exactly like `quantize`/`rerank` (a rejected or already-persisted-different config just leaves PQ off; never throws at apply):

```
if (option_pq_requested)
    {
    if (engine->set_pq_config(option_pq_m, option_pq_posture, option_pq_rerank_quant) == 0)
        {
        if (option_pq_tier_requested) engine->set_pq_resident_tier(option_pq_tier);
        engine->set_pq_policy(option_pq_eager);
        }
    }
if (option_mvpq_requested)
    {
    if (engine->set_multivector_pq_config(option_mvpq_m, option_mvpq_posture, option_mvpq_rerank_quant) == 0)
        {
        if (option_mvpq_tier_requested) engine->set_multivector_resident_tier(option_mvpq_tier);
        engine->set_multivector_pq_policy(option_mvpq_eager);
        }
    }
```

Ordering: dense `pq` after `set_quantization` (engine rejects PQ when V4 int8 quantize is enabled — mutually exclusive, so `set_pq_config` returns nonzero and PQ stays off — the intended, non-fatal outcome); token `multivectorPq` after `set_rerank_config` (token PQ requires rerank configured — else `set_multivector_pq_config` returns nonzero, stays off). `set_pq_resident_tier`/`set_multivector_resident_tier` only when the tier key was given (leaving the engine's FLOAT default otherwise). Config restored on reopen is handled by the engine (`load_pq_config`/`load_multivector_pq_config`), so re-passing the same bag is idempotent.

## 3. Build methods

**Python** (`antelope_core.cpp`) — sync, mirroring `build_quantized` (require_open → `py::gil_scoped_release` → `engine->build_pq()` → `throw std::runtime_error("build_pq failed")` on nonzero):
```cpp
void build_pq()             { require_open(); long rc; { py::gil_scoped_release r; rc = engine->build_pq(); }            if (rc) throw std::runtime_error("build_pq failed"); }
void build_multivector_pq() { require_open(); long rc; { py::gil_scoped_release r; rc = engine->build_multivector_pq(); } if (rc) throw std::runtime_error("build_multivector_pq failed"); }
```
Bind with `.def("build_pq", …)` / `.def("build_multivector_pq", …)` beside `build_quantized`.

**Node** (`segment_index.cpp`) — async, mirroring `BuildQuantized`: extend `MaintenanceWorker::Operation` (line 1626) with `BUILD_PQ, BUILD_MULTIVECTOR_PQ`; add the two `case`s to the Execute switch (`result = wrapper->engine->build_pq();` / `build_multivector_pq();`); add their reject messages to the OnError string (line ~1660); add `BuildPq`/`BuildMultivectorPq` methods (state-guard → `MaintenanceWorker(..., BUILD_PQ)` → `worker->Queue(); return worker->Promise();`); register `InstanceMethod("buildPq", …)` / `InstanceMethod("buildMultivectorPq", …)`. Returns a Promise like the other builders (subject to the same MAINTENANCE busy-guard).

## 4. Testing

**Python** (`python/tests/`, new `test_pq_config.py` or extend a vectors test):
- `pq={"m":0,"posture":"replace"}` on a dim-N index: add docs, `flush()`, `build_pq()`, `search_vector(q,k)` returns hits; top-1 is the planted nearest (replace-ADC recall floor).
- `pq={"posture":"rerank","resident_tier":"int8"}`: `build_pq()`, search returns exact-ish top-1 (rerank tier).
- `multivectorPq={"m":0}` with `rerank={"dimension":N}`: add multi-vector docs, `flush()`, `build_multivector_pq()`, `search_rerank(...)` returns hits.
- Reopen (new `SegmentIndex` on same dir) with the same `pq` bag: search still works (persisted config honored) — no re-`build_pq` needed for already-built segments.
- Mutual exclusion: `quantize="int8"` + `pq={...}` → PQ stays off, constructor + open do NOT throw; a plain `search_vector` still works.
- Bad enum: `pq={"posture":"bogus"}` → `ValueError` at construction.

**Node** (`nodejs/test/`, mirror an existing vectors test with the project's runner):
- The same matrix in JS: `pq:{m:0,posture:'replace'}` → `await buildPq()` → `searchVector` hits; `multivectorPq` + `rerank` → `await buildMultivectorPq()` → `searchRerank` hits; reopen honors persisted config; `quantize:'int8'` + `pq` leaves PQ off without throwing; `pq:{posture:'bogus'}` throws `TypeError`.

**Environment caveat:** verifying either binding requires it to build here. The Python extension builds in this environment (confirmed by the #13/#14/#15 work). The Node addon build (`node-gyp`) is unverified here; if `npm`/`node-gyp`/toolchain can't build it, implement + code-review the Node changes and report the Node test run as environment-blocked (do not fabricate a pass). The Python half is independently shippable.

## 5. Sequencing (implementation tasks)

1. **Python `pq` + `multivectorPq` + build methods + tests** (independently verifiable here).
2. **Node `pq` + `multivectorPq` + `MaintenanceWorker` build ops + tests** (report environment-blocked if the addon can't build here).

Split by binding so a Node build blocker can't hold up the Python half.

## 6. Repo constraints

Engine setters are POST-open (apply in `open()` after a successful open, before first flush), immutable/persisted, and return nonzero on rejection → treat as non-fatal at apply (match `quantize`/`rerank`). Node config keys camelCase, Python snake_case. Python: `py::value_error`→ValueError, string enums like existing `quantize`. Node: `Napi::TypeError` for bad enums, async builders via `MaintenanceWorker` + Promise. No header changes → no `rm obj/*.o` needed for the binding-only edits, but the Python extension's own `make` already cleans engine objects (#13). Confirm exact line numbers by grep before editing.

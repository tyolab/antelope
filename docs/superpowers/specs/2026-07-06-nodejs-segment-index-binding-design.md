# Node.js Binding for ATIRE_segment_index — Design

**Date:** 2026-07-06
**Status:** Draft for review
**Parent work:** the segmented incremental index (Phases 1+2) and hybrid vector search V1,
all shipped on master. This document specifies the Node.js binding that makes
`ATIRE_segment_index` usable from JavaScript.

**Problem:** the engine's only JS surface is the legacy SWIG wrapper, whose generated
V8 code requires Node ≤ 14 and a fragile `.so`-copy install. The new segment-index API
(including `Float32Array` embeddings) needs a modern, self-contained binding.

**Approach (approved):** a hand-written **Node-API** addon using `node-addon-api`,
statically linked against the engine, shipped inside the existing `antelope-search`
package beside the untouched legacy wrapper. Fast operations synchronous;
`flush()`/`maintain()` asynchronous on a background thread with a busy-guard.

---

## 1. Decisions

| Question | Decision | Rationale |
|---|---|---|
| Binding technology | Hand-written Node-API (`node-addon-api`, NAPI version 8) | ABI-stable: one `.node` works on Node ≥ 12.22 and Electron; ends the SWIG Node ≤ 14 constraint. The surface is one class with ~14 methods — hand-written beats generated at this size. |
| Linking | **Static**: engine objects compiled/linked into `antelope_segment.node` | Self-contained artifact; retires the `.so`-copy-to-/usr/local/lib install step and the ELF symbol-shadowing workaround. Precedent: `nodejs/binding-static.gyp`. |
| Threading | Sync fast path; `flush()`/`maintain()` return Promises via `Napi::AsyncWorker`; busy-guard makes concurrent engine calls throw | Adds/searches are µs–ms; compaction can take seconds and must not stall the event loop. The engine is single-threaded — the guard makes that contract explicit. |
| Handle representation | `{ generation, docid }` plain object | The packed 64-bit handle exceeds `Number.MAX_SAFE_INTEGER` once generations pass 2^13; an object is self-describing and avoids BigInt friction. |
| Vectors | `Float32Array` (also accepts `number[]`, converted); binding passes the buffer pointer, engine copies at add-time | Zero marshalling layer; no lifetime issues because `add_document` copies internally. |
| Package | Same npm package (`antelope-search`), new export `SegmentIndex`; legacy SWIG addon untouched | No forced migration; existing consumers unaffected. |
| Legacy wrapper | Frozen as-is | Out of scope. |

## 2. Package layout

```
nodejs/
  addon/segment_index.cpp      # the entire binding (one file; ~600 lines)
  binding.gyp                  # gains a second target "antelope_segment"; legacy target untouched
  index.js                     # gains: exports.SegmentIndex = require(<built addon>).SegmentIndex
  segment_index.d.ts           # hand-written TypeScript definitions
  test/segment_index.test.js   # node --test suite
  package.json                 # + devDependency node-addon-api; + npm scripts build:segment / test:segment
```

The `antelope_segment` gyp target compiles `addon/segment_index.cpp` together with the
engine translation units (all `source/*.cpp` plus the non-main `atire/*.cpp`, mirroring
the GNUmakefile's `SOURCES` set — the exact mechanism, source list vs. prebuilt static
archive, is an implementation-plan decision; the requirement is a self-contained `.node`
with no runtime `.so` dependency on the engine). Compiled with the same feature defines
as the GNUmakefile (`IMPACT_HEADER`, `FILENAME_INDEX`, `SPECIAL_COMPRESSION`,
`PARALLEL_INDEXING_DOCUMENTS`, `HASHER`, …) — divergence here produces subtle
format incompatibilities, so the plan must enumerate them from the GNUmakefile verbatim.

## 3. JavaScript API

```ts
type Metric = 'dot' | 'cosine' | 'l2';
interface SegmentIndexOptions {
	dimension?: number;        // enables vectors; fixed at index creation
	metric?: Metric;           // default 'dot'; only meaningful with dimension
	flushThreshold?: number;   // docs per auto-flush; 0 = manual; default 10000
	mergeFactor?: number;      // default 10
	tombstoneRatio?: number;   // default 0.25
	autoMaintain?: boolean;    // default false
}
interface DocRef { generation: number; docid: number; }
interface Hit extends DocRef { key: string; score: number; }

class SegmentIndex {
	constructor(options?: SegmentIndexOptions);
	open(directory: string): void;                    // throws on failure (incl. config mismatch)
	close(): void;                                    // idempotent; further calls throw

	addDocument(key: string, text: string, vector?: Float32Array | number[]): DocRef;
	updateDocument(key: string, text: string, vector?: Float32Array | number[]): DocRef;
	deleteDocument(key: string): boolean;             // false = key unknown

	search(text: string, k: number): Hit[];
	searchVector(vector: Float32Array | number[], k: number): Hit[];
	searchHybrid(text: string | null, vector: Float32Array | number[] | null, k: number): Hit[];

	flush(): Promise<void>;                           // rejects on engine failure
	maintain(): Promise<void>;

	documentCount(): number;
	vectorDimension(): number;                        // 0 = vectors disabled
}
```

Mapping to the C++ surface:
- Constructor stores options; `open()` applies them (`set_vector_config` before the
  engine `open`, then the setters). Metric strings map to the engine enum; unknown
  strings throw `TypeError`.
- `addDocument`/`updateDocument` return the `{generation, docid}` decomposition of the
  engine handle. Engine `-1` becomes a thrown `Error` whose message distinguishes the
  binding-detectable causes (no vector support configured, zero vector under cosine,
  dimension mismatch — all checked in the binding before the engine call) from the
  engine-side rejection ("document rejected: empty or unparseable, or index is in a
  degraded read-only state").
- `search*` build the `Hit[]` from the engine's hit array (deep copies — the JS objects
  own their strings; engine result lifetime never escapes the call).
- `searchHybrid(null, null, k)` returns `[]`. Non-null text with `k < 1` returns `[]`.
- Vector arguments: `Float32Array` used directly (buffer pointer + length check);
  `number[]` converted to a temporary float buffer; anything else throws `TypeError`;
  length must equal `vectorDimension()` exactly.
- `documentCount`/`vectorDimension` are trivial forwards.

## 4. Threading model

- One `ATIRE_segment_index*` per `SegmentIndex` instance, owned by the wrapper,
  deleted at `close()` or GC finalization (whichever first; `close()` nulls the pointer).
- A per-instance state flag: `IDLE` or `MAINTENANCE`.
- `flush()`/`maintain()`: if state != IDLE → rejected Promise. Otherwise set
  `MAINTENANCE`, dispatch a `Napi::AsyncWorker` whose `Execute()` calls the engine
  method (no JS access), and whose `OnOK`/`OnError` restore `IDLE` and settle the
  Promise (nonzero engine return → rejection with a message naming the operation).
- Every synchronous method checks the flag at entry and throws
  `Error("maintenance in progress")` when not IDLE. This is the entire concurrency
  story: the engine never sees two concurrent calls.
- `close()` while MAINTENANCE throws (must await first). GC finalization during
  MAINTENANCE cannot occur (the AsyncWorker holds a `Napi::ObjectReference` on the
  wrapper for its lifetime).

## 5. Error handling summary

| Condition | JS behavior |
|---|---|
| `open` failure (bad dir, config mismatch, corrupt state) | throws `Error` with cause text |
| method before `open` / after `close` | throws `Error("index is not open")` |
| busy (maintenance in flight) | throws / rejects `Error("maintenance in progress")` |
| vector on non-vector index; wrong dimension; wrong type | throws `TypeError` (binding-level, pre-engine) |
| zero vector under cosine | throws `Error` |
| engine add/update rejection | throws `Error` (empty/unparseable document or degraded index) |
| `deleteDocument` unknown key | returns `false` (not an exception — expected outcome) |
| `flush`/`maintain` engine failure | Promise rejection |

## 6. Testing

`nodejs/test/segment_index.test.js`, run with `node --test` against the locally built
addon (npm script `test:segment`; build via `build:segment`):

- Lifecycle: open fresh dir → add (with/without vectors) → search/searchVector/
  searchHybrid ordering and `Hit` shape → update changes results → delete removes →
  `await flush()` → reopen in a new instance → persistence assertions.
- Compaction: delete past the tombstone ratio, `await maintain()`, `documentCount()`
  drop observable, hybrid results still correct.
- Busy-guard: start `maintain()` on a large-enough index (or `flush()`), immediately
  call `addDocument` → throws; after `await`, calls succeed.
- Options: metric strings; dimension mismatch `TypeError`; config-mismatch reopen throws;
  `number[]` acceptance; defaults applied.
- Error table: every §5 row exercised.
- The full C++ suite (7 binaries) as regression alongside — the addon build must not
  perturb the GNUmakefile build.

## 7. Out of scope

- Prebuilt binaries / CI build matrix / npm publish workflow.
- Any change to the legacy SWIG wrapper, its gyp targets, or its exports.
- Worker-thread pooling, concurrent readers, streaming APIs.
- WASM target.

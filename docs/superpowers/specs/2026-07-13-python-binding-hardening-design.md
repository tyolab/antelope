# Python Binding Hardening Design

**Status:** approved 2026-07-13, ready for implementation planning. Issues #13, #14, #15 (bundled — all Minor, same binding, same holistic review).

**Goal:** Three small hardening fixes to the pybind11 `antelope` binding: a stale-object clean before a from-source build (#13), a clear error when multi-vectors are passed without rerank configured (#14), and RAII cleanup of the built `ANT_filter*` across the search methods (#15).

**Architecture (one sentence):** Three localized edits — one to `python/setup.py` (clean objects before `make`), and two to `python/src/antelope_core.cpp` (a `rerank_configured()` guard before multi-vector extraction, and `std::unique_ptr<ANT_filter>` at the eight search sites).

**Tech stack:** Python packaging (`setup.py`, `distutils/setuptools` `build_ext`) + C++ pybind11 (`antelope_core.cpp`). Engine accessors used: `rerank_configured()` (returns `rerank_dimension_current != 0`), `rerank_dimension()`.

**Scope:** binding-only. **Environment caveat:** verifying #14's new test and the #13 build requires the pybind extension to actually compile here (full engine + pybind11 + the pip build). If the extension cannot build in this environment, the changes are still implemented and code-reviewed; the Python test run is reported as environment-blocked, not silently skipped.

---

## 1. #13 — stale-object clean before rebuild (`python/setup.py`)

`make_then_build.run()` currently:
```python
subprocess.check_call(["make", "all"], cwd=REPO)
subprocess.check_call(["make", "engine_lib"], cwd=REPO)
# ...assert the six archives exist...
super().run()
```
The engine has no header dependency tracking, so a changed engine header with stale `obj/*.o` links an inconsistent `lib/libantelope_engine.a`; the extension then segfaults at runtime (this actually bit the shipped binding). **Fix:** unconditionally clear objects + the engine archive before `make all`:
```python
subprocess.check_call("rm -f obj/*.o lib/libantelope_engine.a", shell=True, cwd=REPO)
subprocess.check_call(["make", "all"], cwd=REPO)
subprocess.check_call(["make", "engine_lib"], cwd=REPO)
```
Unconditional (not a staleness heuristic): a from-source pip build is already a full compile, and correctness beats shaving an incremental rebuild. The existing archive-existence assertion stays. Acceptance: a binding rebuild after an engine-header change produces a consistent archive (no runtime SEGV); `pip install ./python` from a tree with stale `obj/*.o` still yields a working extension.

## 2. #14 — clear error when multi-vectors passed but rerank unconfigured (`python/src/antelope_core.cpp`)

Two sites call `extract_multivectors(rows, engine->rerank_dimension(), &num)`: the `write_document` multi-vector branch (~line 844) and `search_rerank` (~line 1166). When rerank is unconfigured, `rerank_dimension()` is 0, so `extract_multivectors` → `extract_vector(row, 0)` throws `py::type_error("vectors are not enabled on this index")` — misleading, since dense vectors may be enabled; it is *rerank* (multi-vector) support that is not configured. **Fix:** immediately before each of those two `extract_multivectors` calls, add:
```cpp
if (!engine->rerank_configured())
	throw py::value_error("multi-vectors given but rerank is not configured (pass rerank={'dimension': N} to the constructor)");
```
`py::value_error` surfaces as Python `ValueError`. Behavior stays safe (it already threw); only the message/type improves and now names the real fix. Place the guard so it fires only on the path where multi-vectors are actually supplied (inside the existing "multi_vectors given" branch for `write_document`; unconditionally at the top of `search_rerank`, which inherently requires rerank).

## 3. #15 — RAII for the built `ANT_filter*` (`python/src/antelope_core.cpp`)

Eight search methods build `ANT_filter *flt = parse_filter_option(filter, engine);`, use it inside a `py::gil_scoped_release` scope, then `delete flt;` (sites at lines 946, 966, 990, 1012, 1032, 1053, 1074, 1175). If `engine->search*()` ever threw, control would skip `delete flt` and leak the filter (theoretical today — the engine returns status codes, does not throw — but it is manual cross-language memory management). **Fix:** at each site, hold the filter in `std::unique_ptr<ANT_filter>`:
```cpp
std::unique_ptr<ANT_filter> flt(parse_filter_option(filter, engine));
{
	py::gil_scoped_release release;
	count = flt ? engine->search(&buf[0], k, flt.get()) : engine->search(&buf[0], k);
}
// (no manual delete)
```
The default `std::default_delete<ANT_filter>` calls `delete`, identical to the current manual free (which relies on `~ANT_filter` recursively freeing the tree), so there is no ownership/double-free change — only exception safety. Apply uniformly to all eight sites; remove each `delete flt;`. Ensure `<memory>` is included (add if absent).

## 4. Testing

- **#14:** new `python/tests` case — construct an index WITHOUT `rerank=`, call the multi-vector path (`write_document(..., multi_vectors=[[...]])` and/or `search_rerank([...])`), and assert it raises `ValueError` whose message contains "rerank is not configured". Mirror the existing python test structure.
- **#15:** no new test (the throwing path is unreachable with today's engine); the existing `python/tests` filtered-search suite is the happy-path regression (unchanged results, no leak under a normal run — optionally spot-check under ASan if the build supports it).
- **#13:** verified by a clean from-source build (`pip install ./python` or the project's build command) yielding an importable extension + green `python/tests`.
- If the extension cannot build in this environment, report which checks are environment-blocked; the C++ edits are still verified to compile as part of the engine/pybind translation unit where possible, and reviewed for correctness.

## 5. Sequencing (TDD tasks)

1. **#13 setup.py:** add the unconditional `rm -f obj/*.o lib/libantelope_engine.a` before `make all`; verify a from-source build still produces the archives + an importable extension (or report environment-blocked).
2. **#14 + #15 antelope_core.cpp:** the `rerank_configured()` guards at both extract sites + the new `ValueError` test (write first, watch it assert the OLD misleading message, then fix); the `std::unique_ptr<ANT_filter>` conversion at all eight search sites; rebuild + run `python/tests`.

## 6. Repo constraints

Engine has no header dep tracking (the very cause of #13); `python/setup.py`'s `build_ext` runs the engine `make`; pybind11 module is `antelope` (`antelope.SegmentIndex`); `py::value_error`→`ValueError`, `py::type_error`→`TypeError`; `ANT_filter` is built by `parse_filter_option` (returns NULL when no filter option) and freed by `delete` (recursive dtor); the eight search sites are `search`/`search_vector`/`search_hybrid` and their variants plus `search_rerank` — grep `delete flt` for the exact set.

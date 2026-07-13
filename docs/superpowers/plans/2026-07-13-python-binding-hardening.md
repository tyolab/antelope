# Python Binding Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Three localized hardening fixes to the pybind11 `antelope` binding — clean stale objects before a from-source build (#13), raise a clear `ValueError` when multi-vectors are passed without rerank configured (#14), and RAII-wrap the built `ANT_filter*` across the eight search sites (#15).

**Architecture:** One edit to `python/setup.py` (unconditional `rm -f obj/*.o lib/libantelope_engine.a` before `make all`) and two kinds of edit to `python/src/antelope_core.cpp` (a `rerank_configured()` guard before each of the two `extract_multivectors` calls, and `std::unique_ptr<ANT_filter>` at all eight `parse_filter_option`/`delete flt` sites).

**Tech Stack:** Python packaging (`setuptools` `build_ext`) + C++ pybind11 (`antelope_core.cpp`), engine static lib built via `make`. Tests: `pytest` under `python/tests/`.

---

## Environment caveat (read first)

Verifying #14's new test and #13's build both require the pybind extension to compile in THIS environment (full engine + pybind11 + pip build). If the extension cannot build here:
- Still make all three code edits and confirm they are syntactically correct / match the spec.
- Report which checks are **environment-blocked** (the pytest run for #14, the from-source build for #13) rather than silently skipping or claiming a green run.
- Do NOT fabricate a passing test run.

Before any build after a header change, the engine has no header dependency tracking, so run `rm -f obj/*.o lib/libantelope_engine.a` from the repo root first (this is the very defect #13 fixes). For the Python extension specifically, a stale-object rebuild is what setup.py must now guard against.

## File Structure

- **`python/setup.py`** — `make_then_build.run()` (lines 15–23) drives the engine build before the extension compile. #13 adds a clean step here.
- **`python/src/antelope_core.cpp`** — the binding. #14 adds two guards (before the `extract_multivectors` calls at lines 844 and 1166); #15 converts eight `ANT_filter*` sites (lines 940/960/984/1006/1026/1047/1068/1168 build; 946/966/990/1012/1032/1053/1074/1175 free); needs `#include <memory>` (currently absent).
- **`python/tests/test_errors.py`** — existing error-path suite (mirrors `pytest.raises(ValueError)` style). #14's new test lands here.

---

## Task 1: #13 — clean stale objects before the from-source build

**Files:**
- Modify: `python/setup.py:15-23`

- [ ] **Step 1: Read the current `make_then_build.run()`**

Confirm it currently reads (lines 15–23):

```python
class make_then_build(build_ext):
    def run(self):
        # externals + engine objects, then archive; externals are gitignored artifacts absent in a fresh checkout
        subprocess.check_call(["make", "all"], cwd=REPO)
        subprocess.check_call(["make", "engine_lib"], cwd=REPO)
        missing = [a for a in ARCHIVES if not os.path.exists(a)]
        if missing:
            raise SystemExit("missing engine archives after make: %s" % missing)
        super().run()
```

- [ ] **Step 2: Add the unconditional clean before `make all`**

Insert a clean step as the first line of the build body so a changed engine header never links against stale `obj/*.o`:

```python
class make_then_build(build_ext):
    def run(self):
        # externals + engine objects, then archive; externals are gitignored artifacts absent in a fresh checkout
        # engine has no header dependency tracking -> clear stale objects/archive so a header change can't link a
        # mismatched libantelope_engine.a (which then SEGVs at runtime). A from-source build is already a full compile.
        subprocess.check_call("rm -f obj/*.o lib/libantelope_engine.a", shell=True, cwd=REPO)
        subprocess.check_call(["make", "all"], cwd=REPO)
        subprocess.check_call(["make", "engine_lib"], cwd=REPO)
        missing = [a for a in ARCHIVES if not os.path.exists(a)]
        if missing:
            raise SystemExit("missing engine archives after make: %s" % missing)
        super().run()
```

The archive-existence assertion (lines 20–22) stays unchanged.

- [ ] **Step 3: Verify the from-source build produces a working extension**

Run (from repo root):

```bash
pip install --force-reinstall ./python
python -c "import antelope; print(antelope.__file__)"
```

Expected: build runs `rm -f ...` then `make all`/`make engine_lib`, produces the six archives, compiles `antelope._core`, and the import succeeds.

**If the extension cannot build in this environment:** report #13's build check as environment-blocked (per the Environment caveat). The edit is still verified by inspection — the `rm -f` line runs before `make all` with `shell=True, cwd=REPO`.

- [ ] **Step 4: Commit**

```bash
git add python/setup.py
git commit -m "fix(python): clear stale engine objects before from-source build (#13)"
```

---

## Task 2: #14 + #15 — rerank guard + RAII filter in antelope_core.cpp

**Files:**
- Modify: `python/src/antelope_core.cpp` (add `#include <memory>`; two `rerank_configured()` guards; eight `unique_ptr<ANT_filter>` conversions)
- Test: `python/tests/test_errors.py`

- [ ] **Step 1: Write the failing #14 test**

Append to `python/tests/test_errors.py` (mirrors the existing `pytest.raises(ValueError)` cases). The index has dense vectors enabled but NO `rerank=`, so `rerank_dimension()` is 0 and passing multi-vectors currently raises the misleading `TypeError("vectors are not enabled on this index")`. Both the `write_document` path and the `search_rerank` path must raise `ValueError` naming rerank:

```python
def test_multivectors_without_rerank_raises_valueerror():
    import tempfile, pytest, antelope
    # dense vectors enabled, but rerank (multi-vector) support is NOT configured
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        with pytest.raises(ValueError) as e1:
            ix.add_document("d0", "<DOC>x</DOC>", vector=[1, 0, 0, 0], multi_vectors=[[1, 0, 0, 0]])
        assert "rerank is not configured" in str(e1.value)
        with pytest.raises(ValueError) as e2:
            ix.search_rerank("x", [1, 0, 0, 0], [[1, 0, 0, 0]], first_stage_n=10, k=5)
        assert "rerank is not configured" in str(e2.value)
```

- [ ] **Step 2: Run it to watch it fail against the OLD behavior**

Run:

```bash
cd python && python -m pytest tests/test_errors.py::test_multivectors_without_rerank_raises_valueerror -v
```

Expected: FAIL — the calls raise `TypeError("vectors are not enabled on this index")` (from `extract_vector(row, 0)`), not `ValueError` with "rerank is not configured". (If the extension can't build here, this is the environment-blocked check — proceed to implement and note it.)

- [ ] **Step 3: Add `#include <memory>`**

The top of `antelope_core.cpp` includes `<cstdio> <cstdlib> <stdexcept> <string> <vector>` but not `<memory>`. Add it (needed for `std::unique_ptr` in #15):

```cpp
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>
```

- [ ] **Step 4: Add the #14 guard in the `write_document` multi-vector branch**

At line ~842, the branch is:

```cpp
	if (has_mv)
		{
		mv = extract_multivectors(multi_vectors, engine->rerank_dimension(), &num_mv);	// touches Python -> must be before gil release
		mvptr = num_mv > 0 ? mv.data() : NULL;
		}
```

Add the guard as the first statement inside the branch, before `extract_multivectors`:

```cpp
	if (has_mv)
		{
		if (!engine->rerank_configured())
			throw py::value_error("multi-vectors given but rerank is not configured (pass rerank={'dimension': N} to the constructor)");
		mv = extract_multivectors(multi_vectors, engine->rerank_dimension(), &num_mv);	// touches Python -> must be before gil release
		mvptr = num_mv > 0 ? mv.data() : NULL;
		}
```

- [ ] **Step 5: Add the #14 guard in `search_rerank`**

At line ~1165, before the `extract_multivectors` call:

```cpp
	long long num_qv = 0;
	std::vector<float> qmv = extract_multivectors(query_multi_vectors, engine->rerank_dimension(), &num_qv);
```

Insert the guard immediately before the `extract_multivectors` line (search_rerank inherently requires rerank, so the guard is unconditional here):

```cpp
	long long num_qv = 0;
	if (!engine->rerank_configured())
		throw py::value_error("multi-vectors given but rerank is not configured (pass rerank={'dimension': N} to the constructor)");
	std::vector<float> qmv = extract_multivectors(query_multi_vectors, engine->rerank_dimension(), &num_qv);
```

- [ ] **Step 6: Convert all eight `ANT_filter*` sites to `std::unique_ptr` (#15)**

Each of the eight sites currently follows this shape (the search call differs per site — `search`/`search_vector`/`search_hybrid`/... and `search_rerank`; preserve each site's exact engine call and arguments, only changing the pointer's ownership and the `flt`→`flt.get()` usages, and removing the manual `delete`):

```cpp
	ANT_filter *flt = parse_filter_option(filter, engine);
	long long count;
	{
	py::gil_scoped_release release;
	count = flt ? engine->search(&buf[0], k, flt) : engine->search(&buf[0], k);
	}
	delete flt;
```

becomes:

```cpp
	std::unique_ptr<ANT_filter> flt(parse_filter_option(filter, engine));
	long long count;
	{
	py::gil_scoped_release release;
	count = flt ? engine->search(&buf[0], k, flt.get()) : engine->search(&buf[0], k);
	}
```

Apply to all eight sites — build lines currently at 940, 960, 984, 1006, 1026, 1047, 1068, 1168; corresponding `delete flt;` at 946, 966, 990, 1012, 1032, 1053, 1074, 1175. At each site: (a) change `ANT_filter *flt = parse_filter_option(...);` to `std::unique_ptr<ANT_filter> flt(parse_filter_option(...));`, (b) change every `flt` passed to an `engine->search*` call to `flt.get()` (the `flt ? ...` truthiness test works unchanged on `unique_ptr`), and (c) delete the `delete flt;` line. `std::default_delete<ANT_filter>` calls `delete`, identical to the current manual free (which relies on `~ANT_filter` recursively freeing the predicate tree) — no ownership/double-free change, only exception safety.

Grep to confirm none remain:

```bash
cd python && grep -n "delete flt\|ANT_filter \*flt" src/antelope_core.cpp
```

Expected: no matches (all eight converted).

- [ ] **Step 7: Rebuild and run the #14 test + regression suites**

Run:

```bash
cd python && pip install --force-reinstall . && python -m pytest tests/test_errors.py tests/test_rerank.py tests/test_filter.py -v
```

Expected: PASS — `test_multivectors_without_rerank_raises_valueerror` now passes (both paths raise `ValueError` naming rerank), and the existing rerank/filter suites are unchanged (the RAII conversion is behavior-preserving; the filtered-search happy paths still return the same results).

**If the extension cannot build in this environment:** report the pytest run as environment-blocked. The edits are still verified by inspection: `<memory>` included, two guards added before the two `extract_multivectors` calls, eight sites converted with no `delete flt`/raw `ANT_filter *flt` remaining.

- [ ] **Step 8: Commit**

```bash
git add python/src/antelope_core.cpp python/tests/test_errors.py
git commit -m "fix(python): clear rerank-unconfigured error + RAII filter cleanup (#14, #15)"
```

---

## Self-review notes

- **Spec coverage:** #13 → Task 1; #14 → Task 2 Steps 4–5 (guards) + Step 1 test; #15 → Task 2 Step 6 (eight sites) + Step 3 (`<memory>`). All three spec sections mapped.
- **Message string** is identical in both #14 guards: `"multi-vectors given but rerank is not configured (pass rerank={'dimension': N} to the constructor)"` — the test asserts the substring `"rerank is not configured"`, which both contain.
- **`rerank_configured()`** returns `rerank_dimension_current != 0` (engine accessor, `atire_segment_index.h`) — confirmed to exist before use.
- **No new tests for #15** (its throwing path is unreachable with today's status-code engine); the existing filter suite is the regression lock.

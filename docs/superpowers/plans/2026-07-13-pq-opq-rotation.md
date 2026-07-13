# Dense PQ OPQ Rotation Implementation Plan (#22 sub-project 1/3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in, deterministic OPQ rotation to the dense `.pq` store — a learned orthogonal `R` (D×D) applied before subspace splitting — improving recall at the same m/k, metric-exactly (orthogonal rotation preserves dot/L2/cosine) and byte-deterministically (same platform).

**Architecture:** `ANT_pq_codec` gains pure rotation helpers (deterministic symmetric Jacobi eigensolver + eigenvalue-balanced subspace allocation + apply/apply-transpose). `ANT_pq_store` owns `R`, rotates the query once before each `adc_table`, un-rotates `reconstruct` via `Rᵀ`, and persists `R` in a v2 `.pq` sidecar. `set_pq_opq(enable)` gates it (persisted in `pq.config` v3, immutable). Default off ⇒ non-OPQ path byte-identical.

**Tech Stack:** C++ engine — `source/pq_codec.{h,cpp}`, `source/pq_store.{h,cpp}`, `atire/atire_segment_index*`, tests `tests/*.cpp` (auto-discovered → `bin/<name>` via `make <name>`, `CHECK()`).

---

## Repo setup / gotchas (read first)

- Fresh worktree: `mkdir -p obj bin lib` + copy `external/**/*.a` from the main checkout (gitignored build products).
- Header change (`pq_codec.h`, `pq_store.h`, `atire_segment_index.h`) → `rm -f obj/*.o lib/libantelope_engine.a` before rebuild (no header dep tracking).
- Build+run a test: `make <name>` → `./bin/<name>`, exit 0 = pass. `source/*.cpp`+`tests/*.cpp` auto-discovered. `ls: cannot access 'tools/*.cpp'` line is harmless.
- ASan: the GNUmakefile has NO sanitizer hook (only `USE_GCC_DEBUG` = `-g`). Report the ASan sweep as environment-blocked (do not invent a build) — as in prior PQ sub-projects.
- **Contracts to preserve:** `.pq` deterministic-rebuild (same input → byte-identical file) and forgiving-load (any corruption → degraded empty store → fallback). Default (no `set_pq_opq`) stays byte-identical to today.

## File Structure

- `source/pq_codec.{h,cpp}` — 3 new pure static methods (Task 1).
- `source/pq_store.{h,cpp}` — `rotation` member, v2 sidecar r/w, query rotation, reconstruct un-rotation, writer `finish` train/rotate/encode + `opq` on `create` (Task 2).
- `atire/atire_segment_index.h` + `atire/atire_segment_index_vector.cpp` — `set_pq_opq`, `pq_opq_current`, `pq.config` v3, writer-create sites pass the flag, compaction retrain (Task 3).
- `tests/test_pq_opq.cpp` (new) — codec + integration tests across all 3 tasks.

---

## Task 1: Codec rotation helpers (`source/pq_codec.{h,cpp}`)

**Files:**
- Modify: `source/pq_codec.h` (declare 3 methods), `source/pq_codec.cpp` (define + a file-static Jacobi)
- Test: `tests/test_pq_opq.cpp` (new; codec-only cases this task)

- [ ] **Step 1: Write the failing codec test**

Create `tests/test_pq_opq.cpp`:

```cpp
/*
	TEST_PQ_OPQ.CPP -- #22 OPQ rotation. Task 1 locks the pure codec helpers:
	R is orthogonal, apply/apply_transpose are inverses, the rotation preserves
	dot product, and training is deterministic.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../source/pq_codec.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void test_rotation_orthogonal_and_dot_preserving(void)
{
	const long long D = 8, m = 4, n = 64;
	// anisotropic data: mix dims so variance is off-axis
	float *vecs = new float[n * D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D + d] = (float)(((i * 7 + d * 13) % 17) - 8) * (d < 3 ? 4.0f : 0.2f);

	float *R = new float[D * D];
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R) == 0);

	// R R^T == I (orthogonal): check a few entries of R*R^T
	for (long long a = 0; a < D; a++)
		for (long long b = 0; b < D; b++)
			{
			double dot = 0;
			for (long long k = 0; k < D; k++) dot += (double)R[a*D+k] * (double)R[b*D+k];
			CHECK(fabs(dot - (a == b ? 1.0 : 0.0)) < 1e-4);
			}

	// apply then apply_transpose round-trips
	float q[D], rq[D], back[D];
	for (long long d = 0; d < D; d++) q[d] = (float)(d + 1) * 0.3f - 1.0f;
	ANT_pq_codec::apply_rotation(q, D, R, rq);
	ANT_pq_codec::apply_rotation_transpose(rq, D, R, back);
	for (long long d = 0; d < D; d++) CHECK(fabs(back[d] - q[d]) < 1e-4);

	// dot product preserved: dot(Rq, Rx) == dot(q, x)
	float x[D], rx[D];
	for (long long d = 0; d < D; d++) x[d] = (float)((d * 3) % 5) - 2.0f;
	ANT_pq_codec::apply_rotation(x, D, R, rx);
	double d_orig = 0, d_rot = 0;
	for (long long d = 0; d < D; d++) { d_orig += (double)q[d]*x[d]; d_rot += (double)rq[d]*rx[d]; }
	CHECK(fabs(d_orig - d_rot) < 1e-3);

	delete [] vecs; delete [] R;
	printf("test_rotation_orthogonal_and_dot_preserving OK\n");
}

static void test_rotation_deterministic(void)
{
	const long long D = 6, m = 3, n = 40;
	float *vecs = new float[n * D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D + d] = (float)(((i * 5 + d * 11) % 13) - 6);
	float *R1 = new float[D*D], *R2 = new float[D*D];
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R1) == 0);
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R2) == 0);
	CHECK(memcmp(R1, R2, (size_t)(D*D)*sizeof(float)) == 0);   // byte-identical
	delete [] vecs; delete [] R1; delete [] R2;
	printf("test_rotation_deterministic OK\n");
}

static void test_rotation_rejects_bad_args(void)
{
	float R[16];
	float v[4] = {1,2,3,4};
	CHECK(ANT_pq_codec::train_rotation(v, 4, 3, 1, R) == 1);  // 3 does not divide 4
	CHECK(ANT_pq_codec::train_rotation(v, 4, 2, 0, R) == 1);  // n == 0
	printf("test_rotation_rejects_bad_args OK\n");
}

int main(void)
{
	test_rotation_orthogonal_and_dot_preserving();
	test_rotation_deterministic();
	test_rotation_rejects_bad_args();
	printf("ALL TESTS PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Run it to confirm it fails to compile/link**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_opq && ./bin/test_pq_opq
```
Expected: FAIL to compile/link (`train_rotation`/`apply_rotation`/`apply_rotation_transpose` undeclared).

- [ ] **Step 3: Declare the three methods in `source/pq_codec.h`**

After `reconstruct(...)` (line 20) add:
```cpp
	// OPQ (#22): learn an orthogonal D*D row-major rotation R (metric-preserving, no centering).
	// train_rotation returns 1 on m-does-not-divide-dimension or n==0 (caller leaves OPQ off).
	static long train_rotation(const float *vectors, long long dimension, long long m, long long n, float *R);
	static void apply_rotation(const float *vec, long long dimension, const float *R, float *out);            // out = R * vec
	static void apply_rotation_transpose(const float *vec, long long dimension, const float *R, float *out);  // out = R^T * vec
```

- [ ] **Step 4: Implement in `source/pq_codec.cpp`**

Add `#include <math.h>` is already present. Append the file-static Jacobi eigensolver and the three methods at the end of the file:

```cpp
/*
	Deterministic cyclic symmetric Jacobi eigensolver. `a` (n*n, row-major,
	symmetric) is DESTROYED; eigenvalues -> eigval[n]; eigenvectors -> columns
	of V[n*n]. Fixed sweep order + fixed cap => deterministic on one platform.
*/
static void ant_pq_jacobi(double *a, long long n, double *eigval, double *V)
{
long long i, j, p, q, k, sweep;
for (i = 0; i < n; i++)
	for (j = 0; j < n; j++)
		V[i*n + j] = (i == j) ? 1.0 : 0.0;
for (sweep = 0; sweep < 100; sweep++)
	{
	double off = 0.0;
	for (p = 0; p < n; p++)
		for (q = p+1; q < n; q++)
			off += a[p*n + q] * a[p*n + q];
	if (off <= 1e-30)
		break;
	for (p = 0; p < n; p++)
		for (q = p+1; q < n; q++)
			{
			double apq = a[p*n + q];
			if (fabs(apq) <= 1e-300)
				continue;
			double app = a[p*n + p], aqq = a[q*n + q];
			double phi = 0.5 * (aqq - app) / apq;
			double t = (phi >= 0 ? 1.0 : -1.0) / (fabs(phi) + sqrt(phi*phi + 1.0));
			double c = 1.0 / sqrt(t*t + 1.0);
			double s = t * c;
			for (k = 0; k < n; k++)
				{
				double akp = a[k*n + p], akq = a[k*n + q];
				a[k*n + p] = c*akp - s*akq;
				a[k*n + q] = s*akp + c*akq;
				}
			for (k = 0; k < n; k++)
				{
				double apk = a[p*n + k], aqk = a[q*n + k];
				a[p*n + k] = c*apk - s*aqk;
				a[q*n + k] = s*apk + c*aqk;
				}
			for (k = 0; k < n; k++)
				{
				double vkp = V[k*n + p], vkq = V[k*n + q];
				V[k*n + p] = c*vkp - s*vkq;
				V[k*n + q] = s*vkp + c*vkq;
				}
			}
	}
for (i = 0; i < n; i++)
	eigval[i] = a[i*n + i];
}

long ANT_pq_codec::train_rotation(const float *vectors, long long dimension, long long m, long long n, float *R)
{
long long i, j, d, D = dimension;
if (m < 1 || dimension % m != 0 || n <= 0)
	return 1;

// second-moment matrix M = sum_i x_i x_i^T  (D*D, symmetric, NO centering -> metric-preserving)
std::vector<double> M((size_t)(D*D), 0.0);
for (i = 0; i < n; i++)
	{
	const float *x = vectors + i * D;
	for (d = 0; d < D; d++)
		{
		double xd = (double)x[d];
		double *row = &M[(size_t)(d*D)];
		for (j = 0; j < D; j++)
			row[j] += xd * (double)x[j];
		}
	}

std::vector<double> eigval((size_t)D), V((size_t)(D*D));
ant_pq_jacobi(&M[0], D, &eigval[0], &V[0]);   // V columns = eigenvectors (M destroyed)

// sign-canonicalize each eigenvector column: force largest-|component| positive (kills sign ambiguity)
for (j = 0; j < D; j++)
	{
	long long best = 0; double bestmag = -1.0;
	for (d = 0; d < D; d++)
		{ double mg = fabs(V[(size_t)(d*D + j)]); if (mg > bestmag) { bestmag = mg; best = d; } }
	if (V[(size_t)(best*D + j)] < 0.0)
		for (d = 0; d < D; d++)
			V[(size_t)(d*D + j)] = -V[(size_t)(d*D + j)];
	}

// eigenvalue-balanced subspace allocation: assign each eigenvector (desc eigenvalue) to the
// not-yet-full subspace with the smallest running log-variance product -> balanced subspaces.
long long sub = D / m;
std::vector<long long> idx((size_t)D);
for (d = 0; d < D; d++) idx[d] = d;
// stable sort by descending eigenvalue (deterministic tie-break by index via <= comparison)
for (i = 0; i < D; i++)
	for (j = i+1; j < D; j++)
		if (eigval[idx[j]] > eigval[idx[i]])
			{ long long tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp; }
std::vector<double> bucket_log((size_t)m, 0.0);
std::vector<long long> bucket_cnt((size_t)m, 0);
std::vector<long long> order((size_t)D, -1);   // output row -> eigenvector index
std::vector<long long> bucket_fill((size_t)m, 0);
for (i = 0; i < D; i++)
	{
	long long e = idx[i];
	long long chosen = -1; double best_log = 0;
	for (long long b = 0; b < m; b++)
		{
		if (bucket_cnt[b] >= sub) continue;
		if (chosen < 0 || bucket_log[b] < best_log) { chosen = b; best_log = bucket_log[b]; }
		}
	double lam = eigval[e]; if (lam < 1e-12) lam = 1e-12;
	bucket_log[chosen] += log(lam);
	// place at subspace `chosen`'s next contiguous output row
	order[(size_t)(chosen*sub + bucket_fill[chosen])] = e;
	bucket_fill[chosen]++;
	bucket_cnt[chosen]++;
	}

// R rows = allocated eigenvectors (row r takes eigenvector `order[r]`, i.e. column order[r] of V)
for (i = 0; i < D; i++)
	{
	long long e = order[i];
	for (d = 0; d < D; d++)
		R[(size_t)(i*D + d)] = (float)V[(size_t)(d*D + e)];
	}
return 0;
}

void ANT_pq_codec::apply_rotation(const float *vec, long long dimension, const float *R, float *out)
{
long long r, d, D = dimension;
for (r = 0; r < D; r++)
	{
	double acc = 0.0;
	const float *Rr = R + r*D;
	for (d = 0; d < D; d++)
		acc += (double)Rr[d] * (double)vec[d];
	out[r] = (float)acc;
	}
}

void ANT_pq_codec::apply_rotation_transpose(const float *vec, long long dimension, const float *R, float *out)
{
long long r, d, D = dimension;
for (d = 0; d < D; d++)
	out[d] = 0.0f;
for (r = 0; r < D; r++)
	{
	double vr = (double)vec[r];
	const float *Rr = R + r*D;
	for (d = 0; d < D; d++)
		out[d] += (float)(Rr[d] * vr);
	}
}
```
(`<vector>` and `<math.h>` are already included at the top of `pq_codec.cpp`.)

- [ ] **Step 5: Rebuild and run — codec tests pass**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_opq && ./bin/test_pq_opq
```
Expected: `ALL TESTS PASSED` (orthogonality, round-trip, dot-preservation, determinism, bad-arg rejection).

- [ ] **Step 6: Commit**

```bash
git add source/pq_codec.h source/pq_codec.cpp tests/test_pq_opq.cpp
git commit -m "feat(pq): OPQ rotation codec helpers — Jacobi eigensolver + balanced allocation (#22)"
```

---

## Task 2: Store integration (`source/pq_store.{h,cpp}`)

**Files:**
- Modify: `source/pq_store.h` (`rotation` member; `opq` param on `create`), `source/pq_store.cpp` (dtor, load v2, 3 query-rotation sites, prepare_query, reconstruct un-rotation, writer finish)
- Test: extend `tests/test_pq_opq.cpp`

- [ ] **Step 1: Extend the test — OPQ store round-trip, recall, back-compat, determinism**

Append to `tests/test_pq_opq.cpp` (before `main`), and add the calls to `main`. These drive the store through the writer with OPQ on:

```cpp
#include "../source/pq_store.h"
#include "../source/index_tombstones.h"
#include "../source/vector_candidate.h"

// build a .pq at `path` from `n` D-dim rows via the writer, OPQ on/off; returns present count
static void write_pq(const char *path, const float *vecs, long long D, long long m, long long n, long metric, long opq)
{
	ANT_pq_store_writer w;
	CHECK(w.create(path, D, m, metric, opq) == 0);
	for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
	CHECK(w.finish() == 0);
}

static void test_store_opq_roundtrip_and_backcompat(void)
{
	const long long D = 8, m = 4, n = 60;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D+d] = (float)(((i*7 + d*13) % 17) - 8) * (d < 3 ? 4.0f : 0.2f);

	char p_opq[] = "/tmp/ant_opq_XXXXXX";  CHECK(mkstemp(p_opq) >= 0);
	char p_no[]  = "/tmp/ant_no_XXXXXX";   CHECK(mkstemp(p_no) >= 0);
	write_pq(p_opq, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	write_pq(p_no,  vecs, D, m, n, ANT_pq_codec::METRIC_L2, 0);

	ANT_pq_store *s_opq = ANT_pq_store::load(p_opq, D, n, ANT_pq_codec::METRIC_L2);
	ANT_pq_store *s_no  = ANT_pq_store::load(p_no,  D, n, ANT_pq_codec::METRIC_L2);
	CHECK(s_opq != NULL && s_opq->document_count() == n);   // OPQ v2 sidecar loads
	CHECK(s_no  != NULL && s_no->document_count() == n);    // non-OPQ still loads

	// reconstruct under OPQ returns an ORIGINAL-space approximation (R^T applied):
	// its error vs the true vector is comparable to the non-OPQ store (not rotated garbage).
	float recon[8], truth[8];
	double err_opq = 0, err_no = 0;
	for (long long doc = 0; doc < n; doc++)
		{
		for (long long d = 0; d < D; d++) truth[d] = vecs[doc*D+d];
		s_opq->reconstruct(doc, recon);
		for (long long d = 0; d < D; d++) err_opq += (recon[d]-truth[d])*(recon[d]-truth[d]);
		s_no->reconstruct(doc, recon);
		for (long long d = 0; d < D; d++) err_no += (recon[d]-truth[d])*(recon[d]-truth[d]);
		}
	CHECK(err_opq < err_no * 3.0 + 1.0);   // same order of magnitude, not rotated-space nonsense

	delete s_opq; delete s_no;
	remove(p_opq); remove(p_no);
	delete [] vecs;
	printf("test_store_opq_roundtrip_and_backcompat OK\n");
}

static void test_store_opq_deterministic(void)
{
	const long long D = 8, m = 4, n = 50;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*5 + d*11) % 13) - 6) * (d < 2 ? 3.0f : 0.3f);
	char a[] = "/tmp/ant_da_XXXXXX", b[] = "/tmp/ant_db_XXXXXX";
	CHECK(mkstemp(a) >= 0); CHECK(mkstemp(b) >= 0);
	write_pq(a, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	write_pq(b, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
	CHECK(fa && fb);
	fseek(fa,0,SEEK_END); fseek(fb,0,SEEK_END);
	long la = ftell(fa), lb = ftell(fb);
	CHECK(la == lb && la > 0);
	rewind(fa); rewind(fb);
	unsigned char *ba = new unsigned char[la], *bb = new unsigned char[lb];
	CHECK(fread(ba,1,la,fa)==(size_t)la && fread(bb,1,lb,fb)==(size_t)lb);
	CHECK(memcmp(ba, bb, la) == 0);   // byte-identical OPQ .pq
	fclose(fa); fclose(fb); delete[] ba; delete[] bb;
	remove(a); remove(b); delete [] vecs;
	printf("test_store_opq_deterministic OK\n");
}
```
Add `#include <unistd.h>` at the top of the test for `mkstemp`. Add the two new calls into `main` before the final print. (Grep `source/vector_candidate.h` / `index_tombstones.h` exact paths first; adjust includes if the store test needs them — the recall assertion below can use `scan_adc`, else keep to reconstruct which needs no candidate type.) If `ANT_vector_candidate` isn't needed by these two cases, drop that include.

- [ ] **Step 2: Run — watch it fail to compile (`create` arity, no OPQ)**

Run: `rm -f obj/*.o lib/libantelope_engine.a && make test_pq_opq && ./bin/test_pq_opq`
Expected: FAIL — `create(...)` takes 4 args not 5 (no `opq`), and OPQ isn't wired.

- [ ] **Step 3: Add the `rotation` member + free it**

In `source/pq_store.h`, in the private members (after `unsigned char *codes;`) add:
```cpp
	float *rotation;			// D*D OPQ rotation R (row-major), NULL when OPQ off
```
And change the writer's `create` signature (line 55) to:
```cpp
	long create(const char *path, long long dim, long long m, long metric, long opq);
```
Add a private `long opq;` member to `ANT_pq_store_writer` (beside `metric`).

In `source/pq_store.cpp`: init `rotation = NULL;` in the `ANT_pq_store()` ctor (beside `codebook = NULL;`); `delete [] rotation;` in `~ANT_pq_store()` (beside `delete [] codebook;`). In the writer ctor init `opq = 0;`.

- [ ] **Step 4: Sidecar v2 — write `R` (finish) and read/validate it (load)**

Bump the version + header. At the top of `pq_store.cpp` where `ANT_PQ_STORE_VERSION` and `ANT_PQ_STORE_HEADER_SIZE` are defined:
- `ANT_PQ_STORE_VERSION` 1 → 2.
- Add an `opq` i64 to the header: `ANT_PQ_STORE_HEADER_SIZE = 8 + 4 + 8 + 8 + 8 + 8 + 8` (added one i64). Confirm the exact enum line and update the banner comment (magic/version/dimension/documents/m/k/**opq**).

In `ANT_pq_store_writer::create`, store the passed `opq` into the new member.

In `finish()` (the header-write block ~line 430-449): after computing `codebook`/`codes`, when `opq` is set, train + apply the rotation BEFORE training the codebook. Restructure the train/encode section so:
```cpp
	// OPQ: learn R over present rows, rotate ALL rows in-place, then train+encode on rotated data
	float *rotation = NULL;
	if (opq)
		{
		rotation = new float[dimension * dimension];
		if (ANT_pq_codec::train_rotation(present_rows, dimension, m, present_count, rotation) != 0)
			{ delete [] rotation; delete [] present_rows; return 1; }   // (present_rows still needed below — see note)
		}
```
NOTE on ordering: `present_rows` is currently freed right after `train`. Reorder so, when `opq`:
1. build `present_rows` (already there),
2. if opq: `train_rotation(present_rows,...) -> rotation`, then rotate `present_rows` in place (scratch row) AND rotate every row of `buffer` in place (so both training and encoding use rotated vectors),
3. `train(present_rows or rotated, ...) -> codebook`,
4. `encode(buffer[i] rotated, ...) -> codes`.
Concretely: after obtaining `rotation`, allocate `float tmp[dimension]`; for each present row rotate into tmp and copy back; also rotate each of the `documents` rows of `buffer` in place (absent rows are zero → rotate to zero, harmless). Then the existing `train(present_rows,...)` and `encode(buffer + i*dimension,...)` loops operate on rotated data unchanged. Keep the non-opq path byte-identical (skip all rotation when `!opq`).

Then in the header-write: add `long long opq_flag = opq ? 1 : 0;` and write it right after `k`:
```cpp
	|| fwrite(&k, sizeof(k), 1, fp) != 1
	|| fwrite(&opq_flag, sizeof(opq_flag), 1, fp) != 1)
```
And after the codebook write, when `opq`, write the rotation:
```cpp
	if (!failed && opq && fwrite(rotation, sizeof(float), (size_t)(dimension*dimension), fp) != (size_t)(dimension*dimension))
		failed = 1;
```
Free `rotation` alongside `codebook`/`codes` on every exit path.

In `load()` (lines ~67-153): read the new `opq` i64 after `stored_k`; accept `stored_version == 2` (and still accept `1` for old files → `opq=0`, no rotation block). Compute `rotation_floats = (stored_opq ? stored_dimension*stored_dimension : 0)` (bounded: `stored_dimension ≤ 65536` already validated → `dimension² ≤ 2³²`, `*4 ≤ 2³⁴`, fits i64). Add it to `expected_size`. Read the rotation block (after the codes read) into a `new float[...]` when present; assign to `result->rotation`. Validate `stored_opq` is 0 or 1 (else degrade). For a v1 file there is no `opq` field to read — branch the header read on version: read 6 fields for v1, 7 for v2. Keep validate-before-allocate (bound `rotation_floats` before `new`).

- [ ] **Step 5: Rotate the query at the 3 `adc_table` sites + un-rotate reconstruct**

In `pq_store.cpp`, wherever `ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table)` is called (`score` ~192, `prepare_query` ~217, `scan_adc` ~251), rotate the query first when `rotation != NULL`:
```cpp
	const float *q = query;
	float *rq = NULL;
	if (rotation != NULL)
		{
		rq = new float[dimension];
		ANT_pq_codec::apply_rotation(query, dimension, rotation, rq);
		q = rq;
		}
	ANT_pq_codec::adc_table(q, dimension, m, codebook, metric, table);
	delete [] rq;   // delete[] NULL is a no-op
```
(For `score`'s stack-table path keep the existing table alloc; just add the query rotation. For `prepare_query`, rotate before building the returned table; the "build once per search" property holds — one D-matvec per prepare, not per node.)

In `reconstruct` (~162): when `rotation != NULL`, reconstruct into a scratch (rotated space) then `apply_rotation_transpose` into `out`:
```cpp
	if (rotation != NULL)
		{
		float *tmp = new float[dimension];
		ANT_pq_codec::reconstruct(codes + docid * m, dimension, m, codebook, tmp);
		ANT_pq_codec::apply_rotation_transpose(tmp, dimension, rotation, out);
		delete [] tmp;
		}
	else
		ANT_pq_codec::reconstruct(codes + docid * m, dimension, m, codebook, out);
```

- [ ] **Step 6: Rebuild and run — store tests pass; existing PQ tests unchanged**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_opq && ./bin/test_pq_opq
make test_pq_store && ./bin/test_pq_store
make test_pq_search && ./bin/test_pq_search
make test_pq_metrics && ./bin/test_pq_metrics
make test_pq_config && ./bin/test_pq_config
```
Expected: `test_pq_opq` all pass; the existing PQ suites unchanged (non-OPQ path byte-identical; `create` gained an `opq` param — grep all existing `ANT_pq_store_writer` `create(...)` callers in `atire/` and pass `0` there in Task 3; for the standalone `test_pq_store` if it constructs a writer directly, it must pass `0` — update that call if present, it's a mechanical arity fix locking non-OPQ behavior).

- [ ] **Step 7: Commit**

```bash
git add source/pq_store.h source/pq_store.cpp tests/test_pq_opq.cpp
git commit -m "feat(pq): OPQ in the .pq store — v2 sidecar, query rotation, reconstruct un-rotation (#22)"
```

---

## Task 3: Config + lifecycle (`atire/atire_segment_index*`)

**Files:**
- Modify: `atire/atire_segment_index.h` (`pq_opq_current`, `set_pq_opq`, `pq_opq()` getter), `atire/atire_segment_index_vector.cpp` (`pq.config` v3 r/w, `set_pq_opq`, writer-create sites pass the flag)
- Test: `tests/test_pq_opq.cpp` (config persistence + end-to-end recall gain)

- [ ] **Step 1: Write the failing config + recall-gain test**

Append an engine-level case to `tests/test_pq_opq.cpp` (needs `#include "../atire/atire_segment_index.h"`). It builds two indexes over the SAME anisotropic data — one `set_pq_config(..)` only, one also `set_pq_opq(1)` — and asserts OPQ's replace-posture recall@k ≥ the non-OPQ recall against exact-float ground truth; plus a reopen-persistence check (`pq_opq()` restored):

```cpp
// (engine test) — anisotropic dense data; OPQ replace-recall >= non-OPQ replace-recall,
// and set_pq_opq persists across reopen. Mirrors test_pq_metrics' harness for building
// float ground-truth vs a PQ index; see that file for add_document(key,body,vec) + get_hit.
```
Write it modeled on `tests/test_pq_metrics.cpp` (grep it for the exact fixture: `set_vector_config` PRE-open, `open`, `set_pq_config` POST-open, `add_document(key, body, vec)` 3-arg dense, `flush`, `build_pq`, `search_vector`, `get_hit(i)->filename`). Data: dim 8, ~200 docs, variance concentrated in a rotated 3-dim subset so an axis-aligned split is lossy (OPQ should strictly help). Assert `opq_recall >= no_recall` (both vs float top-k; use recall@10 over ~20 queries, exact-float index as truth). Reopen: new `ATIRE_segment_index`, `open` same dir, assert `pq_opq() == 1`.

- [ ] **Step 2: Run — fails (`set_pq_opq`/`pq_opq` undeclared)**

`rm -f obj/*.o lib/libantelope_engine.a && make test_pq_opq && ./bin/test_pq_opq` → FAIL to compile.

- [ ] **Step 3: Add the member, getter, and setter declaration**

In `atire/atire_segment_index.h`: add `long pq_opq_current;` beside `pq_resident_tier_current` (line ~124); a getter `long pq_opq(void) { return pq_opq_current; }` beside `pq_posture()`/`pq_rerank_quant()`; and declare `long set_pq_opq(long enable);` beside `set_pq_resident_tier`. Initialize `pq_opq_current = 0;` in the constructor (grep the ctor init list where `pq_resident_tier_current` is set to 0).

- [ ] **Step 4: `pq.config` v3 — persist `opq`**

In `atire/atire_segment_index_vector.cpp`:
- `save_pq_config` (line ~496): bump `version = 3u`; add `long long opq = pq_opq_current;` and write it after `tier`:
```cpp
	|| fwrite(&tier, sizeof(tier), 1, fp) != 1
	|| fwrite(&opq, sizeof(opq), 1, fp) != 1)
```
- `load_pq_config` (line ~457): accept `version == 3u` too (`version != 1u && version != 2u && version != 3u` → reject); after the `version==2` tier read, add:
```cpp
	long long opq = 0;
	if (version == 3u)
		{
		if (fread(&opq, sizeof(opq), 1, fp) != 1 || (opq != 0 && opq != 1))
			{ fclose(fp); return 0; }
		}
	...
	pq_opq_current = (long)opq;   // after the other pq_*_current assignments
```
(v1/v2 files → `opq=0`, back-compat.)

- [ ] **Step 5: Implement `set_pq_opq` (immutable, opt-in, POST-config)**

Add beside `set_pq_resident_tier`:
```cpp
long ATIRE_segment_index::set_pq_opq(long enable)
{
if (directory == NULL || !pq_configured())
	return 1;						// must be open + PQ configured
long want = enable ? 1 : 0;
if (pq_opq_current == want)
	return 0;						// idempotent
if (pq_opq_current != 0)
	return 1;						// immutable once enabled
pq_opq_current = want;
if (save_pq_config() != 0)
	{ pq_opq_current = 0; return 1; }
return 0;
}
```
(Mirror the exact open/PQ-configured guard style of the surrounding setters — grep `set_pq_resident_tier`'s guards and match.)

- [ ] **Step 6: Pass the OPQ flag into every `.pq` writer create site**

Grep every `ANT_pq_store_writer` `create(` call in `atire/atire_segment_index_vector.cpp` (build_pq backfill, eager-at-flush, compaction retrain). Each currently calls `create(path, dim, m, metric)`; change to `create(path, dim, m, metric, pq_opq_current)` so backfilled/compacted segments train `R` under the configured flag. (Confirm there are no other `create(` callers of the PQ writer elsewhere; the standalone test writer calls were handled in Task 2.)

- [ ] **Step 7: Rebuild and run — config + recall-gain pass; full PQ suite green**

Run:
```bash
rm -f obj/*.o lib/libantelope_engine.a
make test_pq_opq && ./bin/test_pq_opq
for t in test_pq_store test_pq_search test_pq_metrics test_pq_config test_pq_compaction test_pq_resident_tier test_pq_hnsw test_pq_load_hardening; do make $t >/dev/null 2>&1 && ./bin/$t 2>&1 | tail -1 | sed "s/^/$t: /"; done
```
Expected: `test_pq_opq` all pass (incl. recall gain + reopen persistence); every existing PQ suite still passes (non-OPQ byte-identical; `create` arity fixed everywhere).

- [ ] **Step 8: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index_vector.cpp tests/test_pq_opq.cpp
git commit -m "feat(pq): set_pq_opq config + pq.config v3 + build/compaction retrain (#22)"
```

---

## Self-review notes

- **Spec coverage:** codec helpers (Task 1) → §2; store R/sidecar/query-rotation/reconstruct (Task 2) → §3; config + lifecycle (Task 3) → §4. Metric-exactness (§1) locked by Task 1's dot-preservation test; recall gain (§5) by Task 3; determinism, back-compat, reconstruct round-trip, load-hardening by Task 2.
- **Type/name consistency:** `train_rotation`/`apply_rotation`/`apply_rotation_transpose`, `rotation` member, `create(...,opq)`, `pq_opq_current`/`pq_opq()`/`set_pq_opq`, `pq.config` v3 — identical across tasks.
- **Byte-identity:** every rotation path is guarded by `opq`/`rotation != NULL`; the non-OPQ path is untouched (default off).
- **Overflow safety:** `dimension ≤ 65536` (existing load validation) → `dimension² ≤ 2³²` bounds the `R` block before allocation.
- **Determinism:** Jacobi has a fixed sweep order + cap; eigenvector signs are canonicalized; the eigenvalue-allocation sort is a deterministic descending sort with index tie-break — so `R` (and the whole `.pq`) is byte-identical on rebuild (same platform; cross-platform caveat documented in the spec).
- **Line numbers indicative** — implementer confirms by grep before editing.

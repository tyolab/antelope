# Vector V3 HNSW Graph Index Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in, per-segment hand-rolled HNSW graph index for higher-recall/faster approximate vector search, coexisting with V1 (exact) and V2 (SimHash), with transparent exact fallback.

**Architecture:** A new `ANT_hnsw` (source/hnsw.{h,cpp}) builds a hierarchical navigable small-world graph over a segment's dense vectors, searches it for exactly-scored candidates, and serialises a `seg_G.hnsw` CSR sidecar (mirroring `ANT_vector_store`/`ANT_signature_store`). `ATIRE_segment_index` gains an `hnsw.config`, lifecycle hooks (flush/compact/open/backfill), a cached per-segment graph, and `*_hnsw` search methods. Cosine/L2 use the graph; dot and unconfigured indexes fall back to the untouched V1 exact path.

**Tech Stack:** C++ (STL allowed — 37 source files already use it; use `std::vector`/`std::priority_queue` for build-time temporaries, flatten to plain C arrays for storage/search), GNU make (auto-discovers `source/*.cpp` and `tests/*.cpp`), existing `ANT_vector_store`, `ANT_index_tombstones`, `ANT_mersenne_twister`, the Node-API addon.

**Task dependency order (important):** the cached `segment.hnsw_graph` field + its load (Task 6) must land **before** the `*_hnsw` searches (Tasks 7-8) that read it. Do the tasks in the numbered order.

---

## Conventions used throughout

- **Build after any header change:** `rm -f obj/*.o && make all && make tests` (no header dependency tracking). Pure `.cpp` edits can skip the purge.
- **Run a C++ test binary:** `make tests && ./bin/<name>` (each `tests/*.cpp` auto-builds to `bin/<name>`).
- **Segment sidecar filenames are ZERO-PADDED:** `segment_filename(buf, size, generation, "hnsw")` → `<dir>/seg_%06lld.hnsw`. When a test hardcodes a path, use `seg_%06lld.hnsw` (NOT `seg_%lld`).
- **`kernel()` is a similarity (higher = nearer):** `ANT_vector_store::kernel(a, b, dim, metric)` returns cosine/dot directly and L2 **negated**, so a larger value always means "closer". The HNSW code works in `distance = -kernel` (lower = nearer) so the textbook pseudocode applies verbatim; final output scores convert back to `kernel` (`score = -distance`) to match the exact-scan path the coordinator merges against.
- **Reference patterns (shipped V2 code to mirror):** `source/signature_store.{h,cpp}` (forgiving-load two-phase validation, writer), and in `atire/`: `set_approximate_config`/`load_signature_config`/`save_signature_config`/`build_signatures`/`vector_candidates_approx`/`search_vector_approx`/`search_hybrid_approx` and the flush `.vsig` block, the `append_segment` `.vsig` load, the compaction `.vsig` rewrite + input-free. V3 mirrors each with `hnsw` in place of `signature`/`vsig`.
- **HNSW config magic** is the 64-bit little-endian value of the bytes "ANTHNSW1" = `0x31575348544E41ULL`. (Bytes A,N,T,H,N,S,W,1 = 0x41,0x4E,0x54,0x48,0x4E,0x53,0x57,0x31; little-endian uint64 = 0x3157534E48544E41ULL. **Compute it in code** as shown in Task 3 rather than trusting this literal — see the memcmp-based approach.)
- **Fixed build seed:** `#define ANT_HNSW_SEED 0x1234567890ABCDEFULL` (level assignment RNG; makes builds deterministic).

---

## Task 1: `ANT_hnsw` — build + search (in-memory graph)

**Files:** Create `source/hnsw.h`, `source/hnsw.cpp`, `tests/test_hnsw.cpp`

The graph holds only topology (per-node level + per-layer neighbour docids). Vectors and the
distance kernel come from an `ANT_vector_store` passed into `build`/`search` — the graph never
owns or copies vectors. Build uses `std::vector` temporaries, then flattens into the plain-array
CSR (`levels`/`offsets`/`neighbours`) that both search and (Task 2) save operate on.

- [ ] **Step 1: Write the failing test** (`tests/test_hnsw.cpp`)

```cpp
/*
	TEST_HNSW.CPP -- unit tests for ANT_hnsw (build + search + recall).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/hnsw.h"
#include "../source/vector_store.h"
#include "../source/index_tombstones.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/*
	Build an in-memory ANT_vector_store by writing a .vec with the writer and loading it.
	dim floats per doc, all present.
*/
static ANT_vector_store *make_store(const char *path, long long dim, long long n, float *data)
{
ANT_vector_store_writer w;
long long i;
CHECK(w.create(path, dim) == 0);
for (i = 0; i < n; i++)
	CHECK(w.append(data + i * dim) == 0);
CHECK(w.finish() == 0);
return ANT_vector_store::load(path, dim, n);
}

static void brute_force_topk(const float *query, const float *data, long long dim, long long n, long metric, long long k, long long *out)
{
long long i, j;
double *score = new double[n];
for (i = 0; i < n; i++)
	score[i] = ANT_vector_store::kernel(query, data + i * dim, dim, metric);
for (j = 0; j < k; j++)
	{
	long long best = -1;
	for (i = 0; i < n; i++)
		if (score[i] > -1e300 && (best < 0 || score[i] > score[best]))
			best = i;
	out[j] = best;
	score[best] = -1e300;
	}
delete [] score;
}

static void test_recall_and_determinism(void)
{
long long dim = 24, n = 500, k = 10, i, d;
char path[64]; strcpy(path, "/tmp/ant_hnsw_XXXXXX"); int fd = mkstemp(path); if (fd >= 0) close(fd);
float *data = new float[n * dim];
srand(11);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 200 - 100);
ANT_vector_store *store = make_store(path, dim, n, data);

ANT_hnsw a, b;
CHECK(a.build(store, /*M=*/16, /*ef_construction=*/200, ANT_vector_store::VECTOR_METRIC_L2) == 0);
CHECK(b.build(store, 16, 200, ANT_vector_store::VECTOR_METRIC_L2) == 0);
CHECK(a.node_count() == n);

ANT_index_tombstones stones(n);
float query[24]; for (d = 0; d < dim; d++) query[d] = (float)(rand() % 200 - 100);

long long ha[10], hb[10]; double sa[10], sb[10];
long long ca = a.search(query, ANT_vector_store::VECTOR_METRIC_L2, /*ef_search=*/64, k, store, &stones, ha, sa);
long long cb = b.search(query, ANT_vector_store::VECTOR_METRIC_L2, 64, k, store, &stones, hb, sb);
CHECK(ca == k && cb == k);
/* determinism: two builds from the same data give identical results */
for (i = 0; i < k; i++) { CHECK(ha[i] == hb[i]); }

long long exact[10]; brute_force_topk(query, data, dim, n, ANT_vector_store::VECTOR_METRIC_L2, k, exact);
long long overlap = 0, j;
for (i = 0; i < k; i++) for (j = 0; j < k; j++) if (ha[i] == exact[j]) { overlap++; break; }
CHECK((double)overlap / (double)k >= 0.8);		/* single query, generous floor; e2e test averages */
/* scores must be descending kernel (higher = nearer) */
for (i = 1; i < k; i++) CHECK(sa[i] <= sa[i-1] + 1e-9);
delete store; delete [] data; unlink(path);
printf("test_recall_and_determinism OK (overlap=%lld/%lld)\n", overlap, k);
}

static void test_ef_monotonic(void)
{
long long dim = 16, n = 300, k = 10, i, d;
char path[64]; strcpy(path, "/tmp/ant_hnsw2_XXXXXX"); int fd = mkstemp(path); if (fd >= 0) close(fd);
float *data = new float[n * dim];
srand(3);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 100 - 50);
ANT_vector_store *store = make_store(path, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 16, 200, ANT_vector_store::VECTOR_METRIC_COSINE) == 0);
ANT_index_tombstones stones(n);
float q[16]; for (d = 0; d < dim; d++) q[d] = (float)(rand() % 100 - 50);
long long exact[10]; brute_force_topk(q, data, dim, n, ANT_vector_store::VECTOR_METRIC_COSINE, k, exact);
double recall_lo = 0, recall_hi = 0; long long h[10]; double s[10]; long long i2, j2;
g.search(q, ANT_vector_store::VECTOR_METRIC_COSINE, 16, k, store, &stones, h, s);
for (i2=0;i2<k;i2++) for(j2=0;j2<k;j2++) if(h[i2]==exact[j2]){recall_lo++;break;}
g.search(q, ANT_vector_store::VECTOR_METRIC_COSINE, 128, k, store, &stones, h, s);
for (i2=0;i2<k;i2++) for(j2=0;j2<k;j2++) if(h[i2]==exact[j2]){recall_hi++;break;}
CHECK(recall_hi >= recall_lo);		/* more ef never hurts recall */
delete store; delete [] data; unlink(path);
printf("test_ef_monotonic OK (ef16=%.0f ef128=%.0f)\n", recall_lo, recall_hi);
}

static void test_tombstone_filter(void)
{
long long dim = 8, n = 50, i, d;
char path[64]; strcpy(path, "/tmp/ant_hnsw3_XXXXXX"); int fd = mkstemp(path); if (fd >= 0) close(fd);
float *data = new float[n * dim];
for (i = 0; i < n; i++) for (d = 0; d < dim; d++) data[i*dim+d] = (float)((i + d) % 7);
ANT_vector_store *store = make_store(path, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 8, 64, ANT_vector_store::VECTOR_METRIC_L2) == 0);
ANT_index_tombstones stones(n);
stones.set_deleted(3); stones.set_deleted(7);
float q[8]; for (d = 0; d < dim; d++) q[d] = (float)(d % 7);
long long h[20]; double s[20];
long long c = g.search(q, ANT_vector_store::VECTOR_METRIC_L2, 32, 20, store, &stones, h, s);
for (i = 0; i < c; i++) { CHECK(h[i] != 3 && h[i] != 7); }		/* deleted docids never returned */
delete store; delete [] data; unlink(path);
printf("test_tombstone_filter OK\n");
}

int main(void)
{
test_recall_and_determinism();
test_ef_monotonic();
test_tombstone_filter();
printf("PASSED\n");
return 0;
}
```

Confirm `ANT_index_tombstones` has `set_deleted(long long)` and `is_deleted(long long)` and a
`(long long documents)` ctor (check `source/index_tombstones.h`); adjust the test if the names
differ. Confirm the metric enum lives on `ANT_vector_store` as `VECTOR_METRIC_L2` /
`VECTOR_METRIC_COSINE` / `VECTOR_METRIC_DOT` (check `source/vector_store.h`); if the enum is
elsewhere, include the right header and use the real names.

- [ ] **Step 2: Run to verify it fails** — `make tests 2>&1 | tail -3` → FAIL (`source/hnsw.h` missing).

- [ ] **Step 3: `source/hnsw.h`**

```cpp
/*
	HNSW.H -- hand-rolled hierarchical navigable small-world graph over a
	segment's dense vectors.  Topology only: per-node level + per-layer
	neighbour docids in a flat CSR layout.  Vectors + distance come from an
	ANT_vector_store passed to build()/search(); the graph never owns vectors.
	Works in distance = -kernel (lower = nearer) so textbook HNSW applies; see
	docs/superpowers/specs/2026-07-07-vector-v3-hnsw-design.md.
*/
#ifndef HNSW_H_
#define HNSW_H_

class ANT_vector_store;
class ANT_index_tombstones;

#define ANT_HNSW_SEED 0x1234567890ABCDEFULL

class ANT_hnsw
{
private:
	long long documents;			// node slots == vector store document_count
	long long M;
	long long M0;					// 2*M, the layer-0 degree cap
	long long ef_construction;
	long long entry_point;			// docid at the top of the hierarchy; -1 if empty
	long long max_level;
	int *levels;					// [documents]; per-node top level, -1 if not in graph
	long long *offsets;				// [documents+1]; prefix sums into neighbours[] (int units)
	int *neighbours;				// CSR stream: per node, per layer 0..level: [count][docid...]

	/* build helpers (defined in the .cpp) */
	double distance(long long a, const float *query, ANT_vector_store *vectors, long metric);

public:
	ANT_hnsw();
	~ANT_hnsw();

	long long node_count(void) { return documents; }
	long long get_M(void) { return M; }
	long long get_ef_construction(void) { return ef_construction; }
	long empty(void) { return entry_point < 0; }

	/* Build the graph over every present vector in `vectors` (docid order,
	   deterministic).  Returns 0 on success, nonzero on allocation failure. */
	long build(ANT_vector_store *vectors, long long M, long long ef_construction, long metric);

	/* Search for the top_k highest-kernel present, non-tombstoned docids for
	   `query`.  Fills out_docids/out_scores (scores = kernel, descending),
	   returns the count (<= top_k).  ef_search is clamped to >= top_k. */
	long long search(const float *query, long metric, long long ef_search, long long top_k,
		ANT_vector_store *vectors, ANT_index_tombstones *tombstones,
		long long *out_docids, double *out_scores);

	/* Task 2 will add: save()/load()/adopt of the CSR arrays. */
	friend class ANT_hnsw_serialiser;		// Task 2 hook; harmless now
} ;

#endif /* HNSW_H_ */
```

- [ ] **Step 4: `source/hnsw.cpp`** — build + search. Use `std::vector`/`std::priority_queue`
for build temporaries; flatten to CSR. Distance = `-kernel`.

```cpp
/*
	HNSW.CPP
*/
#include <math.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include <queue>
#include "hnsw.h"
#include "vector_store.h"
#include "index_tombstones.h"
#include "mersenne_twister.h"

ANT_hnsw::ANT_hnsw()
{
documents = 0; M = 0; M0 = 0; ef_construction = 0; entry_point = -1; max_level = -1;
levels = NULL; offsets = NULL; neighbours = NULL;
}

ANT_hnsw::~ANT_hnsw()
{
delete [] levels; delete [] offsets; delete [] neighbours;
}

double ANT_hnsw::distance(long long a, const float *query, ANT_vector_store *vectors, long metric)
{
return -ANT_vector_store::kernel(vectors->get(a), query, vectors->dimension, metric);
}

/*
	Pairs used in the heaps: (distance, docid).  std::priority_queue is a
	max-heap on the pair, so a "furthest-on-top" heap of found nodes is the
	default; a "nearest-on-top" candidate heap uses std::greater.
*/
typedef std::pair<double, long long> DN;

/*
	SEARCH-LAYER (Malkov & Yashunin Alg. 2), distance = -kernel.  Returns the
	found set (docid + distance) with |result| <= ef, exploring `layer`'s edges.
	`neighbours_of(node, layer, &count)` returns a pointer to node's neighbour
	docids at `layer` by walking its CSR slice.
*/
static const int *neighbours_of(const int *neighbours, const long long *offsets, const int *levels, long long node, long long layer, long long *out_count)
{
if (levels[node] < layer) { *out_count = 0; return NULL; }
const int *p = neighbours + offsets[node];
long long lc;
for (lc = 0; lc < layer; lc++)			/* skip lower layers: each is [count][count docids] */
	p += 1 + *p;
*out_count = *p;
return p + 1;
}

long ANT_hnsw::build(ANT_vector_store *vectors, long long M_in, long long ef_construction_in, long metric)
{
long long n = vectors->document_count(), i;
ANT_mersenne_twister twister;
double mL;

delete [] levels; delete [] offsets; delete [] neighbours;
levels = NULL; offsets = NULL; neighbours = NULL;

documents = n;
M = M_in < 1 ? 16 : M_in;
M0 = 2 * M;
ef_construction = ef_construction_in < 1 ? 200 : ef_construction_in;
entry_point = -1;
max_level = -1;
twister.init_genrand64(ANT_HNSW_SEED);
mL = 1.0 / log((double)M);

levels = new int[n > 0 ? n : 1];
for (i = 0; i < n; i++) levels[i] = -1;

/* mutable adjacency during build: adj[node][layer] = neighbour docids */
std::vector<std::vector<std::vector<long long> > > adj(n);

for (long long q = 0; q < n; q++)
	{
	if (!vectors->has(q))
		continue;						/* lexical-only doc: no vector, no node */

	double u = twister.genrand64_real2();		/* [0,1) */
	if (u <= 0.0) u = 1e-16;
	long long level = (long long)floor(-log(u) * mL);
	levels[q] = (int)level;
	adj[q].resize(level + 1);

	if (entry_point < 0)					/* first node */
		{ entry_point = q; max_level = level; continue; }

	long long ep = entry_point;
	long long L = max_level;

	/* greedy descent through the upper layers with ef=1 (read adj[] directly) */
	for (long long lc = L; lc > level; lc--)
		{
		long long changed = 1;
		while (changed)
			{
			changed = 0;
			double best_d = distance(ep, vectors->get(q), vectors, metric);
			if (lc < (long long)adj[ep].size())
				for (size_t e = 0; e < adj[ep][lc].size(); e++)
					{
					long long cand = adj[ep][lc][e];
					double d = distance(cand, vectors->get(q), vectors, metric);
					if (d < best_d) { best_d = d; ep = cand; changed = 1; }
					}
			}
		}

	/* insert at each layer from min(L, level) down to 0 */
	long long top = L < level ? L : level;
	for (long long lc = top; lc >= 0; lc--)
		{
		/* SEARCH-LAYER(q, {ep}, ef_construction, lc) over adj[] */
		std::priority_queue<DN> W;						/* max-heap: furthest on top */
		std::priority_queue<DN, std::vector<DN>, std::greater<DN> > C;	/* nearest on top */
		std::vector<char> visited(n, 0);
		double dep = distance(ep, vectors->get(q), vectors, metric);
		W.push(DN(dep, ep)); C.push(DN(dep, ep)); visited[ep] = 1;
		while (!C.empty())
			{
			DN c = C.top(); C.pop();
			if (c.first > W.top().first) break;
			long long cnode = c.second;
			if (lc < (long long)adj[cnode].size())
				for (size_t e = 0; e < adj[cnode][lc].size(); e++)
					{
					long long ecand = adj[cnode][lc][e];
					if (visited[ecand]) continue;
					visited[ecand] = 1;
					double de = distance(ecand, vectors->get(q), vectors, metric);
					if (de < W.top().first || (long long)W.size() < ef_construction)
						{ C.push(DN(de, ecand)); W.push(DN(de, ecand)); if ((long long)W.size() > ef_construction) W.pop(); }
					}
			}
		/* drain W into a nearest-first vector */
		std::vector<DN> cand;
		while (!W.empty()) { cand.push_back(W.top()); W.pop(); }
		/* cand is furthest-first; SELECT-NEIGHBORS-HEURISTIC wants nearest-first */
		std::sort(cand.begin(), cand.end());			/* ascending distance = nearest first */

		long long degree_cap = (lc == 0) ? M0 : M;
		/* Alg. 4 heuristic: keep e unless it is closer to an already-kept r than to q */
		std::vector<long long> selected;
		for (size_t ci = 0; ci < cand.size() && (long long)selected.size() < degree_cap; ci++)
			{
			long long e = cand[ci].second;
			double e_to_q = cand[ci].first;
			long good = 1;
			for (size_t si = 0; si < selected.size(); si++)
				if (distance(e, vectors->get(selected[si]), vectors, metric) < e_to_q)
					{ good = 0; break; }
			if (good) selected.push_back(e);
			}

		/* connect q <-> selected, prune each neighbour back to its cap */
		for (size_t si = 0; si < selected.size(); si++)
			{
			long long nbr = selected[si];
			adj[q][lc].push_back(nbr);
			adj[nbr][lc].push_back(q);
			if ((long long)adj[nbr][lc].size() > degree_cap)
				{
				/* prune nbr's neighbours by the same heuristic w.r.t. nbr */
				std::vector<DN> nn;
				for (size_t z = 0; z < adj[nbr][lc].size(); z++)
					nn.push_back(DN(distance(adj[nbr][lc][z], vectors->get(nbr), vectors, metric), adj[nbr][lc][z]));
				std::sort(nn.begin(), nn.end());
				std::vector<long long> kept;
				for (size_t z = 0; z < nn.size() && (long long)kept.size() < degree_cap; z++)
					{
					long long e = nn[z].second; double e_to_nbr = nn[z].first; long good = 1;
					for (size_t si2 = 0; si2 < kept.size(); si2++)
						if (distance(e, vectors->get(kept[si2]), vectors, metric) < e_to_nbr) { good = 0; break; }
					if (good) kept.push_back(e);
					}
				adj[nbr][lc] = kept;
				}
			}
		if (!cand.empty()) ep = cand[0].second;			/* nearest, for the next layer down */
		}

	if (level > max_level) { max_level = level; entry_point = q; }
	}

/* flatten adj[] to CSR */
offsets = new long long[n + 1];
offsets[0] = 0;
long long total = 0;
for (i = 0; i < n; i++)
	{
	long long slot = 0;
	if (levels[i] >= 0)
		for (long long lc = 0; lc <= levels[i]; lc++)
			slot += 1 + (long long)adj[i][lc].size();
	total += slot;
	offsets[i + 1] = total;
	}
neighbours = new int[total > 0 ? total : 1];
long long w = 0;
for (i = 0; i < n; i++)
	if (levels[i] >= 0)
		for (long long lc = 0; lc <= levels[i]; lc++)
			{
			neighbours[w++] = (int)adj[i][lc].size();
			for (size_t z = 0; z < adj[i][lc].size(); z++)
				neighbours[w++] = (int)adj[i][lc][z];
			}
return 0;
}

long long ANT_hnsw::search(const float *query, long metric, long long ef_search, long long top_k,
	ANT_vector_store *vectors, ANT_index_tombstones *tombstones,
	long long *out_docids, double *out_scores)
{
if (entry_point < 0 || documents == 0)
	return 0;
long long ef = ef_search < top_k ? top_k : ef_search;

long long ep = entry_point;
double dep = distance(ep, query, vectors, metric);

/* greedy descent from max_level down to layer 1 with ef=1 */
for (long long lc = max_level; lc >= 1; lc--)
	{
	long long changed = 1;
	while (changed)
		{
		changed = 0;
		long long count; const int *nb = neighbours_of(neighbours, offsets, levels, ep, lc, &count);
		for (long long e = 0; e < count; e++)
			{
			double d = distance(nb[e], query, vectors, metric);
			if (d < dep) { dep = d; ep = nb[e]; changed = 1; }
			}
		}
	}

/* ef search at layer 0 */
std::priority_queue<DN> W;
std::priority_queue<DN, std::vector<DN>, std::greater<DN> > C;
std::vector<char> visited(documents, 0);
W.push(DN(dep, ep)); C.push(DN(dep, ep)); visited[ep] = 1;
while (!C.empty())
	{
	DN c = C.top(); C.pop();
	if (c.first > W.top().first) break;
	long long count; const int *nb = neighbours_of(neighbours, offsets, levels, c.second, 0, &count);
	for (long long e = 0; e < count; e++)
		{
		long long ecand = nb[e];
		if (visited[ecand]) continue;
		visited[ecand] = 1;
		double de = distance(ecand, query, vectors, metric);
		if (de < W.top().first || (long long)W.size() < ef)
			{ C.push(DN(de, ecand)); W.push(DN(de, ecand)); if ((long long)W.size() > ef) W.pop(); }
		}
	}

/* W holds up to ef nearest (furthest on top); drain, filter tombstones, sort nearest-first, take top_k */
std::vector<DN> found;
while (!W.empty()) { found.push_back(W.top()); W.pop(); }
std::sort(found.begin(), found.end());		/* ascending distance = nearest first */
long long out = 0;
for (size_t i = 0; i < found.size() && out < top_k; i++)
	{
	long long docid = found[i].second;
	if (tombstones != NULL && tombstones->is_deleted(docid)) continue;
	out_docids[out] = docid;
	out_scores[out] = -found[i].first;		/* back to kernel: higher = nearer */
	out++;
	}
return out;
}
```

IMPORTANT implementation notes for the engineer:
- Add `#include <algorithm>` for `std::sort` (already listed in the includes above).
- Guard `adj[ep][lc]` accesses with `lc < (long long)adj[ep].size()` (a node's `adj` vector has
  only `level+1` layers; `ep` on an upper layer always has that layer, but be defensive).
- `distance()` calls `vectors->get(a)` and `vectors->dimension` — `dimension` is a public
  member (confirmed in `vector_store.h`). If it is private in your tree, add an accessor or use
  `vectors->document_count()`-adjacent API; verify.

- [ ] **Step 5: Run to verify it passes** — `make tests 2>&1 | tail -3 && ./bin/test_hnsw` → `PASSED`.

- [ ] **Step 6: Commit**

```bash
git add source/hnsw.h source/hnsw.cpp tests/test_hnsw.cpp
git commit -m "feat(vector-v3): ANT_hnsw hierarchical graph build + search"
```

---

## Task 2: `ANT_hnsw` — save + forgiving load (`seg_G.hnsw` CSR sidecar)

**Files:** Modify `source/hnsw.h`, `source/hnsw.cpp`, `tests/test_hnsw.cpp`

- [ ] **Step 1: Add failing tests** (append + call in `main()`)

```cpp
static void test_save_load_roundtrip(void)
{
long long dim = 16, n = 200, k = 8, i, d;
char vpath[64]; strcpy(vpath, "/tmp/ant_hnsw_v_XXXXXX"); { int fd=mkstemp(vpath); if(fd>=0) close(fd); }
char gpath[64]; strcpy(gpath, "/tmp/ant_hnsw_g_XXXXXX"); { int fd=mkstemp(gpath); if(fd>=0) close(fd); unlink(gpath); }
float *data = new float[n * dim]; srand(5);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 100 - 50);
ANT_vector_store *store = make_store(vpath, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 16, 200, ANT_vector_store::VECTOR_METRIC_L2) == 0);
CHECK(g.save(gpath) == 0);
ANT_hnsw *loaded = ANT_hnsw::load(gpath, 16, 200, n);
CHECK(loaded->node_count() == n);
CHECK(!loaded->empty());
ANT_index_tombstones stones(n);
float q[16]; for (d = 0; d < dim; d++) q[d] = (float)(rand() % 100 - 50);
long long h1[8], h2[8]; double s1[8], s2[8];
long long c1 = g.search(q, ANT_vector_store::VECTOR_METRIC_L2, 64, k, store, &stones, h1, s1);
long long c2 = loaded->search(q, ANT_vector_store::VECTOR_METRIC_L2, 64, k, store, &stones, h2, s2);
CHECK(c1 == c2);
for (i = 0; i < c1; i++) { CHECK(h1[i] == h2[i]); }		/* loaded graph searches identically */
delete loaded; delete store; delete [] data; unlink(vpath); unlink(gpath);
printf("test_save_load_roundtrip OK\n");
}

static void test_load_degrade(void)
{
/* missing file -> empty graph */
ANT_hnsw *missing = ANT_hnsw::load("/tmp/does_not_exist_hnsw", 16, 200, 10);
CHECK(missing->node_count() == 0 && missing->empty());
CHECK(missing->search(NULL, ANT_vector_store::VECTOR_METRIC_L2, 8, 4, NULL, NULL, NULL, NULL) == 0);
delete missing;
/* config mismatch -> empty graph */
long long dim = 8, n = 20, i;
char vpath[64]; strcpy(vpath, "/tmp/ant_hnsw_v2_XXXXXX"); { int fd=mkstemp(vpath); if(fd>=0) close(fd); }
char gpath[64]; strcpy(gpath, "/tmp/ant_hnsw_g2_XXXXXX"); { int fd=mkstemp(gpath); if(fd>=0) close(fd); unlink(gpath); }
float *data = new float[n * dim]; for (i = 0; i < n*dim; i++) data[i] = (float)(i % 5);
ANT_vector_store *store = make_store(vpath, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 8, 64, ANT_vector_store::VECTOR_METRIC_L2) == 0);
CHECK(g.save(gpath) == 0);
ANT_hnsw *wrong_M = ANT_hnsw::load(gpath, 16, 64, n);		/* M mismatch */
CHECK(wrong_M->node_count() == 0 && wrong_M->empty());
delete wrong_M;
ANT_hnsw *wrong_n = ANT_hnsw::load(gpath, 8, 64, n + 1);		/* doc count mismatch */
CHECK(wrong_n->node_count() == 0 && wrong_n->empty());
delete wrong_n;
delete store; delete [] data; unlink(vpath); unlink(gpath);
printf("test_load_degrade OK\n");
}
```

Add `test_save_load_roundtrip();` and `test_load_degrade();` to `main()`.

- [ ] **Step 2: Run to verify it fails** — FAIL (`save`/`load` not members).

- [ ] **Step 3: Declare in `source/hnsw.h`** (public section):

```cpp
	long save(const char *filename);						// write CSR sidecar; 0 on success
	static ANT_hnsw *load(const char *filename, long long expected_M, long long expected_ef_construction, long long expected_documents);
```

Remove the `friend class ANT_hnsw_serialiser;` line (it was a placeholder; not needed).

- [ ] **Step 4: Implement in `source/hnsw.cpp`** (`#include <stdio.h>` at top). On-disk layout:
header (magic u64, version u32, M i64, ef_construction i64, documents i64, entry_point i64,
max_level i64) then `levels[documents]` (i32), `offsets[documents+1]` (i64),
`neighbours[offsets[documents]]` (i32). Header is 8+4+8+8+8+8+8 = **52 bytes**.

```cpp
#define ANT_HNSW_VERSION 1u

static unsigned long long ant_hnsw_magic(void)
{
unsigned long long m; const char *s = "ANTHNSW1"; memcpy(&m, s, 8); return m;		/* endian-correct */
}

long ANT_hnsw::save(const char *filename)
{
char temp[4200]; FILE *fp;
unsigned long long magic = ant_hnsw_magic();
unsigned int version = ANT_HNSW_VERSION;
long long neighbour_count = offsets != NULL ? offsets[documents] : 0;
if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp)) return 1;
if ((fp = fopen(temp, "wb")) == NULL) return 1;
if (fwrite(&magic,sizeof(magic),1,fp)!=1 || fwrite(&version,sizeof(version),1,fp)!=1
	|| fwrite(&M,sizeof(M),1,fp)!=1 || fwrite(&ef_construction,sizeof(ef_construction),1,fp)!=1
	|| fwrite(&documents,sizeof(documents),1,fp)!=1 || fwrite(&entry_point,sizeof(entry_point),1,fp)!=1
	|| fwrite(&max_level,sizeof(max_level),1,fp)!=1
	|| fwrite(levels,sizeof(int),(size_t)documents,fp)!=(size_t)documents
	|| fwrite(offsets,sizeof(long long),(size_t)(documents+1),fp)!=(size_t)(documents+1)
	|| fwrite(neighbours,sizeof(int),(size_t)neighbour_count,fp)!=(size_t)neighbour_count)
	{ fclose(fp); remove(temp); return 1; }
fclose(fp);
if (rename(temp, filename) != 0) { remove(temp); return 1; }
return 0;
}

ANT_hnsw *ANT_hnsw::load(const char *filename, long long expected_M, long long expected_ef_construction, long long expected_documents)
{
ANT_hnsw *g = new ANT_hnsw();		/* empty by default (degraded) */
FILE *fp; unsigned long long magic; unsigned int version;
long long m, efc, docs, ep, maxl, i;
if ((fp = fopen(filename, "rb")) == NULL) return g;

/* Phase 1: fixed header + range checks (no big allocation yet) */
if (fread(&magic,sizeof(magic),1,fp)!=1 || magic != ant_hnsw_magic()
	|| fread(&version,sizeof(version),1,fp)!=1 || version != ANT_HNSW_VERSION
	|| fread(&m,sizeof(m),1,fp)!=1 || fread(&efc,sizeof(efc),1,fp)!=1
	|| fread(&docs,sizeof(docs),1,fp)!=1 || fread(&ep,sizeof(ep),1,fp)!=1
	|| fread(&maxl,sizeof(maxl),1,fp)!=1
	|| m != expected_M || efc != expected_ef_construction || docs != expected_documents
	|| docs < 0 || docs > (1LL<<40) || ep < -1 || ep >= docs || maxl < -1 || maxl > 4096)
	{ fclose(fp); return g; }

/* read levels[] and offsets[] (bounded by docs, already range-checked) */
int *lv = new int[docs > 0 ? docs : 1];
long long *off = new long long[docs + 1];
if (fread(lv,sizeof(int),(size_t)docs,fp)!=(size_t)docs
	|| fread(off,sizeof(long long),(size_t)(docs+1),fp)!=(size_t)(docs+1))
	{ delete [] lv; delete [] off; fclose(fp); return g; }

/* Phase 2: validate offsets monotonic + exact file size before the big alloc */
long long ncount = off[docs];
long good = (off[0] == 0 && ncount >= 0 && ncount <= (docs + 1) * (2*16 + 1) * 64);	/* loose sane cap */
for (i = 0; good && i < docs; i++)
	{
	if (off[i+1] < off[i]) good = 0;
	if (lv[i] < -1 || lv[i] > maxl) good = 0;
	}
long long header = 52, expected_size = header + 4*docs + 8*(docs+1) + 4*ncount;
if (good)
	{
	long long cur = ftell(fp), end;
	if (fseek(fp, 0, SEEK_END) != 0 || (end = ftell(fp)) != expected_size) good = 0;
	else fseek(fp, cur, SEEK_SET);
	}
if (!good) { delete [] lv; delete [] off; fclose(fp); return g; }

int *nb = new int[ncount > 0 ? ncount : 1];
if (fread(nb,sizeof(int),(size_t)ncount,fp)!=(size_t)ncount)
	{ delete [] lv; delete [] off; delete [] nb; fclose(fp); return g; }
fclose(fp);

g->documents = docs; g->M = m; g->M0 = 2*m; g->ef_construction = efc;
g->entry_point = ep; g->max_level = maxl;
g->levels = lv; g->offsets = off; g->neighbours = nb;
return g;
}
```

Note the loose neighbour cap is defensive only; the exact-file-size check is the real guard.
Confirm `ANT_hnsw::search` already early-returns 0 when `entry_point < 0` (it does), so a
degraded empty graph is safe to `search` (the `test_load_degrade` NULL-args call relies on that
early return happening BEFORE any pointer use).

- [ ] **Step 5: Run to verify it passes** — `make tests 2>&1 | tail -3 && ./bin/test_hnsw` → `PASSED`.

- [ ] **Step 6: Commit**

```bash
git add source/hnsw.h source/hnsw.cpp tests/test_hnsw.cpp
git commit -m "feat(vector-v3): ANT_hnsw save + forgiving two-phase load (seg_G.hnsw)"
```

---

## Task 3: `hnsw.config` + `set_hnsw_config` / `set_ef_search` + open-load

**Files:** Modify `atire/atire_segment_index.h`, `atire/atire_segment_index.cpp`, `atire/atire_segment_index_vector.cpp`; `tests/test_segment_index.cpp`

This mirrors V2 Task 3 (`signature.config` / `set_approximate_config`). Read the shipped V2
methods `load_signature_config` / `save_signature_config` / `set_approximate_config` in
`atire/atire_segment_index_vector.cpp` and copy their shape.

- [ ] **Step 1: Failing test** (append + call in `main()`)

```cpp
static void test_hnsw_config_persists(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(a->open(dir) == 0);
CHECK(a->set_hnsw_config(0, 0) == 0);			/* 0,0 => defaults M=16, ef_construction=200 */
CHECK(a->hnsw_configured() == 1);
delete a;
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(dir) == 0);
CHECK(b->hnsw_configured() == 1);				/* config reloads */
delete b;
delete [] dir;
printf("test_hnsw_config_persists OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — `rm -f obj/*.o && make tests 2>&1 | tail -5` → FAIL.

- [ ] **Step 3: `atire/atire_segment_index.h`** — add forward decl near the other `class`
decls: `class ANT_hnsw;`. Add private members near `signature_bits_current`:

```cpp
	long long hnsw_M_current;				// 0 = HNSW not configured
	long long hnsw_ef_construction_current;
	long long hnsw_ef_search;				// query knob; default 64
```

Add private method decls near `load_signature_config`:

```cpp
	long load_hnsw_config(void);
	long save_hnsw_config(void);
	long long vector_candidates_hnsw(const float *query, long long top_k, ANT_vector_candidate *best);	// Task 7
```

Add public API near `set_approximate_config`:

```cpp
	long set_hnsw_config(long long M, long long ef_construction);	// M<=0 => 16, ef_construction<=0 => 200; persists hnsw.config on first enable; 0 on success
	void set_ef_search(long long ef);								// clamps >= 1; default 64
	long hnsw_configured(void) { return hnsw_M_current != 0; }
	long build_hnsw(void);											// Task 5 backfill
	long long search_vector_hnsw(const float *query, long long top_k);						// Task 7
	long long search_hybrid_hnsw(char *query_text, const float *query_vector, long long top_k);	// Task 8
```

Add `#include "../source/hnsw.h"` at the top of `atire/atire_segment_index.cpp` and
`atire/atire_segment_index_vector.cpp`.

- [ ] **Step 4: ctor + open in `atire/atire_segment_index.cpp`** — in the ctor body (by the
signature-member inits): `hnsw_M_current = 0; hnsw_ef_construction_current = 0; hnsw_ef_search = 64;`
Immediately after the existing `load_signature_config(); rebuild_query_signer();` in `open()`,
add: `load_hnsw_config();`

- [ ] **Step 5: config load/save + setters in `atire/atire_segment_index_vector.cpp`** —
mirror `load_signature_config`/`save_signature_config`/`set_approximate_config` exactly, with
`hnsw.config` and two int64 fields (M, ef_construction). Magic computed via memcmp of "ANTHNSW1"
(same helper style as `ant_hnsw_magic` — or inline the 8-byte compare). Skeleton:

```cpp
long ATIRE_segment_index::load_hnsw_config(void)
{
char filename[4096]; FILE *fp;
unsigned long long magic, want; unsigned int version; long long M, efc;
const char *tag = "ANTHNSW1"; memcpy(&want, tag, 8);
snprintf(filename, sizeof(filename), "%s/hnsw.config", directory);
if ((fp = fopen(filename, "rb")) == NULL) return 0;
if (fread(&magic,sizeof(magic),1,fp)!=1 || magic != want
	|| fread(&version,sizeof(version),1,fp)!=1 || version != 1u
	|| fread(&M,sizeof(M),1,fp)!=1 || fread(&efc,sizeof(efc),1,fp)!=1
	|| M < 1 || M > 4096 || efc < 1 || efc > 100000)
	{ fclose(fp); return 0; }
fclose(fp);
hnsw_M_current = M; hnsw_ef_construction_current = efc;
return 0;
}

long ATIRE_segment_index::save_hnsw_config(void)
{
char filename[4096], temp[4200]; FILE *fp;
unsigned long long magic; unsigned int version = 1u;
long long M = hnsw_M_current, efc = hnsw_ef_construction_current;
const char *tag = "ANTHNSW1"; memcpy(&magic, tag, 8);
snprintf(filename, sizeof(filename), "%s/hnsw.config", directory);
if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp)) return 1;
if ((fp = fopen(temp, "wb")) == NULL) return 1;
if (fwrite(&magic,sizeof(magic),1,fp)!=1 || fwrite(&version,sizeof(version),1,fp)!=1
	|| fwrite(&M,sizeof(M),1,fp)!=1 || fwrite(&efc,sizeof(efc),1,fp)!=1)
	{ fclose(fp); remove(temp); return 1; }
fclose(fp);
if (rename(temp, filename) != 0) { remove(temp); return 1; }
return 0;
}

long ATIRE_segment_index::set_hnsw_config(long long M, long long ef_construction)
{
if (directory == NULL) return 1;					/* must be open */
if (vector_dimension_current == 0) return 1;		/* HNSW requires vectors enabled */
if (hnsw_M_current != 0) return 0;					/* already configured; immutable */
if (M <= 0) M = 16;
if (ef_construction <= 0) ef_construction = 200;
if (M > 4096 || ef_construction > 100000) return 1;
hnsw_M_current = M; hnsw_ef_construction_current = ef_construction;
if (save_hnsw_config() != 0) { hnsw_M_current = 0; hnsw_ef_construction_current = 0; return 1; }
return 0;
}

void ATIRE_segment_index::set_ef_search(long long ef)
{
hnsw_ef_search = ef < 1 ? 1 : ef;
}
```

- [ ] **Step 6: Run to verify it passes** — `rm -f obj/*.o && make all && make tests 2>&1 | tail -3 && ./bin/test_segment_index 2>&1 | grep -E "hnsw_config_persists|PASSED"` → OK.

- [ ] **Step 7: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v3): hnsw.config + set_hnsw_config/set_ef_search + open-load"
```

---

## Task 4: flush builds `seg_G.hnsw`

**Files:** Modify `atire/atire_segment_index.cpp`, `tests/test_segment_index.cpp`

Mirror the shipped V2 flush `.vsig` block (search `atire/atire_segment_index.cpp` for the
`signature_bits_current != 0` block inside `flush()`), replacing signature-signing with an
HNSW build over the just-written `.vec`.

- [ ] **Step 1: Failing test** (append + call in `main()`) — note zero-padded filename:

```cpp
static void test_flush_builds_hnsw(void)
{
char *dir = make_index_dir(); char hnsw[4096]; float v[8] = {1,0,0,0,0,0,0,0};
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_hnsw_config(16, 200) == 0);
CHECK(idx->add_document("d1", "<DOC>alpha</DOC>", v) >= 0);
CHECK(idx->flush() == 0);
long long g = idx->disk_segment_generation(0);
snprintf(hnsw, sizeof(hnsw), "%s/seg_%06lld.hnsw", dir, g);
FILE *fp = fopen(hnsw, "rb"); CHECK(fp != NULL); fclose(fp);
delete idx; delete [] dir;
printf("test_flush_builds_hnsw OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL (no `.hnsw` written).

- [ ] **Step 3: Implement in `flush()`** — after the `.vec` writer's successful `finish()` (the
same place V2 writes `.vsig`), add a best-effort HNSW build. It reloads the just-written `.vec`
(simplest, matches the compaction approach) and builds a graph:

```cpp
	/*
		V3: build the HNSW graph sidecar alongside .vec.  Non-fatal to the
		flush -- a failure leaves the segment graph-less (exact-scanned) until
		build_hnsw()/compaction.
	*/
	if (hnsw_M_current != 0)
		{
		char hnsw_name[4096], vec_reload[4096];
		segment_filename(hnsw_name, sizeof(hnsw_name), writer_generation, "hnsw");
		segment_filename(vec_reload, sizeof(vec_reload), writer_generation, "vec");
		ANT_vector_store *v = ANT_vector_store::load(vec_reload, vector_dimension_current, writer_documents);
		if (v->document_count() == writer_documents && writer_documents > 0)
			{
			ANT_hnsw graph;
			if (graph.build(v, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0)
				graph.save(hnsw_name);
			}
		delete v;
		}
```

Use the exact local names the surrounding flush uses for the writer generation and document
count (the V2 block used `writer_generation` and `writer_documents`; confirm and match).

- [ ] **Step 4: Run to verify it passes** — `make all && ./bin/test_segment_index 2>&1 | grep -E "flush_builds_hnsw|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v3): flush builds the seg_G.hnsw graph sidecar"
```

---

## Task 5: `build_hnsw()` backfill

**Files:** Modify `atire/atire_segment_index_vector.cpp`, `tests/test_segment_index.cpp`

Mirror the shipped V2 `build_signatures()` exactly, swapping the signing loop for an HNSW build.

- [ ] **Step 1: Failing test** (append + call in `main()`)

```cpp
static void test_build_hnsw_backfill(void)
{
char *dir = make_index_dir(); char hnsw[4096]; float v[8] = {0,1,0,0,0,0,0,0};
ATIRE_segment_index *a = new ATIRE_segment_index();		/* segment created BEFORE HNSW enabled */
CHECK(a->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(a->open(dir) == 0);
CHECK(a->add_document("d1", "<DOC>beta</DOC>", v) >= 0);
CHECK(a->flush() == 0);
long long g = a->disk_segment_generation(0);
delete a;
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(dir) == 0);
CHECK(b->set_hnsw_config(16, 200) == 0);
snprintf(hnsw, sizeof(hnsw), "%s/seg_%06lld.hnsw", dir, g);
CHECK(fopen(hnsw, "rb") == NULL);			/* not there yet */
CHECK(b->build_hnsw() == 0);
FILE *fp = fopen(hnsw, "rb"); CHECK(fp != NULL); fclose(fp);
delete b; delete [] dir;
printf("test_build_hnsw_backfill OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL.

- [ ] **Step 3: Implement `build_hnsw()`** in `atire/atire_segment_index_vector.cpp` (mirror
`build_signatures`):

```cpp
long ATIRE_segment_index::build_hnsw(void)
{
long long which;
char vec_name[4096], hnsw_name[4096];
if (hnsw_M_current == 0) return 1;

for (which = 0; which < segment_count; which++)
	{
	long long generation = segments[which].generation;
	long long docs = segments[which].engine->get_document_count();

	segment_filename(hnsw_name, sizeof(hnsw_name), generation, "hnsw");
	ANT_hnsw *existing = ANT_hnsw::load(hnsw_name, hnsw_M_current, hnsw_ef_construction_current, docs);
	long long already = existing->node_count() == docs && docs > 0 && !existing->empty();
	delete existing;
	if (already) continue;

	segment_filename(vec_name, sizeof(vec_name), generation, "vec");
	ANT_vector_store *vectors = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
	if (vectors->document_count() == docs && docs > 0)
		{
		ANT_hnsw graph;
		if (graph.build(vectors, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0)
			graph.save(hnsw_name);
		}
	delete vectors;
	}
return 0;
}
```

- [ ] **Step 4: Run to verify it passes** — `make all && ./bin/test_segment_index 2>&1 | grep -E "build_hnsw_backfill|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v3): build_hnsw() backfill for pre-existing segments"
```

---

## Task 6: cache `segment.hnsw_graph` — load on segment open/append

**Files:** Modify `atire/atire_segment_index.h` (segment struct), `atire/atire_segment_index.cpp`; `tests/test_segment_index.cpp`

Mirror the shipped V2 Task 6 (`segment.signatures`) at ALL the same sites. This is the
prerequisite for Task 7.

- [ ] **Step 1: Failing test** (append + call in `main()`)

```cpp
static void test_segment_hnsw_loaded(void)
{
char *dir = make_index_dir(); float v[8] = {1,1,0,0,0,0,0,0};
ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->set_vector_config(8, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(a->open(dir) == 0);
CHECK(a->set_hnsw_config(16, 200) == 0);
CHECK(a->add_document("d1", "<DOC>alpha</DOC>", v) >= 0);
CHECK(a->flush() == 0);
delete a;
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(dir) == 0);
CHECK(b->disk_segment_count() == 1);
CHECK(b->disk_segment_has_hnsw(0) == 1);		/* new test hook */
delete b; delete [] dir;
printf("test_segment_hnsw_loaded OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL (`disk_segment_has_hnsw` not a member).

- [ ] **Step 3: field + load + free + hook.** In the `segment` struct (next to
`ANT_signature_store *signatures;`): `ANT_hnsw *hnsw_graph;` Add public test hook near the
other `disk_segment_*` hooks (DEFINE it in the .cpp to keep the header include-free — mirror
`disk_segment_has_signatures`):

Header decl: `long disk_segment_has_hnsw(long long which);`
`.cpp` definition (after `disk_segment_has_signatures`):
```cpp
long ATIRE_segment_index::disk_segment_has_hnsw(long long which)
{
return segments[which].hnsw_graph != NULL && !segments[which].hnsw_graph->empty();
}
```

In `append_segment` (where `.vsig` is loaded into `segments[segment_count].signatures`), load
`.hnsw` the same way, right after:
```cpp
	if (vector_dimension_current != 0 && hnsw_M_current != 0)
		{
		char hnsw_name[4096];
		segment_filename(hnsw_name, sizeof(hnsw_name), segments[segment_count].generation, "hnsw");
		segments[segment_count].hnsw_graph = ANT_hnsw::load(hnsw_name, hnsw_M_current, hnsw_ef_construction_current, segments[segment_count].engine->get_document_count());
		}
	else
		segments[segment_count].hnsw_graph = NULL;
```
(Match the exact index/generation expression the surrounding `.vsig` load uses.)

Free it in the dtor loop (alongside `delete segments[which].signatures;`):
`delete segments[which].hnsw_graph;`

Add `.hnsw` to `delete_segment_files` (alongside `.vsig`). The orphan sweep already matches
`seg_*` generically — no change.

- [ ] **Step 4: Run to verify it passes** — `rm -f obj/*.o && make all && ./bin/test_segment_index 2>&1 | grep -E "segment_hnsw_loaded|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index.h atire/atire_segment_index.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v3): cache segment.hnsw_graph (load on open/append) + .hnsw cleanup"
```

---

## Task 7: `search_vector_hnsw` — graph search + fallback

**Files:** Modify `atire/atire_segment_index_vector.cpp`, `atire/atire_segment_index.h` (already declared in Task 3), `tests/test_segment_index.cpp`

Read the shipped V2 `vector_candidates_approx` and `search_vector_approx` first — the structure
is identical; only the per-segment gatherer differs (graph search instead of Hamming shortlist),
and there is **no separate rerank** (graph candidates are already exactly scored).

- [ ] **Step 1: Failing tests** (append + call BOTH in `main()`)

```cpp
static void test_hnsw_recall(void)
{
char *dir = make_index_dir();
long long dim = 32, n = 400, k = 10, i, d;
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_hnsw_config(16, 200) == 0);
idx->set_ef_search(64);
srand(9);
float *vecs = new float[n * dim]; char key[32], doc[64];
for (i = 0; i < n; i++)
	{
	for (d = 0; d < dim; d++) vecs[i*dim+d] = (float)(rand() % 200 - 100);
	snprintf(key, sizeof(key), "k%lld", i); snprintf(doc, sizeof(doc), "<DOC>term%lld</DOC>", i);
	CHECK(idx->add_document(key, doc, vecs + i*dim) >= 0);
	}
CHECK(idx->flush() == 0);
/* average recall over 25 queries (V2 lesson: never a single-query coin-flip) */
double total = 0; long long nq = 25, qi;
for (qi = 0; qi < nq; qi++)
	{
	float query[32]; for (d = 0; d < dim; d++) query[d] = (float)(rand() % 200 - 100);
	long long eh = idx->search_vector(query, k); char ek[10][256];
	for (i = 0; i < eh; i++) strcpy(ek[i], idx->get_hit(i)->filename);
	long long ah = idx->search_vector_hnsw(query, k);
	long long overlap = 0, j;
	for (i = 0; i < ah; i++) for (j = 0; j < eh; j++) if (strcmp(idx->get_hit(i)->filename, ek[j]) == 0) { overlap++; break; }
	total += (double)overlap / (double)k;
	}
double mean = total / (double)nq;
CHECK(mean >= 0.85);		/* margin-safe; HNSW typically >> this at ef=64 */
delete [] vecs; delete idx; delete [] dir;
printf("test_hnsw_recall OK (mean_recall=%.3f over %lld q)\n", mean, nq);
}

static void test_hnsw_dot_fallback(void)
{
char *dir = make_index_dir();
long long dim = 8, i, d; float v[8];
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_hnsw_config(16, 200) == 0);
for (i = 0; i < 20; i++) { for (d=0;d<dim;d++) v[d]=(float)((i*7+d)%11); char key[16]; snprintf(key,sizeof(key),"k%lld",i); CHECK(idx->add_document(key,"<DOC>x</DOC>",v)>=0); }
CHECK(idx->flush() == 0);
float q[8]; for (d=0;d<dim;d++) q[d]=(float)(d%5);
long long he = idx->search_vector(q, 5); char ek[5][256];
for (i=0;i<he;i++) strcpy(ek[i], idx->get_hit(i)->filename);
long long ha = idx->search_vector_hnsw(q, 5);
CHECK(ha == he);
for (i=0;i<ha;i++) CHECK(strcmp(ek[i], idx->get_hit(i)->filename) == 0);	/* dot => byte-identical exact fallback */
delete idx; delete [] dir;
printf("test_hnsw_dot_fallback OK\n");
}
```

Also add an **L2 recall** test (`test_hnsw_l2_recall`) identical to `test_hnsw_recall` but with
`VECTOR_METRIC_L2` — this exercises V3's new L2 approximate path. Same `mean >= 0.85` assertion,
averaged over 25 queries.

- [ ] **Step 2: Run to verify it fails** — FAIL.

- [ ] **Step 3: Implement `vector_candidates_hnsw`** in `atire/atire_segment_index_vector.cpp`
(mirror `vector_candidates_approx`; per-segment gatherer uses the graph):

```cpp
long long ATIRE_segment_index::vector_candidates_hnsw(const float *query, long long top_k, ANT_vector_candidate *best)
{
long long which, docid, best_count = 0;
long long ef = hnsw_ef_search < top_k ? top_k : hnsw_ef_search;
float *normalized = NULL;
long long *cand_docids = new long long[ef > 0 ? ef : 1];
double *cand_scores = new double[ef > 0 ? ef : 1];

if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{ delete [] normalized; delete [] cand_docids; delete [] cand_scores; return 0; }
	query = normalized;
	}

for (which = 0; which < segment_count; which++)
	{
	if (segments[which].vectors == NULL)
		continue;
	if (segments[which].hnsw_graph != NULL && !segments[which].hnsw_graph->empty()
		&& segments[which].hnsw_graph->node_count() == segments[which].engine->get_document_count())
		{
		long long c = segments[which].hnsw_graph->search(query, vector_metric, ef, ef,
			segments[which].vectors, segments[which].tombstones, cand_docids, cand_scores);
		for (long long p = 0; p < c; p++)
			ANT_vector_candidate_insert(best, &best_count, top_k, cand_scores[p], segments[which].generation, cand_docids[p]);
		}
	else
		segments[which].vectors->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k);
	}

for (docid = 0; docid < writer_documents; docid++)		/* live buffer: exact */
	{
	if (writer_vector_presence == NULL || !(writer_vector_presence[docid / 8] & (1 << (docid % 8)))) continue;
	if (writer_tombstones->is_deleted(docid)) continue;
	ANT_vector_candidate_insert(best, &best_count, top_k,
		ANT_vector_store::kernel(query, writer_vector_data + docid * vector_dimension_current, vector_dimension_current, vector_metric),
		writer_generation, docid);
	}

delete [] normalized; delete [] cand_docids; delete [] cand_scores;
return best_count;
}
```
CRITICAL: copy the live-buffer loop VERBATIM from the real `vector_candidates_approx` (field
names must match the tree). The graph `search` is asked for `ef` candidates (not `top_k`) so the
global top-k sees enough per segment; scores from `search` are `kernel` values (higher=better),
exactly what `ANT_vector_candidate_insert` expects.

- [ ] **Step 4: Implement `search_vector_hnsw`** — copy `search_vector`'s body VERBATIM,
changing only (a) the guard and (b) the one `vector_candidates(` → `vector_candidates_hnsw(`:

```cpp
long long ATIRE_segment_index::search_vector_hnsw(const float *query, long long top_k)
{
if (hnsw_M_current == 0 || vector_metric == VECTOR_METRIC_DOT)
	return search_vector(query, top_k);			/* transparent fallback (dot / unconfigured) */
/* ...verbatim copy of search_vector()'s body, with vector_candidates(...) -> vector_candidates_hnsw(...) ... */
}
```

- [ ] **Step 5: Run to verify it passes** — `make all && ./bin/test_segment_index 2>&1 | grep -E "hnsw_recall|hnsw_l2_recall|hnsw_dot_fallback|PASSED"` → all OK. Run the full binary 3x to confirm stability.

- [ ] **Step 6: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v3): search_vector_hnsw graph search + dot/unconfigured fallback"
```

---

## Task 8: `search_hybrid_hnsw`

**Files:** Modify `atire/atire_segment_index.h` (declared in Task 3), `atire/atire_segment_index_vector.cpp`, `tests/test_segment_index.cpp`

Copy `search_hybrid`'s body verbatim with the guard + the one `vector_candidates(` →
`vector_candidates_hnsw(` swap (exactly as V2 Task 8 did for `search_hybrid_approx`).

- [ ] **Step 1: Failing test** (append + call in `main()`)

```cpp
static void test_hnsw_hybrid_smoke(void)
{
char *dir = make_index_dir();
long long dim = 16, i, d; float v[16];
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_hnsw_config(16, 200) == 0);
for (i = 0; i < 50; i++) { for (d=0;d<dim;d++) v[d]=(float)((i+d)%9-4); char key[16]; snprintf(key,sizeof(key),"k%lld",i); CHECK(idx->add_document(key,"<DOC>alpha beta</DOC>",v)>=0); }
CHECK(idx->flush() == 0);
char query[32]; strcpy(query, "alpha");
float qv[16]; for (d=0;d<dim;d++) qv[d]=(float)(d%5-2);
long long hits = idx->search_hybrid_hnsw(query, qv, 5);
CHECK(hits > 0 && hits <= 5);
delete idx; delete [] dir;
printf("test_hnsw_hybrid_smoke OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL.

- [ ] **Step 3: Implement `search_hybrid_hnsw`**:

```cpp
long long ATIRE_segment_index::search_hybrid_hnsw(char *query_text, const float *query_vector, long long top_k)
{
if (hnsw_M_current == 0 || vector_metric == VECTOR_METRIC_DOT)
	return search_hybrid(query_text, query_vector, top_k);	/* transparent fallback */
/* ...verbatim copy of search_hybrid()'s body, with vector_candidates(...) -> vector_candidates_hnsw(...) ... */
}
```
Do NOT alter the RRF (k=60) math or the lexical leg — only the vector-candidate source changes.
Copy ALL locals from `search_hybrid` (fused, filename_buffer, counters, etc.).

- [ ] **Step 4: Run to verify it passes** — `make all && ./bin/test_segment_index 2>&1 | grep -E "hnsw_hybrid_smoke|PASSED"` → OK.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_vector.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v3): search_hybrid_hnsw (RRF over HNSW vector leg)"
```

---

## Task 9: compaction rebuilds `.hnsw`

**Files:** Modify `atire/atire_segment_index_compaction.cpp`, `tests/test_segment_index.cpp`

Mirror the shipped V2 Task 9 (`.vsig` rewrite + input-free). Two parts: (A) rebuild the merged
`.hnsw` and refresh the in-memory cache BEFORE the step-6 `segments[]` shuffle; (B) free each
dropped input's `hnsw_graph` at the input-drop site.

- [ ] **Step 1: Failing test** (append + call in `main()`) — zero-padded filename:

```cpp
static void test_compaction_rebuilds_hnsw(void)
{
char *dir = make_index_dir(); char hnsw[4096];
long long dim = 16, i, d; float v[16];
ATIRE_segment_index *idx = new ATIRE_segment_index();
CHECK(idx->set_vector_config(dim, ATIRE_segment_index::VECTOR_METRIC_COSINE) == 0);
CHECK(idx->open(dir) == 0);
CHECK(idx->set_hnsw_config(16, 200) == 0);
for (i=0;i<10;i++){for(d=0;d<dim;d++)v[d]=(float)((i+d)%7);char k[16];snprintf(k,sizeof(k),"a%lld",i);CHECK(idx->add_document(k,"<DOC>x</DOC>",v)>=0);}
CHECK(idx->flush() == 0);
for (i=0;i<10;i++){for(d=0;d<dim;d++)v[d]=(float)((i*3+d)%7);char k[16];snprintf(k,sizeof(k),"b%lld",i);CHECK(idx->add_document(k,"<DOC>x</DOC>",v)>=0);}
CHECK(idx->flush() == 0);
long long gens[2] = { idx->disk_segment_generation(0), idx->disk_segment_generation(1) };
CHECK(idx->compact(gens, 2) == 0);
long long out_gen = idx->disk_segment_generation(0);
snprintf(hnsw, sizeof(hnsw), "%s/seg_%06lld.hnsw", dir, out_gen);
FILE *fp = fopen(hnsw, "rb"); CHECK(fp != NULL); fclose(fp);
float q[16]; for (d=0;d<dim;d++) q[d]=(float)(d%5);
CHECK(idx->search_vector_hnsw(q, 5) == 5);		/* approx still returns k after compaction */
delete idx; delete [] dir;
printf("test_compaction_rebuilds_hnsw OK\n");
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL.

- [ ] **Step 3: Implement.** Add `#include "../source/hnsw.h"` at the top of
`atire/atire_segment_index_compaction.cpp`. Immediately after the V2 `.vsig` rewrite block
(which is itself after the keymap-repoint loop and before the step-5 manifest swap / step-6
shuffle — the `output_segment` pointer is still valid there), add a best-effort `.hnsw` rebuild:

```cpp
	/*
		V3: rebuild the merged segment's HNSW graph over the merged DENSE
		vectors (reload the fresh output .vec).  Best-effort: failure leaves the
		output graph-less (exact-scanned), never aborts a successful merge.
	*/
	if (hnsw_M_current != 0)
		{
		char out_vec[4096], out_hnsw[4096];
		segment_filename(out_vec, sizeof(out_vec), output_generation, "vec");
		segment_filename(out_hnsw, sizeof(out_hnsw), output_generation, "hnsw");
		long long out_docs = output_segment->engine->get_document_count();
		ANT_vector_store *out_vectors = ANT_vector_store::load(out_vec, vector_dimension_current, out_docs);
		if (out_vectors->document_count() == out_docs && out_docs > 0)
			{
			ANT_hnsw graph;
			if (graph.build(out_vectors, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0)
				graph.save(out_hnsw);
			}
		delete out_vectors;
		delete output_segment->hnsw_graph;
		output_segment->hnsw_graph = ANT_hnsw::load(out_hnsw, hnsw_M_current, hnsw_ef_construction_current, output_segment->engine->get_document_count());
		}
```

Part B — in the step-6 input-drop loop, alongside `delete segments[which].signatures;`, add:
```cpp
			delete segments[which].hnsw_graph;
```

- [ ] **Step 4: Run to verify it passes** — `rm -f obj/*.o && make all && ./bin/test_segment_index 2>&1 | grep -E "compaction_rebuilds_hnsw|PASSED"` → OK. Run the full binary 3x.

- [ ] **Step 5: Commit**

```bash
git add atire/atire_segment_index_compaction.cpp tests/test_segment_index.cpp
git commit -m "feat(vector-v3): compaction rebuilds the merged .hnsw graph (+ free input graphs)"
```

---

## Task 10: Node binding rider

**Files:** Modify `nodejs/addon/segment_index.cpp`, `nodejs/segment_index.d.ts`, `nodejs/README.md`, `nodejs/package.json`; create `nodejs/test/hnsw.test.js`

Mirror the shipped V2 Node rider (the `approximate` option / `buildSignatures` /
`searchVectorApprox` / `searchHybridApprox` work). Read that code first.

- [ ] **Step 1: Failing JS test** (`nodejs/test/hnsw.test.js`) — require the addon DIRECTLY
(like the sibling tests; `index.js` throws on modern Node):

```js
const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

test('hnsw: buildHnsw + searchVectorHnsw returns hits', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_hnsw_'));
  const dim = 16;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', hnsw: { M: 16, efConstruction: 200, efSearch: 64 } });
  idx.open(dir);
  for (let i = 0; i < 40; i++) {
    const v = new Float32Array(dim);
    for (let d = 0; d < dim; d++) v[d] = ((i + d) % 7) - 3;
    idx.addDocument('k' + i, '<DOC>alpha</DOC>', v);
  }
  await idx.flush();
  await idx.buildHnsw();
  const q = new Float32Array(dim);
  for (let d = 0; d < dim; d++) q[d] = (d % 5) - 2;
  const hits = idx.searchVectorHnsw(q, 5);
  assert.strictEqual(hits.length, 5);
  assert.ok(hits[0].key && typeof hits[0].score === 'number');
  idx.close();
});
```
Append `test/hnsw.test.js` to the `test:segment` script's file list in `package.json`.

- [ ] **Step 2: Run to verify it fails** — `cd nodejs && npm run build:segment && npm run test:segment 2>&1 | tail -8` → FAIL (`hnsw`/`buildHnsw`/`searchVectorHnsw` unknown). (If `node_modules` is missing, `npm install` first.)

- [ ] **Step 3: Wire the addon** (`nodejs/addon/segment_index.cpp`) — mirror the V2 `approximate`
work:
- Constructor: capture `hnsw` option into members (e.g. `long option_hnsw_M = -1; long option_hnsw_ef_construction = 0; long option_hnsw_ef_search = 0;` sentinels), parsing `Has("hnsw") && IsObject()` then `M`/`efConstruction`/`efSearch`.
- `Open()`: AFTER `engine->open()` returns 0 (where V2 applies `set_approximate_config`), apply:
```cpp
	if (option_hnsw_M >= 0)
		{
		engine->set_hnsw_config(option_hnsw_M, option_hnsw_ef_construction);	// non-fatal
		if (option_hnsw_ef_search > 0)
			engine->set_ef_search(option_hnsw_ef_search);
		}
```
- Add a `BUILD_HNSW` op to the `MaintenanceWorker` enum + `Execute()` switch (calling `engine->build_hnsw()`) and its OnOK/OnError message ternary; add a `BuildHnsw` method mirroring `BuildSignatures` (busy-guarded, deferred Promise).
- Add sync `SearchVectorHnsw` / `SearchHybridHnsw` mirroring `SearchVectorApprox` / `SearchHybridApprox` verbatim, changing only the engine call to `search_vector_hnsw` / `search_hybrid_hnsw`.
- Register `buildHnsw`, `searchVectorHnsw`, `searchHybridHnsw` in `DefineClass`; declare the three methods in the class body.

- [ ] **Step 4: d.ts + README.** In `nodejs/segment_index.d.ts` add to `SegmentIndexOptions`:
```ts
  hnsw?: { M?: number; efConstruction?: number; efSearch?: number };
```
and to the class:
```ts
  buildHnsw(): Promise<void>;
  searchVectorHnsw(vector: Float32Array | number[], k: number): Hit[];
  searchHybridHnsw(text: string, vector: Float32Array | number[], k: number): Hit[];
```
In `nodejs/README.md`, add an "HNSW graph search (V3)" subsection: the `hnsw` option (M default 16, efConstruction default 200, efSearch default 64), `await buildHnsw()`, and the `*Hnsw` methods; note cosine/L2 only — dot transparently falls back to exact.

- [ ] **Step 5: Run to verify it passes** — `cd nodejs && npm run build:segment && npm run test:segment 2>&1 | tail -8` → all pass (13 total: 12 prior + 1 new).

- [ ] **Step 6: Commit**

```bash
git add nodejs/addon/segment_index.cpp nodejs/segment_index.d.ts nodejs/README.md nodejs/package.json nodejs/test/hnsw.test.js
git commit -m "feat(vector-v3): Node binding -- hnsw option, buildHnsw, *Hnsw methods"
```

---

## Final verification (after all tasks)

```bash
cd /data/tyolab/code/antelope
rm -f obj/*.o && make all && make tests
for t in test_hnsw test_signature test_signature_store test_segment_index test_index_keymap \
         test_index_manifest test_index_tombstones test_index_merge test_vector_store test_wal \
         test_memory_engine_ownership; do
  printf "%-30s " "$t:"; ./bin/$t >/tmp/v3_$t.out 2>&1 && tail -1 /tmp/v3_$t.out || { echo FAILED; tail -8 /tmp/v3_$t.out; }
done
make engine_lib && ( cd nodejs && npm run build:segment && npm run test:segment 2>&1 | tail -6 )
```
Expected: all C++ suites `PASSED` (now 11 binaries incl. `test_hnsw`), JS `pass 13  fail 0`. The
exact `search_vector`/`search_hybrid` and V2 `*_approx` paths are byte-for-byte unchanged — the
pre-existing vector tests must still pass identically.

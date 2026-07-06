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
return -ANT_vector_store::kernel(vectors->get(a), query, vectors->get_dimension(), metric);
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

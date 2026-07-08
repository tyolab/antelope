/*
	HNSW.CPP
*/
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include <queue>
#include "hnsw.h"
#include "vector_source.h"
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

double ANT_hnsw::distance(long long a, const float *query, ANT_vector_source *vectors, long metric)
{
return -vectors->score(a, query, metric);		// score() handles float and int8 backends (reconstructs a for int8)
}

#ifdef ANT_HNSW_PROFILE
long long ant_hnsw_cache_hit = 0, ant_hnsw_cache_miss = 0;
#define PROF(counter) (counter++)
#else
#define PROF(counter) ((void)0)
#endif

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

long ANT_hnsw::build(ANT_vector_source *vectors, long long M_in, long long ef_construction_in, long metric, bool use_distance_cache)
{
long long n = vectors->document_count(), i;
ANT_mersenne_twister twister;
double mL;

if (n > ANT_HNSW_MAX_DOCUMENTS)
	return 1;			/* too many docs for int32 docids; caller falls back to exact scan */

delete [] levels; delete [] offsets; delete [] neighbours;
levels = NULL; offsets = NULL; neighbours = NULL;

documents = n;
M = M_in < 2 ? 16 : M_in;
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

/* SEARCH-LAYER visited set: allocated ONCE for the whole build, then reused
   across every insert/layer via a monotonically increasing epoch stamp
   instead of re-zeroing an n-sized array per call (was the O(n^2) hot spot). */
std::vector<long long> visited_epoch(n > 0 ? n : 1, 0);
long long current_epoch = 0;

/* Pair-keyed distance memo.  EVERY build distance is between two graph nodes
   a,b (distance(a,·) is symmetric: -kernel(get(a),get(b))), and docids are
   < 2^31 (ANT_HNSW_MAX_DOCUMENTS) so (min,max) packs into a u64 key that is
   always >= 1 (a != b), letting key 0 mark an empty slot.  The reverse-edge
   prune heuristic re-runs on nearly every connection to a full node,
   recomputing the same neighbour-pair distances -- caching them cuts the
   dominant build cost.  A flat open-addressing table (linear probing, no
   per-entry allocation) is used rather than std::unordered_map, whose node
   allocation costs more than recomputing a low-dimension distance.
   Deterministic: a hit returns exactly what distance() would, so the produced
   graph is byte-identical to the uncached build.  The table is a bounded
   power-of-two; at 75% load it is cleared and refilled, which only forces
   later recomputation -- no behaviour change.  When enabled it is a TRANSIENT
   working set of up to DCACHE_MAX_SLOTS * 16 bytes (~134MB, dkey+dval), freed
   when build() returns; the in-tree callers build one graph at a time.
   Gated on dimension: below ANT_HNSW_DISTANCE_CACHE_MIN_DIM a distance() is
   cheaper than a cache probe, so the table is neither allocated nor consulted
   (measured crossover ~dim 150; the caller may also force it off). */
static const size_t DCACHE_SLOTS_PER_DOC = 4096;	/* size the table to ~this many pairs/doc, so most stay resident before a clear */
static const size_t DCACHE_MAX_SLOTS = (size_t)1 << 23;	/* hard cap: 8.4M slots * 16 bytes ~= 134MB */
bool use_cache = use_distance_cache && vectors->get_dimension() >= ANT_HNSW_DISTANCE_CACHE_MIN_DIM;
size_t dcap = (size_t)1 << 16;
while (dcap < (size_t)n * DCACHE_SLOTS_PER_DOC && dcap < DCACHE_MAX_SLOTS) dcap <<= 1;
std::vector<unsigned long long> dkey(use_cache ? dcap : 0, 0ULL);
std::vector<double> dval(use_cache ? dcap : 0);
const size_t dmask = dcap - 1;
const unsigned dshift = 64 - (unsigned)__builtin_ctzll(dcap);
size_t dfill = 0;
const size_t dlimit = dcap - (dcap >> 2);			/* clear-and-refill at 75% load */
/* distance between two STORED nodes.  For int8 the "query" node b is
   reconstructed into bscratch (distance() reconstructs a internally via
   score()); for float, get(b) is a zero-copy pointer -- bit-identical to the
   pre-V4 -kernel(get(a),get(b)). */
std::vector<float> bscratch(vectors->is_quantized() ? (size_t)vectors->get_dimension() : 0);
auto dist_stored = [&](long long a, long long b) -> double
	{
	if (vectors->is_quantized())
		{ vectors->reconstruct(b, bscratch.data()); return distance(a, bscratch.data(), vectors, metric); }
	return distance(a, vectors->get(b), vectors, metric);
	};
auto dist_ids = [&](long long a, long long b) -> double
	{
	if (!use_cache)
		return dist_stored(a, b);
	unsigned long long lo = (unsigned long long)(a < b ? a : b);
	unsigned long long hi = (unsigned long long)(a < b ? b : a);
	unsigned long long key = (lo << 32) | hi;
	size_t i = (size_t)((key * 0x9E3779B97F4A7C15ULL) >> dshift);
	while (dkey[i]) { if (dkey[i] == key) { PROF(ant_hnsw_cache_hit); return dval[i]; } i = (i + 1) & dmask; }
	PROF(ant_hnsw_cache_miss);
	double d = dist_stored(a, b);
	if (dfill >= dlimit)						/* table full: wipe and re-find an empty slot */
		{
		std::fill(dkey.begin(), dkey.end(), 0ULL); dfill = 0;
		i = (size_t)((key * 0x9E3779B97F4A7C15ULL) >> dshift);
		while (dkey[i]) i = (i + 1) & dmask;
		}
	dkey[i] = key; dval[i] = d; dfill++;			/* i is the empty slot the probe stopped on */
	return d;
	};

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
			double best_d = dist_ids(ep, q);
			if (lc < (long long)adj[ep].size())
				for (size_t e = 0; e < adj[ep][lc].size(); e++)
					{
					long long cand = adj[ep][lc][e];
					double d = dist_ids(cand, q);
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
		current_epoch++;						/* fresh generation: O(1) instead of re-zeroing an n-sized array */
		double dep = dist_ids(ep, q);
		W.push(DN(dep, ep)); C.push(DN(dep, ep)); visited_epoch[ep] = current_epoch;
		while (!C.empty())
			{
			DN c = C.top(); C.pop();
			if (c.first > W.top().first) break;
			long long cnode = c.second;
			if (lc < (long long)adj[cnode].size())
				for (size_t e = 0; e < adj[cnode][lc].size(); e++)
					{
					long long ecand = adj[cnode][lc][e];
					if (visited_epoch[ecand] == current_epoch) continue;
					visited_epoch[ecand] = current_epoch;
					double de = dist_ids(ecand, q);
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
				{
				if (dist_ids(e, selected[si]) < e_to_q)
					{ good = 0; break; }
				}
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
					{
					nn.push_back(DN(dist_ids(adj[nbr][lc][z], nbr), adj[nbr][lc][z]));
					}
				std::sort(nn.begin(), nn.end());
				std::vector<long long> kept;
				for (size_t z = 0; z < nn.size() && (long long)kept.size() < degree_cap; z++)
					{
					long long e = nn[z].second; double e_to_nbr = nn[z].first; long good = 1;
					for (size_t si2 = 0; si2 < kept.size(); si2++)
						{
						if (dist_ids(e, kept[si2]) < e_to_nbr) { good = 0; break; }
						}
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
	ANT_vector_source *vectors, ANT_index_tombstones *tombstones,
	long long *out_docids, double *out_scores, const unsigned char *filter_bits)
{
if (entry_point < 0 || documents == 0)
	return 0;
long long ef = ef_search < top_k ? top_k : ef_search;

/* admit = live AND (no filter OR filter bit set); a non-admitted node still ROUTES via C for connectivity */
#define ANT_HNSW_ADMIT(docid) \
	((tombstones == NULL || !tombstones->is_deleted(docid)) && \
	 (filter_bits == NULL || (filter_bits[(docid) >> 3] & (1 << ((docid) & 7)))))

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
/* route through the entry point regardless of tombstone; only LIVE nodes enter the result heap W */
C.push(DN(dep, ep)); visited[ep] = 1;
if (ANT_HNSW_ADMIT(ep)) W.push(DN(dep, ep));
while (!C.empty())
	{
	DN c = C.top(); C.pop();
	if (!W.empty() && c.first > W.top().first) break;		/* empty W == +inf: keep exploring */
	long long count; const int *nb = neighbours_of(neighbours, offsets, levels, c.second, 0, &count);
	for (long long e = 0; e < count; e++)
		{
		long long ecand = nb[e];
		if (visited[ecand]) continue;
		visited[ecand] = 1;
		double de = distance(ecand, query, vectors, metric);
		if (W.empty() || (long long)W.size() < ef || de < W.top().first)
			{
			C.push(DN(de, ecand));		/* deleted/non-matching nodes still route (graph connectivity) */
			if (ANT_HNSW_ADMIT(ecand))
				{ W.push(DN(de, ecand)); if ((long long)W.size() > ef) W.pop(); }	/* W = up to ef LIVE matching nearest */
			}
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
	if (!ANT_HNSW_ADMIT(docid)) continue;
	out_docids[out] = docid;
	out_scores[out] = -found[i].first;		/* back to kernel: higher = nearer */
	out++;
	}
return out;
#undef ANT_HNSW_ADMIT
}

/*
	SAVE / LOAD -- persist the CSR topology to a seg_G.hnsw sidecar.  On-disk:
	header (magic u64, version u32, M i64, ef_construction i64, documents i64,
	entry_point i64, max_level i64) == 52 bytes, then levels[documents] (i32),
	offsets[documents+1] (i64), neighbours[offsets[documents]] (i32).
*/
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
	|| docs < 0 || docs > ANT_HNSW_MAX_DOCUMENTS || ep < -1 || ep >= docs || maxl < -1 || maxl > 4096)
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

/* content validation: every node's packed [count][docid...] layers must land
   exactly on off[node+1], every count must be non-negative and fit, and
   every neighbour docid must be in [0, docs).  A corrupt CSR would
   otherwise drive out-of-bounds indexing in search(); degrade instead. */
long content_ok = 1;
for (i = 0; content_ok && i < docs; i++)
	{
	long long p = off[i], end = off[i + 1];
	if (lv[i] < 0)
		{ if (end != p) content_ok = 0; continue; }		/* not-in-graph node must have an empty slice */
	for (long long layer = 0; content_ok && layer <= lv[i]; layer++)
		{
		if (p >= end) { content_ok = 0; break; }			/* ran out of slice before all layers */
		long long cnt = nb[p++];
		if (cnt < 0 || p + cnt > end) { content_ok = 0; break; }
		for (long long z = 0; z < cnt; z++)
			{
			int d = nb[p + z];
			if (d < 0 || d >= docs) { content_ok = 0; break; }
			}
		p += cnt;
		}
	if (content_ok && p != end) content_ok = 0;				/* must consume the slice exactly */
	}
if (!content_ok) { delete [] lv; delete [] off; delete [] nb; return g; }

g->documents = docs; g->M = m; g->M0 = 2*m; g->ef_construction = efc;
g->entry_point = ep; g->max_level = maxl;
g->levels = lv; g->offsets = off; g->neighbours = nb;
return g;
}

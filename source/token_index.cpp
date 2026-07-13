#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include "token_index.h"
#include "hnsw.h"
#include "vector_source.h"
#include "index_tombstones.h"

ANT_token_index::ANT_token_index()
{
graph = NULL; token_docid = NULL; token_count = 0; documents = 0; dimension = 0; metric = 0; M = 0; ef_construction = 0; source = NULL;
scratch_epoch = 0;
}

ANT_token_index::~ANT_token_index()
{
delete graph;
delete [] token_docid;
}

ANT_token_index *ANT_token_index::build(ANT_token_source *source_in, long long M_in, long long ef_construction_in, long metric_in)
{
long long n = source_in->document_count();			// token / node count
if (n <= 0)
	return NULL;                             // nothing to index
if (n > ANT_HNSW_MAX_DOCUMENTS)
	return NULL;                             // too many tokens for int32 node ids -> caller falls back

ANT_token_index *idx = new ANT_token_index();
idx->token_count = n;
idx->documents = source_in->num_documents();
idx->dimension = source_in->get_dimension();
idx->metric = metric_in;
idx->M = M_in;
idx->ef_construction = ef_construction_in;
idx->source = source_in;

idx->token_docid = new int[n];
for (long long t = 0; t < n; t++)
	idx->token_docid[t] = (int)source_in->token_docid_of(t);

idx->graph = new ANT_hnsw();
if (idx->graph->build(source_in, M_in, ef_construction_in, metric_in) != 0)
	{ delete idx; return NULL; }             // graph build failed -> caller falls back

return idx;
}

long ANT_token_index::empty(void)
{
return graph == NULL || graph->empty();
}

long ANT_token_index::token_docid_at(long long t)
{
return (t >= 0 && t < token_count) ? token_docid[t] : -1;
}

/*
	SAVE / LOAD -- persist the token->docid map + header to a .tann sidecar,
	and the graph topology to a paired `<filename>.g` (reusing ANT_hnsw's own
	save/load).  On-disk container: header (magic u64, version u32,
	token_count i64, M i64, ef_construction i64) == 28 bytes, then
	token_docid[token_count] (i32).
*/
#define ANT_TANN_VERSION 1u

static unsigned long long ant_tann_magic(void)
{
unsigned long long m; const char *s = "ANTTANN1"; memcpy(&m, s, 8); return m;		/* endian-correct */
}

long ANT_token_index::save(const char *filename)
{
char temp[4200], gfilename[4200]; FILE *fp;
unsigned long long magic = ant_tann_magic();
unsigned int version = ANT_TANN_VERSION;

if (graph == NULL)
	return 1;

if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp)) return 1;
if (snprintf(gfilename, sizeof(gfilename), "%s.g", filename) >= (int)sizeof(gfilename)) return 1;

if ((fp = fopen(temp, "wb")) == NULL) return 1;
if (fwrite(&magic,sizeof(magic),1,fp)!=1 || fwrite(&version,sizeof(version),1,fp)!=1
	|| fwrite(&token_count,sizeof(token_count),1,fp)!=1 || fwrite(&M,sizeof(M),1,fp)!=1
	|| fwrite(&ef_construction,sizeof(ef_construction),1,fp)!=1
	|| fwrite(token_docid,sizeof(int),(size_t)token_count,fp)!=(size_t)token_count)
	{ fclose(fp); remove(temp); return 1; }
fclose(fp);
if (rename(temp, filename) != 0) { remove(temp); return 1; }

if (graph->save(gfilename) != 0) { remove(filename); remove(gfilename); return 1; }	/* best-effort: also drop any stale/partial .g so a failed save doesn't leave one behind */
return 0;
}

ANT_token_index *ANT_token_index::load(const char *filename, ANT_token_source *source_in, long long expected_M, long long expected_ef_construction, long metric)
{
ANT_token_index *idx = new ANT_token_index();		/* empty by default (degraded) */
idx->source = source_in;
idx->metric = metric;
idx->M = expected_M;
idx->ef_construction = expected_ef_construction;

FILE *fp; unsigned long long magic; unsigned int version;
long long tc, m, efc;
if ((fp = fopen(filename, "rb")) == NULL) return idx;

if (fread(&magic,sizeof(magic),1,fp)!=1 || magic != ant_tann_magic()
	|| fread(&version,sizeof(version),1,fp)!=1 || version != ANT_TANN_VERSION
	|| fread(&tc,sizeof(tc),1,fp)!=1 || fread(&m,sizeof(m),1,fp)!=1
	|| fread(&efc,sizeof(efc),1,fp)!=1
	|| tc != source_in->document_count() || m != expected_M || efc != expected_ef_construction
	|| tc < 0 || tc > ANT_HNSW_MAX_DOCUMENTS)
	{ fclose(fp); return idx; }

long long header = 8 + 4 + 8 + 8 + 8, expected_size = header + 4*tc;
{
long long cur = ftell(fp), end;
if (fseek(fp, 0, SEEK_END) != 0 || (end = ftell(fp)) != expected_size) { fclose(fp); return idx; }
fseek(fp, cur, SEEK_SET);
}

int *docid = new int[tc > 0 ? tc : 1];
if (fread(docid,sizeof(int),(size_t)tc,fp)!=(size_t)tc)
	{ delete [] docid; fclose(fp); return idx; }
fclose(fp);

char gfilename[4200];
if (snprintf(gfilename, sizeof(gfilename), "%s.g", filename) >= (int)sizeof(gfilename))
	{ delete [] docid; return idx; }

ANT_hnsw *g = ANT_hnsw::load(gfilename, expected_M, expected_ef_construction, tc);
if (g == NULL || g->empty())
	{ delete g; delete [] docid; return idx; }

idx->graph = g;
idx->token_docid = docid;
idx->token_count = tc;
idx->documents = source_in->num_documents();
idx->dimension = source_in->get_dimension();

return idx;
}

/*
	SEARCH_CANDIDATES -- token-ANN candidate generation.  The token graph's
	nodes are TOKENS, so it must be searched with tombstones=NULL and
	filter_bits=NULL (those bitmaps are keyed by DOCID); doc-level tombstone
	and filter admission happens here, AFTER mapping token id -> docid --
	i.e. admission is post-hoc on whatever tokens the ANN traversal happened
	to surface, not in-traversal the way the dense/HNSW doc-level path can
	admit.  This is best-effort under a selective filter: docs whose tokens
	aren't among the nearest `token_top_p` per query vector can be missed
	entirely, however large max_candidates/token_top_p are set (over-gather
	reduces this risk, it does not eliminate it).  No guarantee of no-under-fill.
*/
long long ANT_token_index::search_candidates(const float *query, long long num_query_vecs,
	long long token_top_p, long long max_candidates,
	ANT_index_tombstones *tombstones, const unsigned char *filter_bits, long long *out_docids)
{
if (empty() || query == NULL || num_query_vecs <= 0 || max_candidates <= 0 || token_top_p <= 0)
	return 0;

// lazily size the reusable scratch to `documents` (once; grows only if documents grows)
if ((long long)scratch_touched_epoch.size() < documents)
	{
	scratch_provisional.assign((size_t)documents, 0.0);
	scratch_seen_query.assign((size_t)documents, -1);
	scratch_touched_epoch.assign((size_t)documents, 0);   // 0 != any epoch>=1 -> nothing looks touched
	}
scratch_epoch++;                                          // new call -> all prior touches invalidated

std::vector<long long> touched;                           // O(candidates), not O(documents)
std::vector<long long> tok_docids((size_t)token_top_p);
std::vector<double>    tok_scores((size_t)token_top_p);

for (long long i = 0; i < num_query_vecs; i++)
	{
	const float *q = query + i * dimension;
	// tombstones/filter passed as NULL: nodes are tokens, not docids -> admit at doc level below
	long long got = graph->search(q, metric, /*ef_search=*/token_top_p, token_top_p,
		source, /*tombstones=*/NULL, tok_docids.data(), tok_scores.data(), /*filter_bits=*/NULL);
	for (long long r = 0; r < got; r++)
		{
		long long t = tok_docids[r];               // token id
		if (t < 0 || t >= token_count) continue;
		long long d = token_docid[t];              // owning docid
		if (d < 0 || d >= documents) continue;
		if (tombstones != NULL && tombstones->is_deleted(d)) continue;
		if (filter_bits != NULL && !(filter_bits[d >> 3] & (1 << (d & 7)))) continue;
		if (scratch_touched_epoch[d] != scratch_epoch)   // first time this doc appears this call
			{
			scratch_touched_epoch[d] = scratch_epoch;
			scratch_provisional[d]   = 0.0;
			scratch_seen_query[d]    = -1;
			touched.push_back(d);
			}
		// results are descending by kernel, so the FIRST hit for this (query token, doc) is its best
		if (scratch_seen_query[d] == i) continue;        // already took this query token's best for d
		scratch_seen_query[d] = i;
		scratch_provisional[d] += tok_scores[r];
		}
	}

long long out_n = (long long)touched.size();
if (out_n > max_candidates) out_n = max_candidates;
std::partial_sort(touched.begin(), touched.begin() + out_n, touched.end(),
	[&](long long a, long long b){ return scratch_provisional[a] > scratch_provisional[b]; });
for (long long j = 0; j < out_n; j++)
	out_docids[j] = touched[j];
return out_n;
}

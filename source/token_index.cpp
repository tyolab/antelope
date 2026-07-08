#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "token_index.h"
#include "hnsw.h"
#include "multivector_store.h"

ANT_token_index::ANT_token_index()
{
graph = NULL; token_docid = NULL; token_count = 0; documents = 0; dimension = 0; metric = 0; M = 0; ef_construction = 0; store = NULL;
}

ANT_token_index::~ANT_token_index()
{
delete graph;
delete [] token_docid;
}

ANT_token_index *ANT_token_index::build(ANT_multivector_store *store, long long M_in, long long ef_construction_in, long metric_in)
{
long long n = store->token_count();
if (n <= 0)
	return NULL;                             // nothing to index
if (n > ANT_HNSW_MAX_DOCUMENTS)
	return NULL;                             // too many tokens for int32 node ids -> caller falls back

ANT_token_index *idx = new ANT_token_index();
idx->token_count = n;
idx->documents = store->document_count();
idx->dimension = store->get_dimension();
idx->metric = metric_in;
idx->M = M_in;
idx->ef_construction = ef_construction_in;
idx->store = store;

idx->token_docid = new int[n];
for (long long t = 0; t < n; t++)
	idx->token_docid[t] = (int)store->token_docid_of(t);

ANT_multivector_source source(store);
idx->graph = new ANT_hnsw();
if (idx->graph->build(&source, M_in, ef_construction_in, metric_in) != 0)
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

if (graph->save(gfilename) != 0) { remove(filename); return 1; }
return 0;
}

ANT_token_index *ANT_token_index::load(const char *filename, ANT_multivector_store *store, long long expected_M, long long expected_ef_construction, long metric)
{
ANT_token_index *idx = new ANT_token_index();		/* empty by default (degraded) */
idx->store = store;
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
	|| tc != store->token_count() || m != expected_M || efc != expected_ef_construction
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
idx->documents = store->document_count();
idx->dimension = store->get_dimension();

return idx;
}

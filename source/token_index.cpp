#include <stdlib.h>
#include "token_index.h"
#include "hnsw.h"
#include "multivector_store.h"

ANT_token_index::ANT_token_index()
{
graph = NULL; token_docid = NULL; token_count = 0; documents = 0; dimension = 0; metric = 0; M = 0; ef_construction = 0;
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

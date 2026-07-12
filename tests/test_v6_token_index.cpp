/*
	TEST_V6_TOKEN_INDEX.CPP
	-----------------------
	Exercises ANT_token_index: build over a .mvec token pool's flattened
	tokens (nodes == tokens) plus the token->docid map.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/token_index.h"
#include "../source/multivector_store.h"
#include "../source/vector_store.h"
#include "../source/index_tombstones.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/*
	build_store()
	-------------
	dim=4, 3 docs with token counts {2, 1, 3} (6 tokens total).
*/
static ANT_multivector_store *build_store(const char *path)
{
long long dim = 4;
float doc0[2*4] =
	{
	1, 0, 0, 0,
	0, 1, 0, 0
	};
float doc1[1*4] =
	{
	0, 0, 1, 0
	};
float doc2[3*4] =
	{
	0, 0, 0, 1,
	1, 1, 0, 0,
	0, 1, 1, 0
	};

ANT_multivector_store_writer w;
CHECK(w.create(path, dim) == 0);
CHECK(w.append(doc0, 2) == 0);
CHECK(w.append(doc1, 1) == 0);
CHECK(w.append(doc2, 3) == 0);
CHECK(w.finish() == 0);

return ANT_multivector_store::load(path, dim, 3);
}

static void build_test(void)
{
char path[64]; strcpy(path, "/tmp/ant_v6tokidx_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }

ANT_multivector_store *store = build_store(path);
CHECK(store != NULL);
ANT_multivector_source store_src(store);

ANT_token_index *idx = ANT_token_index::build(&store_src, 16, 200, 0 /* METRIC_DOT */);
CHECK(idx != NULL);
CHECK(idx->get_token_count() == 6);
CHECK(idx->get_documents() == 3);
CHECK(!idx->empty());

CHECK(idx->token_docid_at(0) == 0);
CHECK(idx->token_docid_at(1) == 0);
CHECK(idx->token_docid_at(2) == 1);
CHECK(idx->token_docid_at(3) == 2);
CHECK(idx->token_docid_at(5) == 2);
CHECK(idx->token_docid_at(6) == -1);

delete idx;
delete store;
unlink(path);
printf("build_test OK\n");
}

static void empty_store_test(void)
{
char path[64]; strcpy(path, "/tmp/ant_v6tokidx_empty_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }

long long dim = 4;
ANT_multivector_store_writer w;
CHECK(w.create(path, dim) == 0);
CHECK(w.append(NULL, 0) == 0);
CHECK(w.finish() == 0);

ANT_multivector_store *store = ANT_multivector_store::load(path, dim, 1);
CHECK(store != NULL);
CHECK(store->token_count() == 0);
ANT_multivector_source store_src(store);

ANT_token_index *idx = ANT_token_index::build(&store_src, 16, 200, 0 /* METRIC_DOT */);
CHECK(idx == NULL);

delete store;
unlink(path);
printf("empty_store_test OK\n");
}

static void save_load_test(void)
{
char store_path[64]; strcpy(store_path, "/tmp/ant_v6tokidx_sl_XXXXXX"); { int fd = mkstemp(store_path); if (fd >= 0) close(fd); }
ANT_multivector_store *store = build_store(store_path);
CHECK(store != NULL);
ANT_multivector_source store_src(store);

ANT_token_index *idx = ANT_token_index::build(&store_src, 16, 200, 0 /* METRIC_DOT */);
CHECK(idx != NULL);

char path[64]; strcpy(path, "/tmp/ant_tann_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }
CHECK(idx->save(path) == 0);

ANT_token_index *r = ANT_token_index::load(path, &store_src, 16, 200, 0);
CHECK(r != NULL);
CHECK(!r->empty());
CHECK(r->get_token_count() == 6);
CHECK(r->token_docid_at(2) == 1 && r->token_docid_at(5) == 2);
delete r;

/* nonexistent path -> empty, no crash */
ANT_token_index *m = ANT_token_index::load("/tmp/does_not_exist_v6tann", &store_src, 16, 200, 0);
CHECK(m != NULL && m->empty());
delete m;

/* corruption: truncate the container file -> empty, no crash */
{
FILE *f = fopen(path, "r+");
CHECK(f != NULL);
CHECK(ftruncate(fileno(f), 10) == 0);
fclose(f);
}
ANT_token_index *c = ANT_token_index::load(path, &store_src, 16, 200, 0);
CHECK(c != NULL && c->empty());
delete c;

/* config mismatch (wrong M) -> empty; re-save fresh since `path` was truncated above */
char path2[64]; strcpy(path2, "/tmp/ant_tann_XXXXXX"); { int fd = mkstemp(path2); if (fd >= 0) close(fd); }
CHECK(idx->save(path2) == 0);
ANT_token_index *w = ANT_token_index::load(path2, &store_src, 32 /* wrong M */, 200, 0);
CHECK(w != NULL && w->empty());
delete w;

/* stale token_count: a different store with a different token_count */
char store2_path[64]; strcpy(store2_path, "/tmp/ant_v6tokidx_sl2_XXXXXX"); { int fd = mkstemp(store2_path); if (fd >= 0) close(fd); }
long long dim = 4;
float doc0[1*4] = { 1, 0, 0, 0 };
ANT_multivector_store_writer w2;
CHECK(w2.create(store2_path, dim) == 0);
CHECK(w2.append(doc0, 1) == 0);
CHECK(w2.finish() == 0);
ANT_multivector_store *store2 = ANT_multivector_store::load(store2_path, dim, 1);
CHECK(store2 != NULL);
ANT_multivector_source store2_src(store2);
ANT_token_index *idx2 = ANT_token_index::build(&store2_src, 16, 200, 0);
CHECK(idx2 != NULL);
char path3[64]; strcpy(path3, "/tmp/ant_tann_XXXXXX"); { int fd = mkstemp(path3); if (fd >= 0) close(fd); }
CHECK(idx2->save(path3) == 0);

ANT_token_index *stale = ANT_token_index::load(path3, &store_src /* wrong token_count */, 16, 200, 0);
CHECK(stale != NULL && stale->empty());
delete stale;

delete idx2;
delete store2;
unlink(store2_path);
unlink(path3);

delete idx;
delete store;
unlink(store_path);
unlink(path);
unlink(path2);
{ char g[80]; snprintf(g, sizeof(g), "%s.g", path); unlink(g); }
{ char g[80]; snprintf(g, sizeof(g), "%s.g", path2); unlink(g); }
printf("save_load_test OK\n");
}

/*
	build_candidates_store()
	-------------------------
	dim=4, 4 docs.  Tokens are deliberately NOT tied on dot-product with the
	query axes e0=(1,0,0,0)/e1=(0,1,0,0): doc0/doc3 carry an e0-leaning token
	(dot(e0)=1, dot(e1)=0.1), doc1 carries an e1-leaning token (dot(e0)=0.1,
	dot(e1)=1), and doc2's token is deliberately the worst match on BOTH axes
	(dot(e0)=dot(e1)=0.01) so it ranks below the others with no score ties at
	the top_p=3 cutoff used by the candidate tests below.  doc3 additionally
	carries an irrelevant token (0,0,0,1) to prove multi-token docs dedupe to
	their single best hit per query token.
*/
static ANT_multivector_store *build_candidates_store(const char *path)
{
long long dim = 4;
float doc0[1*4] = { 1, 0.1f, 0, 0 };
float doc1[1*4] = { 0.1f, 1, 0, 0 };
float doc2[1*4] = { 0.01f, 0.01f, 1, 0 };
float doc3[2*4] =
	{
	1, 0.1f, 0, 0,
	0, 0, 0, 1
	};

ANT_multivector_store_writer w;
CHECK(w.create(path, dim) == 0);
CHECK(w.append(doc0, 1) == 0);
CHECK(w.append(doc1, 1) == 0);
CHECK(w.append(doc2, 1) == 0);
CHECK(w.append(doc3, 2) == 0);
CHECK(w.finish() == 0);

return ANT_multivector_store::load(path, dim, 4);
}

static long contains(long long *arr, long long n, long long v)
{
long long i;
for (i = 0; i < n; i++)
	if (arr[i] == v)
		return 1;
return 0;
}

static void search_candidates_basic_test(void)
{
char path[64]; strcpy(path, "/tmp/ant_v6tokidx_cand_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }
ANT_multivector_store *store = build_candidates_store(path);
CHECK(store != NULL);
ANT_multivector_source store_src(store);

ANT_token_index *idx = ANT_token_index::build(&store_src, 16, 200, 0 /* METRIC_DOT */);
CHECK(idx != NULL);

float query[2*4] =
	{
	1, 0, 0, 0,   /* e0 */
	0, 1, 0, 0    /* e1 */
	};
long long out[10];
long long n = idx->search_candidates(query, 2, /*token_top_p=*/3, /*max_candidates=*/10, NULL, NULL, out);
CHECK(n == 3);
CHECK(contains(out, n, 0));   /* doc0 hit via e0 */
CHECK(contains(out, n, 1));   /* doc1 hit via e1 */
CHECK(contains(out, n, 3));   /* doc3 hit via e0 */
CHECK(!contains(out, n, 2));  /* doc2 not near either query token */

delete idx;
delete store;
unlink(path);
printf("search_candidates_basic_test OK\n");
}

static void search_candidates_cap_test(void)
{
char path[64]; strcpy(path, "/tmp/ant_v6tokidx_cap_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }
ANT_multivector_store *store = build_candidates_store(path);
CHECK(store != NULL);
ANT_multivector_source store_src(store);

ANT_token_index *idx = ANT_token_index::build(&store_src, 16, 200, 0 /* METRIC_DOT */);
CHECK(idx != NULL);

float query[2*4] =
	{
	1, 0, 0, 0,   /* e0 */
	0, 1, 0, 0    /* e1 */
	};
long long out[10];
long long n = idx->search_candidates(query, 2, /*token_top_p=*/3, /*max_candidates=*/2, NULL, NULL, out);
CHECK(n == 2);
CHECK(contains(out, n, 0) || contains(out, n, 1) || contains(out, n, 3));
{
long long i;
for (i = 0; i < n; i++)
	CHECK(out[i] == 0 || out[i] == 1 || out[i] == 3);
}

delete idx;
delete store;
unlink(path);
printf("search_candidates_cap_test OK\n");
}

static void search_candidates_filter_test(void)
{
char path[64]; strcpy(path, "/tmp/ant_v6tokidx_filt_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }
ANT_multivector_store *store = build_candidates_store(path);
CHECK(store != NULL);
ANT_multivector_source store_src(store);

ANT_token_index *idx = ANT_token_index::build(&store_src, 16, 200, 0 /* METRIC_DOT */);
CHECK(idx != NULL);

/* admit only doc1 (bit 1) and doc3 (bit 3) */
unsigned char filter_bits = (1 << 1) | (1 << 3);

float query[2*4] =
	{
	1, 0, 0, 0,   /* e0 */
	0, 1, 0, 0    /* e1 */
	};
long long out[10];
long long n = idx->search_candidates(query, 2, /*token_top_p=*/3, /*max_candidates=*/10, NULL, &filter_bits, out);
CHECK(!contains(out, n, 0));   /* doc0 excluded by filter */
CHECK(!contains(out, n, 2));   /* doc2 was never a match anyway */
{
long long i;
for (i = 0; i < n; i++)
	CHECK(out[i] == 1 || out[i] == 3);
}

delete idx;
delete store;
unlink(path);
printf("search_candidates_filter_test OK\n");
}

static void search_candidates_tombstone_test(void)
{
char path[64]; strcpy(path, "/tmp/ant_v6tokidx_tomb_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }
ANT_multivector_store *store = build_candidates_store(path);
CHECK(store != NULL);
ANT_multivector_source store_src(store);

ANT_token_index *idx = ANT_token_index::build(&store_src, 16, 200, 0 /* METRIC_DOT */);
CHECK(idx != NULL);

ANT_index_tombstones stones(idx->get_documents());
stones.set_deleted(0);

float query[1*4] = { 1, 0, 0, 0 };   /* e0 */
long long out[10];
long long n = idx->search_candidates(query, 1, /*token_top_p=*/3, /*max_candidates=*/10, &stones, NULL, out);
CHECK(!contains(out, n, 0));   /* doc0 deleted */
CHECK(contains(out, n, 3));    /* doc3 also has e0, still present */

delete idx;
delete store;
unlink(path);
printf("search_candidates_tombstone_test OK\n");
}

static void search_candidates_empty_index_test(void)
{
char path[64]; strcpy(path, "/tmp/ant_v6tokidx_ce_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }
ANT_multivector_store *store = build_candidates_store(path);
CHECK(store != NULL);
ANT_multivector_source store_src(store);

ANT_token_index *idx = ANT_token_index::load("/tmp/ant_v6tokidx_does_not_exist", &store_src, 16, 200, 0);
CHECK(idx != NULL);
CHECK(idx->empty());

float query[1*4] = { 1, 0, 0, 0 };
long long out[10];
long long n = idx->search_candidates(query, 1, 8, 10, NULL, NULL, out);
CHECK(n == 0);

delete idx;
delete store;
unlink(path);
printf("search_candidates_empty_index_test OK\n");
}

int main(void)
{
build_test();
empty_store_test();
save_load_test();
search_candidates_basic_test();
search_candidates_cap_test();
search_candidates_filter_test();
search_candidates_tombstone_test();
search_candidates_empty_index_test();
printf("PASSED\n");
return 0;
}

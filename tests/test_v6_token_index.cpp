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

ANT_token_index *idx = ANT_token_index::build(store, 16, 200, 0 /* METRIC_DOT */);
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

ANT_token_index *idx = ANT_token_index::build(store, 16, 200, 0 /* METRIC_DOT */);
CHECK(idx == NULL);

delete store;
unlink(path);
printf("empty_store_test OK\n");
}

int main(void)
{
build_test();
empty_store_test();
printf("PASSED\n");
return 0;
}

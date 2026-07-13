/*
	TEST_V6_TOKEN_SCRATCH.CPP -- #17: search_candidates reuses epoch-stamped
	scratch across calls. Locks behavior-neutrality: repeated and interleaved
	queries on one index must each return the correct top-k (a stale scratch
	reset would corrupt the 2nd+ call).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"
#include "../source/filter.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_v6_scratch_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

// dim=4 single-token docs; dot-product order is hand-computable.
static ATIRE_segment_index *build_fixture(char **dir_out)
{
char *dir = make_index_dir();
*dir_out = dir;
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

struct { const char *key; float v[4]; } docs[5] =
	{
	{ "d0", {1.0f, 0.0f, 0.0f, 0.0f} },
	{ "d1", {0.9f, 0.1f, 0.0f, 0.0f} },
	{ "d2", {0.0f, 1.0f, 0.0f, 0.0f} },
	{ "d3", {0.1f, 0.9f, 0.0f, 0.0f} },
	{ "d4", {0.0f, 0.0f, 1.0f, 0.0f} },
	};
for (long long i = 0; i < 5; i++)
	{
	char doc[64]; sprintf(doc, "<DOC>%s</DOC>", docs[i].key);
	CHECK(ix->add_document(docs[i].key, doc, NULL, docs[i].v, 1) >= 0);
	}
CHECK(ix->flush() == 0);
CHECK(ix->build_token_index() == 0);   // segment gets a .tann -> token-ANN path active
return ix;
}

int main(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

float qa[4] = {1.0f, 0.0f, 0.0f, 0.0f};   // nearest: d0 then d1
float qb[4] = {0.0f, 1.0f, 0.0f, 0.0f};   // nearest: d2 then d3

// same query twice -> identical top hit both times (scratch reused, not corrupted)
CHECK(ix->search_multivector(qa, 1, 2) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);
CHECK(ix->search_multivector(qa, 1, 2) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);

// interleave a different query -> its own correct top hit (no carry-over from qa)
CHECK(ix->search_multivector(qb, 1, 2) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "d2") == 0);

// back to qa -> still correct
CHECK(ix->search_multivector(qa, 1, 2) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);

delete ix;
delete [] dir;
printf("ALL TESTS PASSED\n");
return 0;
}

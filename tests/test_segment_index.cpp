/*
	TEST_SEGMENT_INDEX.CPP
	----------------------
	End-to-end tests for the segmented incremental index.  Each task in the
	implementation plan appends a test function here; main() calls them in order.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_segidx_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	TEST_NRT_ADD_AND_SEARCH()
	-------------------------
	Documents are searchable immediately after add_document, before any flush.
*/
static void test_nrt_add_and_search(void)
{
char *dir = make_index_dir();
char query[64];
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

long long h1 = index->add_document("doc-1", "<DOC>aardvark zebra</DOC>");
long long h2 = index->add_document("doc-2", "<DOC>zebra quokka</DOC>");
CHECK(h1 >= 0);
CHECK(h2 >= 0);
CHECK(h1 != h2);

strcpy(query, "zebra");
long long hits = index->search(query, 10);
CHECK(hits == 2);

strcpy(query, "aardvark");
hits = index->search(query, 10);
CHECK(hits == 1);
ATIRE_segment_index::hit *hit = index->get_hit(0);
CHECK(strcmp(hit->filename, "doc-1") == 0);
CHECK(ATIRE_segment_index::make_handle(hit->generation, hit->docid) == h1);

/*
	A doc added after a search is visible to the next search (dirty rebuild)
*/
index->add_document("doc-3", "<DOC>aardvark wombat</DOC>");
strcpy(query, "aardvark");
CHECK(index->search(query, 10) == 2);

delete index;
delete [] dir;
printf("test_nrt_add_and_search OK\n");
}

int main(void)
{
test_nrt_add_and_search();
printf("PASSED\n");
return 0;
}

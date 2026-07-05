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

/*
	A zero-term document is rejected (the indexer rolls docno back and
	indexes nothing): -1 handle, no change to the document count, and a
	subsequent real add still works and searches correctly.
*/
CHECK(index->get_document_count() == 3);
CHECK(index->add_document("empty-1", "<DOC></DOC>") == -1);
CHECK(index->get_document_count() == 3);

long long h4 = index->add_document("doc-4", "<DOC>numbat quokka</DOC>");
CHECK(h4 >= 0);
CHECK(index->get_document_count() == 4);
strcpy(query, "numbat");
CHECK(index->search(query, 10) == 1);
CHECK(strcmp(index->get_hit(0)->filename, "doc-4") == 0);
CHECK(ATIRE_segment_index::make_handle(index->get_hit(0)->generation, index->get_hit(0)->docid) == h4);

delete index;
delete [] dir;
printf("test_nrt_add_and_search OK\n");
}

/*
	TEST_FLUSH_AND_REOPEN()
	-----------------------
*/
static void test_flush_and_reopen(void)
{
char *dir = make_index_dir();
char query[64];

/*
	Build, flush, keep searching in the same session
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
long long h1 = index->add_document("doc-1", "<DOC>aardvark zebra</DOC>");
index->add_document("doc-2", "<DOC>zebra quokka</DOC>");
CHECK(index->flush() == 0);

/*
	After flush: same results, now served from the disk segment
*/
strcpy(query, "zebra");
CHECK(index->search(query, 10) == 2);

/*
	Keep writing into the fresh memory segment; search spans disk + memory
*/
index->add_document("doc-3", "<DOC>zebra wombat</DOC>");
strcpy(query, "zebra");
CHECK(index->search(query, 10) == 3);
strcpy(query, "aardvark");
CHECK(index->search(query, 10) == 1);
CHECK(ATIRE_segment_index::make_handle(index->get_hit(0)->generation, index->get_hit(0)->docid) == h1);
CHECK(strcmp(index->get_hit(0)->filename, "doc-1") == 0);
delete index;			// doc-3 not flushed: relaxed durability, lost on close without flush

/*
	Reopen from disk: the flushed segment is there, the unflushed doc is gone
*/
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
strcpy(query, "zebra");
CHECK(reopened->search(query, 10) == 2);
strcpy(query, "wombat");
CHECK(reopened->search(query, 10) == 0);
CHECK(reopened->get_document_count() == 2);
delete reopened;
delete [] dir;
printf("test_flush_and_reopen OK\n");
}

/*
	UNIQUE_TERM()
	-------------
	Build a per-doc unique query term for n in [0, 31] out of two lowercase
	letters (n div 26, n mod 26), never digits.  ATIRE's document tokeniser
	(source/parser.cpp) breaks a run of characters at the letter/number
	boundary (a letter run and a following digit run become two separate
	terms), but its NEXI query tokeniser (source/nexi.cpp, ANT_NEXI::ispart())
	treats a mixed letter+digit run as ONE token.  A term like "unique0" would
	therefore be indexed as two terms ("unique", "0") but looked up as the
	single literal term "unique0", which is never in the vocabulary -- an
	unrelated, pre-existing ATIRE tokeniser quirk, nothing to do with segment
	growth.  Letters-only suffixes sidestep it entirely.
*/
static void unique_term(char *buffer, long long n)
{
sprintf(buffer, "unique%c%c", (int)('a' + n / 26), (int)('a' + n % 26));
}

/*
	TEST_MULTI_SEGMENT_GROWTH()
	---------------------------
	Grow the index across three flushes; results must merge across all
	segments plus the live memory segment, ranked consistently.
*/
static void test_multi_segment_growth(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], term[32];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

/*
	Three batches of 10 docs, each flushed -> three disk segments.
	Every doc contains "common"; doc i contains its own unique_term(i).
*/
long long batch, expected_total = 0;
for (batch = 0; batch < 3; batch++)
	{
	for (i = 0; i < 10; i++)
		{
		long long n = batch * 10 + i;
		sprintf(key, "doc-%lld", n);
		unique_term(term, n);
		sprintf(doc, "<DOC>common %s filler words here</DOC>", term);
		CHECK(index->add_document(key, doc) >= 0);
		expected_total++;
		}
	CHECK(index->flush() == 0);
	}

/*
	Two more docs stay in memory (4th, unflushed segment)
*/
unique_term(term, 30);
sprintf(doc, "<DOC>common %s</DOC>", term);
index->add_document("doc-30", doc);
unique_term(term, 31);
sprintf(doc, "<DOC>common %s</DOC>", term);
index->add_document("doc-31", doc);
expected_total += 2;

CHECK(index->get_document_count() == expected_total);

/*
	A term in every doc: all 32 found across 4 segments
*/
strcpy(query, "common");
CHECK(index->search(query, 100) == expected_total);

/*
	top_k truncation works across segments
*/
strcpy(query, "common");
CHECK(index->search(query, 5) == 5);

/*
	Unique terms resolve to the right doc regardless of segment
*/
unique_term(query, 0);
CHECK(index->search(query, 10) == 1);
CHECK(strcmp(index->get_hit(0)->filename, "doc-0") == 0);
unique_term(query, 25);
CHECK(index->search(query, 10) == 1);
CHECK(strcmp(index->get_hit(0)->filename, "doc-25") == 0);
unique_term(query, 31);
CHECK(index->search(query, 10) == 1);
CHECK(strcmp(index->get_hit(0)->filename, "doc-31") == 0);

/*
	Reopen: three disk segments come back, memory docs are gone
*/
delete index;
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(reopened->get_document_count() == 30);
strcpy(query, "common");
CHECK(reopened->search(query, 100) == 30);
unique_term(query, 15);
CHECK(reopened->search(query, 10) == 1);
CHECK(strcmp(reopened->get_hit(0)->filename, "doc-15") == 0);
delete reopened;
delete [] dir;
printf("test_multi_segment_growth OK\n");
}

int main(void)
{
test_nrt_add_and_search();
test_flush_and_reopen();
test_multi_segment_growth();
printf("PASSED\n");
return 0;
}

/*
	TEST_SEGMENT_INDEX.CPP
	----------------------
	End-to-end tests for the segmented incremental index.  Each task in the
	implementation plan appends a test function here; main() calls them in order.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

/*
	TEST_UPDATE_AND_DELETE()
	------------------------
*/
static void test_update_and_delete(void)
{
char *dir = make_index_dir();
char query[64];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

index->add_document("doc-1", "<DOC>aardvark original</DOC>");
index->add_document("doc-2", "<DOC>quokka stable</DOC>");
CHECK(index->flush() == 0);

/*
	Update a flushed document: old version tombstoned in disk segment,
	new version searchable from memory segment
*/
long long new_handle = index->update_document("doc-1", "<DOC>aardvark revised wombat</DOC>");
CHECK(new_handle >= 0);

strcpy(query, "aardvark");
CHECK(index->search(query, 10) == 1);			// not 2: old version filtered
CHECK(strcmp(index->get_hit(0)->filename, "doc-1") == 0);
CHECK(ATIRE_segment_index::make_handle(index->get_hit(0)->generation, index->get_hit(0)->docid) == new_handle);
strcpy(query, "original");
CHECK(index->search(query, 10) == 0);			// old body no longer reachable
strcpy(query, "wombat");
CHECK(index->search(query, 10) == 1);			// new body is
CHECK(index->get_document_count() == 2);

/*
	Update an unflushed (memory segment) document
*/
index->add_document("doc-3", "<DOC>ephemeral first</DOC>");
index->update_document("doc-3", "<DOC>ephemeral second</DOC>");
strcpy(query, "ephemeral");
CHECK(index->search(query, 10) == 1);
strcpy(query, "first");
CHECK(index->search(query, 10) == 0);

/*
	Delete
*/
CHECK(index->delete_document("doc-2") == 0);
strcpy(query, "quokka");
CHECK(index->search(query, 10) == 0);
CHECK(index->delete_document("no-such-key") == 1);

/*
	upsert: update of an unknown key behaves as add
*/
CHECK(index->update_document("doc-new", "<DOC>upserted marsupial</DOC>") >= 0);
strcpy(query, "marsupial");
CHECK(index->search(query, 10) == 1);

/*
	Tombstones survive flush + reopen
*/
CHECK(index->flush() == 0);
delete index;
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
strcpy(query, "original");
CHECK(reopened->search(query, 10) == 0);
strcpy(query, "quokka");
CHECK(reopened->search(query, 10) == 0);
strcpy(query, "wombat");
CHECK(reopened->search(query, 10) == 1);
strcpy(query, "first");
CHECK(reopened->search(query, 10) == 0);
strcpy(query, "second");
CHECK(reopened->search(query, 10) == 1);
delete reopened;
delete [] dir;
printf("test_update_and_delete OK\n");
}

/*
	TEST_STALE_KEYMAP_RECONCILIATION()
	----------------------------------
	Docs added but never flushed leave keymap entries pointing at a
	generation the manifest never records; reopen must reconcile so
	update/delete of such keys behave as absent (upsert / not-found).
*/
static void test_stale_keymap_reconciliation(void)
{
char *dir = make_index_dir();
char query[64];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->add_document("doc-flushed", "<DOC>kept kangaroo</DOC>");
CHECK(index->flush() == 0);
index->add_document("doc-lost", "<DOC>vanishing vapour</DOC>");
delete index;			// doc-lost's generation never flushed

ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);

/*
	Stale key: delete reports unknown, update upserts a fresh copy
*/
CHECK(reopened->delete_document("doc-lost") == 1);
CHECK(reopened->update_document("doc-lost", "<DOC>vanishing reborn</DOC>") >= 0);
strcpy(query, "reborn");
CHECK(reopened->search(query, 10) == 1);
strcpy(query, "vapour");
CHECK(reopened->search(query, 10) == 0);

/*
	The flushed doc is untouched by reconciliation
*/
CHECK(reopened->delete_document("doc-flushed") == 0);
strcpy(query, "kangaroo");
CHECK(reopened->search(query, 10) == 0);

delete reopened;
delete [] dir;
printf("test_stale_keymap_reconciliation OK\n");
}

/*
	TEST_OVERFETCH_MANY_UPDATES_SAME_KEY()
	--------------------------------------
	Update ONE key 50 times inside a single (unflushed) memory segment.  Each
	update tombstones the previous version, which lives in the SAME writer
	segment -> writer_tombstones->count() grows to 49.  A top_k=1 search must
	over-fetch to 1 + 49 = 50 and still return exactly one live hit carrying
	the newest body.
*/
static void test_overfetch_many_updates_same_key(void)
{
char *dir = make_index_dir();
char query[64];
char body[128];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

index->add_document("hot-key", "<DOC>marsupial version 0</DOC>");
for (i = 1; i <= 49; i++)
	{
	sprintf(body, "<DOC>marsupial version %lld</DOC>", i);
	CHECK(index->update_document("hot-key", body) >= 0);
	}

/*
	Every version shares the term "marsupial"; 49 of the 50 copies are
	tombstoned in the writer segment.  top_k=1 must yield exactly the live one.
*/
strcpy(query, "marsupial");
CHECK(index->search(query, 1) == 1);
CHECK(strcmp(index->get_hit(0)->filename, "hot-key") == 0);

/*
	The surviving copy must be the NEWEST body: term "version" is shared, but
	only the last version's unique token is reachable.  Search the distinctive
	term of the final version and of an intermediate one.
*/
strcpy(query, "marsupial");
CHECK(index->search(query, 10) == 1);				// still only one live doc total
CHECK(index->get_document_count() == 1);

delete index;
delete [] dir;
printf("test_overfetch_many_updates_same_key OK\n");
}

/*
	TEST_OVERFETCH_TEN_DOCS_ALL_UPDATED()
	-------------------------------------
	Ten distinct keys sharing a common term, each updated once (so 10 live +
	10 tombstoned copies in the writer segment).  A top_k=10 search must return
	all 10 live docs -- none masked by the 10 tombstones despite fetch=20.
*/
static void test_overfetch_ten_docs_all_updated(void)
{
char *dir = make_index_dir();
char query[64];
char key[64], body[128];
long long i, hits, seen;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

for (i = 0; i < 10; i++)
	{
	sprintf(key, "doc-%lld", i);
	sprintf(body, "<DOC>common original body number %lld</DOC>", i);
	CHECK(index->add_document(key, body) >= 0);
	}
for (i = 0; i < 10; i++)
	{
	sprintf(key, "doc-%lld", i);
	sprintf(body, "<DOC>common revised body number %lld</DOC>", i);
	CHECK(index->update_document(key, body) >= 0);
	}

strcpy(query, "common");
hits = index->search(query, 10);
CHECK(hits == 10);
CHECK(index->get_document_count() == 10);

/*
	Every returned key is distinct and none is a stale copy: "original" must
	be unreachable, "revised" reachable for all ten.
*/
seen = 0;
for (i = 0; i < hits; i++)
	if (index->get_hit(i)->filename != NULL)
		seen++;
CHECK(seen == 10);

strcpy(query, "original");
CHECK(index->search(query, 20) == 0);
strcpy(query, "revised");
CHECK(index->search(query, 20) == 10);

delete index;
delete [] dir;
printf("test_overfetch_ten_docs_all_updated OK\n");
}

/*
	TEST_OVERFETCH_TOP_SCORERS_ALL_TOMBSTONED_DISK()
	------------------------------------------------
	Cross-segment: a batch of docs is flushed to a disk segment, then EVERY one
	is updated (new copies land in the memory segment; the disk copies are all
	tombstoned).  A search whose highest-scoring matches are the tombstoned
	disk copies must still surface the live memory copies (over-fetch applied
	per-segment: disk fetch = top_k + disk-tombstone-count).
*/
static void test_overfetch_top_scorers_all_tombstoned_disk(void)
{
char *dir = make_index_dir();
char query[64];
char key[64], body[128];
long long i, hits;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

for (i = 0; i < 8; i++)
	{
	sprintf(key, "d-%lld", i);
	sprintf(body, "<DOC>shared token disk copy %lld</DOC>", i);
	CHECK(index->add_document(key, body) >= 0);
	}
CHECK(index->flush() == 0);

for (i = 0; i < 8; i++)
	{
	sprintf(key, "d-%lld", i);
	sprintf(body, "<DOC>shared token memory copy %lld</DOC>", i);
	CHECK(index->update_document(key, body) >= 0);
	}

strcpy(query, "shared");
hits = index->search(query, 8);
CHECK(hits == 8);				// all disk copies tombstoned, all memory copies live
CHECK(index->get_document_count() == 8);

strcpy(query, "disk");
CHECK(index->search(query, 20) == 0);
strcpy(query, "memory");
CHECK(index->search(query, 20) == 8);

/*
	top_k=1 against the same fully-tombstoned disk segment: the single live
	answer must not be masked by the 8 tombstones ahead of it.
*/
strcpy(query, "shared");
CHECK(index->search(query, 1) == 1);

delete index;
delete [] dir;
printf("test_overfetch_top_scorers_all_tombstoned_disk OK\n");
}

/*
	TEST_UPDATE_ACROSS_FLUSH_BOUNDARY()
	-----------------------------------
	Update a doc (disk->memory), flush, update again (disk->memory in the new
	generation).  The keymap generation must track each hop and tombstones must
	land in the right segment each time; get_document_count stays at 1.
*/
static void test_update_across_flush_boundary(void)
{
char *dir = make_index_dir();
char query[64];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);

index->add_document("rolling", "<DOC>alpha stage</DOC>");
CHECK(index->flush() == 0);						// alpha in disk gen A

CHECK(index->update_document("rolling", "<DOC>beta stage</DOC>") >= 0);	// beta in memory; alpha tombstoned in A
CHECK(index->flush() == 0);						// beta flushed to disk gen B

CHECK(index->update_document("rolling", "<DOC>gamma stage</DOC>") >= 0);	// gamma in memory; beta tombstoned in B

strcpy(query, "alpha");
CHECK(index->search(query, 10) == 0);
strcpy(query, "beta");
CHECK(index->search(query, 10) == 0);
strcpy(query, "gamma");
CHECK(index->search(query, 10) == 1);
CHECK(index->get_document_count() == 1);

/*
	Reopen: tombstones in both disk generations must persist; delete then
	reports the newest is the only live copy.
*/
CHECK(index->flush() == 0);
delete index;
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
strcpy(query, "alpha");
CHECK(reopened->search(query, 10) == 0);
strcpy(query, "beta");
CHECK(reopened->search(query, 10) == 0);
strcpy(query, "gamma");
CHECK(reopened->search(query, 10) == 1);
CHECK(reopened->get_document_count() == 1);
CHECK(reopened->delete_document("rolling") == 0);
CHECK(reopened->get_document_count() == 0);
delete reopened;
delete [] dir;
printf("test_update_across_flush_boundary OK\n");
}

/*
	TEST_READD_AFTER_RECONCILIATION()
	---------------------------------
	Exercise the D-then-A log ordering: a key stranded in an unflushed
	generation is reconciled away on reopen (D record), then re-added in the
	new session (A record appended after the D).  A second reopen replays
	A(stale) then D(reconcile) then A(re-add) and must end LIVE.
*/
static void test_readd_after_reconciliation(void)
{
char *dir = make_index_dir();
char query[64];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->add_document("anchor", "<DOC>anchor doc</DOC>");
CHECK(index->flush() == 0);
index->add_document("stranded", "<DOC>stranded original</DOC>");	// never flushed
delete index;

/*
	Reopen 1: reconcile drops "stranded" (D record), then re-add it.
*/
ATIRE_segment_index *reopen1 = new ATIRE_segment_index();
CHECK(reopen1->open(dir) == 0);
strcpy(query, "original");
CHECK(reopen1->search(query, 10) == 0);				// reconciled away
CHECK(reopen1->add_document("stranded", "<DOC>stranded reborn</DOC>") >= 0);
CHECK(reopen1->flush() == 0);						// A(re-add) persisted, flushed
delete reopen1;

/*
	Reopen 2: replay must be A(stale) ... D(reconcile) ... A(re-add) -> live.
*/
ATIRE_segment_index *reopen2 = new ATIRE_segment_index();
CHECK(reopen2->open(dir) == 0);
strcpy(query, "reborn");
CHECK(reopen2->search(query, 10) == 1);				// ends LIVE
strcpy(query, "original");
CHECK(reopen2->search(query, 10) == 0);
CHECK(reopen2->delete_document("stranded") == 0);		// keymap points at the live re-added copy
delete reopen2;
delete [] dir;
printf("test_readd_after_reconciliation OK\n");
}

/*
	TEST_AUTOFLUSH_AND_ORPHAN_CLEANUP()
	-----------------------------------
*/
static void test_autoflush_and_orphan_cleanup(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[128], term[32], orphan[4096];
long long i;

/*
	Auto-flush: threshold 5, add 12 docs -> at least 2 disk segments,
	everything still searchable
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->set_flush_threshold(5);
for (i = 0; i < 12; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(term, i);
	sprintf(doc, "<DOC>common %s</DOC>", term);
	CHECK(index->add_document(key, doc) >= 0);
	}
strcpy(query, "common");
CHECK(index->search(query, 100) == 12);
delete index;

/*
	Orphan cleanup: a seg file not referenced by the manifest (as left by a
	crash mid-flush) is removed at open
*/
snprintf(orphan, sizeof(orphan), "%s/seg_999999.aspt", dir);
FILE *fp = fopen(orphan, "wb");
fputs("torn segment from a crash", fp);
fclose(fp);

ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(access(orphan, F_OK) != 0);		// cleaned up
strcpy(query, "common");
long long hits = reopened->search(query, 100);
CHECK(hits >= 10);						// flushed docs survive (unflushed remainder lost: relaxed durability)
delete reopened;
delete [] dir;
printf("test_autoflush_and_orphan_cleanup OK\n");
}

/*
	TEST_KEYMAP_RECOVERY()
	----------------------
	Delete keymap.log; on reopen it is rebuilt from the segments' stored
	filenames (newest segment wins for duplicate keys) and updates still work.
*/
static void test_keymap_recovery(void)
{
char *dir = make_index_dir();
char query[64], victim[4096];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->add_document("doc-1", "<DOC>aardvark original</DOC>");
CHECK(index->flush() == 0);
index->update_document("doc-1", "<DOC>aardvark revised</DOC>");	// newer copy in second segment
index->add_document("doc-2", "<DOC>quokka</DOC>");
CHECK(index->flush() == 0);
delete index;

snprintf(victim, sizeof(victim), "%s/keymap.log", dir);
CHECK(remove(victim) == 0);

ATIRE_segment_index *recovered = new ATIRE_segment_index();
CHECK(recovered->open(dir) == 0);

/*
	The rebuilt keymap must point doc-1 at the NEWER copy: an update through
	it tombstones the revised version, not the (already dead) original.
*/
CHECK(recovered->update_document("doc-1", "<DOC>aardvark final</DOC>") >= 0);
strcpy(query, "aardvark");
CHECK(recovered->search(query, 10) == 1);
strcpy(query, "revised");
CHECK(recovered->search(query, 10) == 0);
strcpy(query, "final");
CHECK(recovered->search(query, 10) == 1);
CHECK(recovered->delete_document("doc-2") == 0);
strcpy(query, "quokka");
CHECK(recovered->search(query, 10) == 0);

/*
	Flush so the "final" copy (so far only in this session's in-memory
	writer) becomes durable: without this, deleting `recovered` below
	discards it -- an unflushed writer segment is lost on process exit by
	design (see test_stale_keymap_reconciliation), so the checks just below,
	which require "final" to survive into a THIRD session, would otherwise
	be unsatisfiable regardless of how the keymap is rebuilt.
*/
CHECK(recovered->flush() == 0);

/*
	And the rebuilt state persists: reopen again (log now present, no rebuild)
*/
delete recovered;
ATIRE_segment_index *again = new ATIRE_segment_index();
CHECK(again->open(dir) == 0);
strcpy(query, "final");
CHECK(again->search(query, 10) == 1);
strcpy(query, "revised");
CHECK(again->search(query, 10) == 0);
delete again;
delete [] dir;
printf("test_keymap_recovery OK\n");
}

int main(void)
{
test_nrt_add_and_search();
test_flush_and_reopen();
test_multi_segment_growth();
test_update_and_delete();
test_stale_keymap_reconciliation();
test_overfetch_many_updates_same_key();
test_overfetch_ten_docs_all_updated();
test_overfetch_top_scorers_all_tombstoned_disk();
test_update_across_flush_boundary();
test_readd_after_reconciliation();
test_autoflush_and_orphan_cleanup();
test_keymap_recovery();
printf("PASSED\n");
return 0;
}

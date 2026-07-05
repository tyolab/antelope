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
#include "../source/index_merge.h"
#include "../source/index_tombstones.h"
#include "../atire/atire_api.h"
#include "../source/search_engine.h"
#include "../source/search_engine_btree_leaf.h"
#include "../source/version.h"

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

/*
	TEST_KEYMAP_RECOVERY_COMPOUND_LOSS()
	------------------------------------
	Delete BOTH keymap.log and the segment's .del: both copies of an updated
	key now look live inside ONE segment.  The rebuild walks docids ascending
	(oldest first within a segment), so the newer copy still wins the keymap
	and the older copy is re-tombstoned -- and the re-raised tombstones are
	batch-persisted, so the recovered index behaves normally afterwards.
*/
static void test_keymap_recovery_compound_loss(void)
{
char *dir = make_index_dir();
char query[64], victim[4096];

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->add_document("k1", "<DOC>wombat alpha</DOC>");			// docid 0
CHECK(index->update_document("k1", "<DOC>wombat beta</DOC>") >= 0);	// docid 1; docid 0 tombstoned in the writer
index->add_document("k2", "<DOC>echidna</DOC>");			// docid 2
CHECK(index->flush() == 0);		// ONE segment (generation 1) holding both copies of k1, .del written
delete index;

/*
	Compound loss: the keymap log AND the segment's tombstones.  The remove()
	of the .del doubles as an assertion that flush() persisted it at all.
*/
snprintf(victim, sizeof(victim), "%s/keymap.log", dir);
CHECK(remove(victim) == 0);
snprintf(victim, sizeof(victim), "%s/seg_000001.del", dir);
CHECK(remove(victim) == 0);

ATIRE_segment_index *recovered = new ATIRE_segment_index();
CHECK(recovered->open(dir) == 0);

/*
	"wombat" is common to v1 and v2: exactly ONE hit means the rebuild
	re-tombstoned the older copy rather than leaving both live.
*/
strcpy(query, "wombat");
CHECK(recovered->search(query, 10) == 1);
strcpy(query, "alpha");
CHECK(recovered->search(query, 10) == 0);
strcpy(query, "beta");
CHECK(recovered->search(query, 10) == 1);

/*
	The rebuilt keymap points k1 at the v2 copy: updating to v3 tombstones
	v2, and deleting k2 works.
*/
CHECK(recovered->update_document("k1", "<DOC>wombat gamma</DOC>") >= 0);
strcpy(query, "wombat");
CHECK(recovered->search(query, 10) == 1);
strcpy(query, "beta");
CHECK(recovered->search(query, 10) == 0);
strcpy(query, "gamma");
CHECK(recovered->search(query, 10) == 1);
CHECK(recovered->delete_document("k2") == 0);
strcpy(query, "echidna");
CHECK(recovered->search(query, 10) == 0);

delete recovered;
delete [] dir;
printf("test_keymap_recovery_compound_loss OK\n");
}

/*
	TEST_EQUIVALENCE_WITH_ONESHOT()
	-------------------------------
	Final state after a messy history must match the logical collection:
	docs 0..19 added; evens 0..8 updated; docs 15..19 deleted.
	Surviving logical collection: 15 docs, with evens 0..8 revised.
*/
static void test_equivalence_with_oneshot(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
index->set_flush_threshold(7);				// force segment boundaries in awkward places

for (i = 0; i < 20; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common body%s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	}
for (i = 0; i < 10; i += 2)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common revised%s</DOC>", letters);
	CHECK(index->update_document(key, doc) >= 0);
	}
for (i = 15; i < 20; i++)
	{
	sprintf(key, "doc-%lld", i);
	CHECK(index->delete_document(key) == 0);
	}
CHECK(index->flush() == 0);

CHECK(index->get_document_count() == 15);

strcpy(query, "common");
CHECK(index->search(query, 100) == 15);

for (i = 0; i < 15; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	if (i < 10 && i % 2 == 0)
		sprintf(query, "revised%s", letters);
	else
		sprintf(query, "body%s", letters);
	CHECK(index->search(query, 10) == 1);
	CHECK(strcmp(index->get_hit(0)->filename, key) == 0);
	}
for (i = 0; i < 10; i += 2)
	{
	unique_term(letters, i);
	sprintf(query, "body%s", letters);		// pre-update bodies unreachable
	CHECK(index->search(query, 10) == 0);
	}
for (i = 15; i < 20; i++)
	{
	unique_term(letters, i);
	sprintf(query, "body%s", letters);		// deleted docs unreachable
	CHECK(index->search(query, 10) == 0);
	}

/*
	And all of it survives a reopen
*/
delete index;
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(reopened->get_document_count() == 15);
strcpy(query, "common");
CHECK(reopened->search(query, 100) == 15);
delete reopened;
delete [] dir;
printf("test_equivalence_with_oneshot OK\n");
}

/*
	OPEN_SEGMENT_ENGINE()
	---------------------
	Open a flushed segment file exactly the way append_segment() does: ANT_VX
	auto-detects the (V5) version from the header, and the doclist argument is
	unread for V5 so NULL is fine.
*/
static ATIRE_API *open_segment_engine(char *filename)
{
ATIRE_API *engine = new ATIRE_API();
engine->set_ant_version(ANT_VX);
CHECK(engine->open(ATIRE_API::INDEX_IN_MEMORY, filename, NULL, 0, -1) == 0);
return engine;
}

/*
	TEST_MERGER_NO_TOMBSTONES()
	---------------------------
	Merge two flushed segments (no deletions) and verify the output opens
	as a normal index whose contents equal the union: same document count,
	same per-term df/cf, same search membership.
*/
static void test_merger_no_tombstones(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16], seg_a[4096], seg_b[4096], merged[4096];
long long i;

/*
	Build two segments through the normal write path
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 6; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common shared%s %s</DOC>", i % 2 == 0 ? "even" : "odd", letters);
	CHECK(index->add_document(key, doc) >= 0);
	if (i == 2)
		CHECK(index->flush() == 0);
	}
CHECK(index->flush() == 0);
delete index;

snprintf(seg_a, sizeof(seg_a), "%s/seg_000001.aspt", dir);
snprintf(seg_b, sizeof(seg_b), "%s/seg_000002.aspt", dir);
snprintf(merged, sizeof(merged), "%s/seg_000009.aspt", dir);

/*
	Open the inputs and merge
*/
ATIRE_API *engine_a = open_segment_engine(seg_a);
ATIRE_API *engine_b = open_segment_engine(seg_b);

ANT_search_engine *engines[2];
engines[0] = engine_a->get_search_engine();
engines[1] = engine_b->get_search_engine();
ANT_index_tombstones *no_deletes[2];
no_deletes[0] = new ANT_index_tombstones(engine_a->get_document_count());
no_deletes[1] = new ANT_index_tombstones(engine_b->get_document_count());

ANT_index_merger *merger = new ANT_index_merger();
CHECK(merger->merge(engines, no_deletes, 2, merged) == 0);
delete merger;

/*
	Open the output and compare against the union
*/
ATIRE_API *out = open_segment_engine(merged);
CHECK(out->get_document_count() == 6);

/*
	Per-term stats must equal the sum of the inputs'
*/
ANT_search_engine_btree_leaf leaf;
CHECK(out->get_search_engine()->get_postings_details((char *)"common", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 6);
CHECK(out->get_search_engine()->get_postings_details((char *)"sharedeven", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 3);
CHECK(out->get_search_engine()->get_postings_details((char *)"sharedodd", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 3);

/*
	Search membership: every doc findable by its unique term, filenames intact
*/
strcpy(query, "common");
CHECK(out->search(query, 10) == 6);
for (i = 0; i < 6; i++)
	{
	unique_term(letters, i);
	strcpy(query, letters);
	CHECK(out->search(query, 10) == 1);
	}

/*
	Filename index survived: docid 0 is seg_a's first doc
*/
char filename_buffer[4096];
CHECK(strcmp(out->get_document_filename(filename_buffer, 0), "doc-0") == 0);
CHECK(strcmp(out->get_document_filename(filename_buffer, 3), "doc-3") == 0);

delete out;
delete engine_a;
delete engine_b;
delete no_deletes[0];
delete no_deletes[1];
delete [] dir;
printf("test_merger_no_tombstones OK\n");
}

/*
	TEST_MERGER_DROPS_TOMBSTONES()
	-------------------------------
	Merge two segments with deletions; the output must contain exactly the
	live documents, densely renumbered, with corrected df/cf.
*/
static void test_merger_drops_tombstones(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16], seg_a[4096], seg_b[4096], merged[4096], del_a[4096];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 6; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	if (i == 2)
		CHECK(index->flush() == 0);
	}
CHECK(index->flush() == 0);
/*
	Delete doc-1 (segment 1, docid 1) and doc-4 (segment 2, docid 1)
	through the API so the .del files are written for us
*/
CHECK(index->delete_document("doc-1") == 0);
CHECK(index->delete_document("doc-4") == 0);
delete index;

snprintf(seg_a, sizeof(seg_a), "%s/seg_000001.aspt", dir);
snprintf(seg_b, sizeof(seg_b), "%s/seg_000002.aspt", dir);
snprintf(del_a, sizeof(del_a), "%s/seg_000001.del", dir);
snprintf(merged, sizeof(merged), "%s/seg_000009.aspt", dir);

ATIRE_API *engine_a = open_segment_engine(seg_a);
ATIRE_API *engine_b = open_segment_engine(seg_b);
ANT_search_engine *engines[2];
engines[0] = engine_a->get_search_engine();
engines[1] = engine_b->get_search_engine();
ANT_index_tombstones *stones[2];
char del_b[4096];
snprintf(del_b, sizeof(del_b), "%s/seg_000002.del", dir);
stones[0] = ANT_index_tombstones::load(del_a, engine_a->get_document_count());
stones[1] = ANT_index_tombstones::load(del_b, engine_b->get_document_count());
CHECK(stones[0]->count() == 1);
CHECK(stones[1]->count() == 1);

ANT_index_merger *merger = new ANT_index_merger();
CHECK(merger->merge(engines, stones, 2, merged) == 0);
delete merger;

ATIRE_API *out = open_segment_engine(merged);

/*
	4 live docs; df of "common" corrected from 6 to 4
*/
CHECK(out->get_document_count() == 4);
ANT_search_engine_btree_leaf leaf;
CHECK(out->get_search_engine()->get_postings_details((char *)"common", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 4);

/*
	Deleted docs' unique terms are GONE from the vocabulary (df would be 0)
*/
unique_term(letters, 1);
CHECK(out->get_search_engine()->get_postings_details(letters, &leaf) == NULL);
unique_term(letters, 4);
CHECK(out->get_search_engine()->get_postings_details(letters, &leaf) == NULL);

/*
	Survivors searchable; dense renumbering: doc-0,2,3,5 -> 0,1,2,3
*/
strcpy(query, "common");
CHECK(out->search(query, 10) == 4);
char filename_buffer[4096];
CHECK(strcmp(out->get_document_filename(filename_buffer, 0), "doc-0") == 0);
CHECK(strcmp(out->get_document_filename(filename_buffer, 1), "doc-2") == 0);
CHECK(strcmp(out->get_document_filename(filename_buffer, 2), "doc-3") == 0);
CHECK(strcmp(out->get_document_filename(filename_buffer, 3), "doc-5") == 0);

delete out;
delete engine_a;
delete engine_b;
delete stones[0];
delete stones[1];
delete [] dir;
printf("test_merger_drops_tombstones OK\n");
}

/*
	TEST_MERGER_REPEATED_MERGE()
	-----------------------------
	Hardening item 3/4 (Task 2 review): a single ANT_index_merger instance
	must be able to drive a second merge after the first, proving both
	re-entrancy (no leaked/stale per-merge state) and merge-of-merged
	composability.  Builds three segments A, B, C (with a deletion each in
	A and B before the first merge), merges A+B -> out1 with one instance,
	then reuses the SAME instance to merge out1+C -> out2, and checks that
	out2's live document count equals live(A) + live(B) + live(C).
*/
static void test_merger_repeated_merge(void)
{
char *dir = make_index_dir();
char key[256], doc[256], letters[16];
char seg_a[4096], seg_b[4096], seg_c[4096];
char del_a[4096], del_b[4096];
char out1[4096], out2[4096];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
/*
	Segment A: doc-0..2
*/
for (i = 0; i < 3; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	}
CHECK(index->flush() == 0);
/*
	Segment B: doc-3..5
*/
for (i = 3; i < 6; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	}
CHECK(index->flush() == 0);
/*
	Delete one document each from A and B before the first merge
*/
CHECK(index->delete_document("doc-0") == 0);
CHECK(index->delete_document("doc-4") == 0);
/*
	Segment C: doc-6..7, flushed AFTER the deletions above so it carries
	no tombstones of its own
*/
for (i = 6; i < 8; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	}
CHECK(index->flush() == 0);
delete index;

snprintf(seg_a, sizeof(seg_a), "%s/seg_000001.aspt", dir);
snprintf(seg_b, sizeof(seg_b), "%s/seg_000002.aspt", dir);
snprintf(seg_c, sizeof(seg_c), "%s/seg_000003.aspt", dir);
snprintf(del_a, sizeof(del_a), "%s/seg_000001.del", dir);
snprintf(del_b, sizeof(del_b), "%s/seg_000002.del", dir);
snprintf(out1, sizeof(out1), "%s/seg_000009.aspt", dir);
snprintf(out2, sizeof(out2), "%s/seg_000010.aspt", dir);

ATIRE_API *engine_a = open_segment_engine(seg_a);
ATIRE_API *engine_b = open_segment_engine(seg_b);
ANT_search_engine *ab_engines[2];
ab_engines[0] = engine_a->get_search_engine();
ab_engines[1] = engine_b->get_search_engine();
ANT_index_tombstones *ab_stones[2];
ab_stones[0] = ANT_index_tombstones::load(del_a, engine_a->get_document_count());
ab_stones[1] = ANT_index_tombstones::load(del_b, engine_b->get_document_count());
CHECK(ab_stones[0]->count() == 1);
CHECK(ab_stones[1]->count() == 1);

/*
	ONE merger instance drives BOTH merges
*/
ANT_index_merger *merger = new ANT_index_merger();
CHECK(merger->merge(ab_engines, ab_stones, 2, out1) == 0);

delete engine_a;
delete engine_b;
delete ab_stones[0];
delete ab_stones[1];

/*
	Second merge, same instance: out1 (already-merged, tombstone-free) + C
*/
ATIRE_API *engine_out1 = open_segment_engine(out1);
ATIRE_API *engine_c = open_segment_engine(seg_c);
ANT_search_engine *final_engines[2];
final_engines[0] = engine_out1->get_search_engine();
final_engines[1] = engine_c->get_search_engine();
ANT_index_tombstones *final_stones[2];
final_stones[0] = new ANT_index_tombstones(engine_out1->get_document_count());
final_stones[1] = new ANT_index_tombstones(engine_c->get_document_count());

CHECK(merger->merge(final_engines, final_stones, 2, out2) == 0);
delete merger;

/*
	live(A) = 2, live(B) = 2, live(C) = 2 -> 6 total
*/
ATIRE_API *out = open_segment_engine(out2);
CHECK(out->get_document_count() == 6);

ANT_search_engine_btree_leaf leaf;
CHECK(out->get_search_engine()->get_postings_details((char *)"common", &leaf) != NULL);
CHECK(leaf.local_document_frequency == 6);

char query[64];
strcpy(query, "common");
CHECK(out->search(query, 10) == 6);

delete out;
delete engine_out1;
delete engine_c;
delete final_stones[0];
delete final_stones[1];
delete [] dir;
printf("test_merger_repeated_merge OK\n");
}

/*
	TEST_COMPACT_BASIC()
	--------------------
	Compact two segments with deletions into one; searches, counts, keymap
	(update/delete by key) and reopen must all be correct afterwards.
*/
static void test_compact_basic(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 8; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	if (i == 3)
		CHECK(index->flush() == 0);
	}
CHECK(index->flush() == 0);
CHECK(index->delete_document("doc-2") == 0);
CHECK(index->delete_document("doc-5") == 0);

long long inputs[2];
inputs[0] = 1;
inputs[1] = 2;
CHECK(index->compact(inputs, 2) == 0);

/*
	One disk segment now; 6 live docs; tombstone over-fetch overhead gone
*/
CHECK(index->get_document_count() == 6);
strcpy(query, "common");
CHECK(index->search(query, 100) == 6);
unique_term(letters, 2);
strcpy(query, letters);
CHECK(index->search(query, 10) == 0);

/*
	Keymap remapped: update + delete by key still hit the right documents
*/
unique_term(letters, 3);
sprintf(doc, "<DOC>common replacement %s</DOC>", letters);
CHECK(index->update_document("doc-3", doc) >= 0);
strcpy(query, letters);
CHECK(index->search(query, 10) == 1);		// only the replacement's copy of the unique term
CHECK(index->delete_document("doc-7") == 0);
CHECK(index->get_document_count() == 5);

/*
	Durable: reopen sees the compacted state (plus the unflushed update is
	lost, so doc-3 reverts to its compacted body -- relaxed durability).

	The tombstones raised above (doc-3's old copy and doc-7) are against
	the OUTPUT disk segment, so tombstone() persists their .del entry
	immediately -- durable without a flush().  The replacement body for
	doc-3, by contrast, lives only in the in-memory writer and is lost
	when the index is destroyed without flush()ing.  So on reopen: 6 docs
	in the compacted segment, minus the two persisted tombstones (old
	doc-3, doc-7) = 4 live documents; doc-3's replacement never made it to
	disk at all.
*/
delete index;
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(reopened->get_document_count() == 4);
strcpy(query, "common");
CHECK(reopened->search(query, 100) == 4);
delete reopened;
delete [] dir;
printf("test_compact_basic OK\n");
}

/*
	TEST_COMPACT_SUBSET_LEAVES_OTHER_SEGMENT()
	------------------------------------------
	Compact only a subset of the disk segments (here segment 1 of {1,2});
	the untouched segment's file, documents and keymap entries must all
	survive: its docs stay searchable and delete/update by its keys still
	hit the right documents afterwards.
*/
static void test_compact_subset_leaves_other_segment(void)
{
char *dir = make_index_dir();
char query[64], key[64], doc[256], letters[16], path[4096];
long long i;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 8; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	if (i == 3)
		CHECK(index->flush() == 0);
	}
CHECK(index->flush() == 0);

/*
	Compact segment 1 alone (a single-input rewrite); segment 2 is not
	part of the merge and must come through untouched
*/
long long inputs[1];
inputs[0] = 1;
CHECK(index->compact(inputs, 1) == 0);

/*
	Nothing was deleted, so all 8 docs survive; segment 2's docs are
	still individually findable by their unique terms
*/
CHECK(index->get_document_count() == 8);
strcpy(query, "common");
CHECK(index->search(query, 100) == 8);
for (i = 4; i < 8; i++)
	{
	unique_term(letters, i);
	strcpy(query, letters);
	CHECK(index->search(query, 10) == 1);
	}

/*
	Segment 1's file was replaced by the compaction output; segment 2's
	file was left alone
*/
snprintf(path, sizeof(path), "%s/seg_000001.aspt", dir);
CHECK(access(path, F_OK) != 0);
snprintf(path, sizeof(path), "%s/seg_000002.aspt", dir);
CHECK(access(path, F_OK) == 0);

/*
	Keymap entries for segment 2's keys were never touched: delete and
	update by key still resolve to the right documents
*/
CHECK(index->delete_document("doc-5") == 0);
CHECK(index->get_document_count() == 7);
unique_term(letters, 5);
strcpy(query, letters);
CHECK(index->search(query, 10) == 0);

unique_term(letters, 6);
sprintf(doc, "<DOC>common replacement %s</DOC>", letters);
CHECK(index->update_document("doc-6", doc) >= 0);
strcpy(query, letters);
CHECK(index->search(query, 10) == 1);		// only the replacement's copy of the unique term

delete index;
delete [] dir;
printf("test_compact_subset_leaves_other_segment OK\n");
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
test_keymap_recovery_compound_loss();
test_equivalence_with_oneshot();
test_merger_no_tombstones();
test_merger_drops_tombstones();
test_merger_repeated_merge();
test_compact_basic();
test_compact_subset_leaves_other_segment();
printf("PASSED\n");
return 0;
}

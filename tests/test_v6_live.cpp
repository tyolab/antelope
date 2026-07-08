/*
	TEST_V6_LIVE.CPP
	-----------------
	Task 8 of the V6 plan: merges the un-flushed live writer buffer into
	multivector_candidates() (search_multivector()'s candidate gatherer).
	Before this task, docs added but not yet flush()ed were invisible to
	search_multivector() -- only disk segments were scanned.  This locks:

	  1. Live multi-vector docs are merged with disk docs into ONE globally
	     ranked result (not just appended after disk hits) -- a live doc with
	     the single highest MaxSim score across the whole corpus must come
	     back as hit 0, ahead of every disk doc.
	  2. Filtered search_multivector() applies the filter to live docs too:
	     a live doc failing the filter is excluded, a live doc passing it is
	     included, mirroring the already-tested disk-filter behaviour.
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
strcpy(buffer, "/tmp/ant_v6_live_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	TEST_LIVE_MERGE_INTERLEAVES_BY_SCORE()
	-----------------------------------------
	3 flushed (disk) docs + 2 un-flushed (live) docs, all single-token, query
	q=(1,0,0,0) so MaxSim reduces to a plain normalized dot product.  Dots
	(hand-computable, all gaps large so writer normalize() rounding can't
	flip order):

		disk_d0 = (0.9,0.1,0,0)   normalized (0.99388,0.11043,0,0)  dot = 0.99388
		disk_d1 = (0.5,0.5,0,0)   normalized (0.70711,0.70711,0,0)  dot = 0.70711
		disk_d2 = (0.2,0,0,0.98)  normalized (0.19996,0,0,0.97980)  dot = 0.19996
		live_max = (1,0,0,0)      normalized (1,0,0,0)              dot = 1.0        <- GLOBAL max, un-flushed
		live_mid = (0.6,0.4,0,0)  normalized (0.83205,0.55470,0,0)  dot = 0.83205    <- un-flushed, interleaves
		                                                                                between disk_d0 and disk_d1

	Expected global order: live_max (1.0), disk_d0 (0.99388), live_mid
	(0.83205), disk_d1 (0.70711), disk_d2 (0.19996).  If the live docs were
	merely appended after disk results instead of globally ranked, live_max
	would NOT be hit 0 and live_mid would NOT sit between disk_d0/disk_d1 --
	this is the assertion that catches that bug.
*/
static void test_live_merge_interleaves_by_score(void)
{
char *dir = make_index_dir();
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

float disk_d0[4] = {0.9f, 0.1f, 0.0f, 0.0f};
float disk_d1[4] = {0.5f, 0.5f, 0.0f, 0.0f};
float disk_d2[4] = {0.2f, 0.0f, 0.0f, 0.98f};

CHECK(ix->add_document("disk_d0", "<DOC>disk_d0</DOC>", NULL, disk_d0, 1) >= 0);
CHECK(ix->add_document("disk_d1", "<DOC>disk_d1</DOC>", NULL, disk_d1, 1) >= 0);
CHECK(ix->add_document("disk_d2", "<DOC>disk_d2</DOC>", NULL, disk_d2, 1) >= 0);
CHECK(ix->flush() == 0);		// disk_d0/d1/d2 now on disk

float live_max[4] = {1.0f, 0.0f, 0.0f, 0.0f};
float live_mid[4] = {0.6f, 0.4f, 0.0f, 0.0f};

CHECK(ix->add_document("live_max", "<DOC>live_max</DOC>", NULL, live_max, 1) >= 0);
CHECK(ix->add_document("live_mid", "<DOC>live_mid</DOC>", NULL, live_mid, 1) >= 0);
/* NOT flushed -- these two live only in the writer's memory buffer */

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
long long n = ix->search_multivector(q, 1, 10);
CHECK(n == 5);			// 3 disk + 2 live

CHECK(strcmp(ix->get_hit(0)->filename, "live_max") == 0);
CHECK(strcmp(ix->get_hit(1)->filename, "disk_d0") == 0);
CHECK(strcmp(ix->get_hit(2)->filename, "live_mid") == 0);
CHECK(strcmp(ix->get_hit(3)->filename, "disk_d1") == 0);
CHECK(strcmp(ix->get_hit(4)->filename, "disk_d2") == 0);

CHECK(ix->get_hit(0)->score >= ix->get_hit(1)->score);
CHECK(ix->get_hit(1)->score >= ix->get_hit(2)->score);
CHECK(ix->get_hit(2)->score >= ix->get_hit(3)->score);
CHECK(ix->get_hit(3)->score >= ix->get_hit(4)->score);

delete ix;
delete [] dir;
printf("test_live_merge_interleaves_by_score OK\n");
}

/*
	TEST_LIVE_MERGE_FILTERED()
	------------------------------
	Flushed + un-flushed docs tagged with a "tenant" attribute; filtering
	tenant=="acme" must admit the un-flushed acme doc and exclude the
	un-flushed beta doc (mirroring test_v6_filter.cpp's disk-only guarantee,
	extended across the live buffer).
*/
static void test_live_merge_filtered(void)
{
char *dir = make_index_dir();
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

ANT_attribute_schema s;
s.add_field("tenant", ANT_attribute_schema::TYPE_STRING, 0);
CHECK(ix->set_attributes_config(s) == 0);

float acme_f0[4] = {1.0f, 0.0f, 0.0f, 0.0f};		// dot = 1.0
float beta_f1[4] = {0.9f, 0.1f, 0.0f, 0.0f};		// dot = 0.99388

{
ANT_attribute_set A0(ix->attribute_schema());
A0.set_string(0, "acme");
CHECK(ix->add_document("acme_f0", "<DOC>acme_f0</DOC>", NULL, acme_f0, 1, &A0) >= 0);

ANT_attribute_set A1(ix->attribute_schema());
A1.set_string(0, "beta");
CHECK(ix->add_document("beta_f1", "<DOC>beta_f1</DOC>", NULL, beta_f1, 1, &A1) >= 0);
}
CHECK(ix->flush() == 0);		// acme_f0/beta_f1 now on disk

float acme_L[4] = {0.5f, 0.5f, 0.0f, 0.0f};		// dot = 0.70711
float beta_L[4] = {0.8f, 0.2f, 0.0f, 0.0f};		// dot = 0.97014

{
ANT_attribute_set A2(ix->attribute_schema());
A2.set_string(0, "acme");
CHECK(ix->add_document("acme_L", "<DOC>acme_L</DOC>", NULL, acme_L, 1, &A2) >= 0);

ANT_attribute_set A3(ix->attribute_schema());
A3.set_string(0, "beta");
CHECK(ix->add_document("beta_L", "<DOC>beta_L</DOC>", NULL, beta_L, 1, &A3) >= 0);
}
/* acme_L/beta_L NOT flushed -- live buffer only */

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
ANT_filter *f = ANT_filter::eq_string("tenant", "acme");
CHECK(f->build(ix->attribute_schema()) == 0);

long long n = ix->search_multivector(q, 1, 10, f);
CHECK(n == 2);			// acme_f0 (disk) + acme_L (live) only

bool saw_acme_L = false, saw_beta_L = false, saw_beta_f1 = false;
for (long long i = 0; i < n; i++)
	{
	const char *fn = ix->get_hit(i)->filename;
	CHECK(strncmp(fn, "acme_", 5) == 0);		// no beta_* leaks through
	if (strcmp(fn, "acme_L") == 0)
		saw_acme_L = true;
	if (strcmp(fn, "beta_L") == 0)
		saw_beta_L = true;
	if (strcmp(fn, "beta_f1") == 0)
		saw_beta_f1 = true;
	}
CHECK(saw_acme_L);			// live acme doc DOES appear
CHECK(!saw_beta_L);			// live beta doc does NOT appear
CHECK(!saw_beta_f1);			// disk beta doc does NOT appear

/* ranked order within the admitted set: acme_f0 (1.0) before acme_L (0.70711) */
CHECK(strcmp(ix->get_hit(0)->filename, "acme_f0") == 0);
CHECK(strcmp(ix->get_hit(1)->filename, "acme_L") == 0);

delete f;
delete ix;
delete [] dir;
printf("test_live_merge_filtered OK\n");
}

int main(void)
{
test_live_merge_interleaves_by_score();
test_live_merge_filtered();
printf("ALL TESTS PASSED\n");
return 0;
}

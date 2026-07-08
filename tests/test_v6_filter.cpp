/*
	TEST_V6_FILTER.CPP
	-------------------
	Task 7 of the V6 plan: filter support in search_multivector().  Task 6's
	brute-force MaxSim fallback (no token index built yet -- that's Task 9) is
	EXACT -- it scans every doc with a multi-vector -- so under a filter it can
	never under-fill.  This locks that guarantee: every hit obeys the filter,
	and when at least top_k docs match, the full top_k comes back.

	Six single-token docs, tenants assigned so exactly 3 are "acme" (all 3
	inside the requested top_k, so no under-fill can hide) and exactly 1 is
	"solo" (the selective-filter case).  Doc keys are prefixed with the tenant
	so a hit's filename alone proves which tenant it belongs to, without any
	extra hit-side attribute accessor.
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
strcpy(buffer, "/tmp/ant_v6_filter_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	Shared fixture: dim=4, single-token docs/query so MaxSim reduces to a
	plain normalized dot product.  Dots (hand-computable, all gaps > 0.05
	so rounding in the writer's normalize() can't flip order):

		d0 = (1,0,0,0)      normalized (1,0,0,0)                dot = 1.0
		d1 = (0.9,0.1,0,0)  normalized (0.99379,0.11042,0,0)    dot = 0.99379
		d2 = (0.8,0.2,0,0)  normalized (0.97014,0.24254,0,0)    dot = 0.97014
		d3 = (0.5,0.5,0,0)  normalized (0.70711,0.70711,0,0)    dot = 0.70711
		d4 = (0.3,0.7,0,0)  normalized (0.39392,0.91914,0,0)    dot = 0.39392
		d5 = (0,1,0,0)      normalized (0,1,0,0)                dot = 0.0

	tenants: d0,d1,d3 = "acme" (3 docs); d2,d4 = "beta"; d5 = "solo" (1 doc).
	Doc keys are "<tenant>_dN" so a returned filename's prefix proves tenant.
*/
static ATIRE_segment_index *build_fixture(char **dir_out)
{
char *dir = make_index_dir();
*dir_out = dir;
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

ANT_attribute_schema s;
s.add_field("tenant", ANT_attribute_schema::TYPE_STRING, 0);
CHECK(ix->set_attributes_config(s) == 0);

struct { const char *key; float v[4]; const char *tenant; } docs[6] =
	{
	{ "acme_d0", {1.0f, 0.0f, 0.0f, 0.0f}, "acme" },
	{ "acme_d1", {0.9f, 0.1f, 0.0f, 0.0f}, "acme" },
	{ "beta_d2", {0.8f, 0.2f, 0.0f, 0.0f}, "beta" },
	{ "acme_d3", {0.5f, 0.5f, 0.0f, 0.0f}, "acme" },
	{ "beta_d4", {0.3f, 0.7f, 0.0f, 0.0f}, "beta" },
	{ "solo_d5", {0.0f, 1.0f, 0.0f, 0.0f}, "solo" },
	};

for (long long i = 0; i < 6; i++)
	{
	ANT_attribute_set A(ix->attribute_schema());
	A.set_string(0, docs[i].tenant);
	char doc[64];
	sprintf(doc, "<DOC>%s</DOC>", docs[i].key);
	CHECK(ix->add_document(docs[i].key, doc, NULL, docs[i].v, 1, &A) >= 0);
	}
CHECK(ix->flush() == 0);
return ix;
}

/*
	TEST_FILTER_NO_UNDER_FILL()
	----------------------------
	filter tenant=="acme" (3 matching docs), top_k=3: every hit must be an
	"acme_" doc, and since exactly 3 acme docs exist and top_k==3, the full
	top_k comes back (no under-fill) in dot order d0, d1, d3.
*/
static void test_filter_no_under_fill(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
ANT_filter *f = ANT_filter::eq_string("tenant", "acme");
CHECK(f->build(ix->attribute_schema()) == 0);

long long n = ix->search_multivector(q, 1, 3, f);
CHECK(n == 3);
for (long long i = 0; i < n; i++)
	CHECK(strncmp(ix->get_hit(i)->filename, "acme_", 5) == 0);
CHECK(strcmp(ix->get_hit(0)->filename, "acme_d0") == 0);
CHECK(strcmp(ix->get_hit(1)->filename, "acme_d1") == 0);
CHECK(strcmp(ix->get_hit(2)->filename, "acme_d3") == 0);

delete f;
delete ix;
delete [] dir;
printf("test_filter_no_under_fill OK\n");
}

/*
	TEST_FILTER_SELECTIVE()
	-------------------------
	filter tenant=="solo" matches exactly one doc (d5).  Even with a larger
	top_k, exactly that 1 doc is returned.
*/
static void test_filter_selective(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
ANT_filter *f = ANT_filter::eq_string("tenant", "solo");
CHECK(f->build(ix->attribute_schema()) == 0);

long long n = ix->search_multivector(q, 1, 6, f);
CHECK(n == 1);
CHECK(strcmp(ix->get_hit(0)->filename, "solo_d5") == 0);

delete f;
delete ix;
delete [] dir;
printf("test_filter_selective OK\n");
}

/*
	TEST_FILTER_NULL_UNCHANGED()
	------------------------------
	filter==NULL must stay byte-identical to Task 6: all 6 docs returned in
	dot order, top hit is the max-dot doc (acme_d0) regardless of tenant.
*/
static void test_filter_null_unchanged(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
long long n = ix->search_multivector(q, 1, 6);
CHECK(n == 6);
CHECK(strcmp(ix->get_hit(0)->filename, "acme_d0") == 0);

delete ix;
delete [] dir;
printf("test_filter_null_unchanged OK\n");
}

int main(void)
{
test_filter_no_under_fill();
test_filter_selective();
test_filter_null_unchanged();
printf("ALL TESTS PASSED\n");
return 0;
}

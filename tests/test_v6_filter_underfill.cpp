/*
	TEST_V6_FILTER_UNDERFILL.CPP -- #16: filtered search_multivector must not
	under-fill a .tann segment. 40 "beta" docs cluster at the query direction
	and 3 "acme" docs sit orthogonal to it; with candidate_multiplier=1 the
	token-ANN nearest-32 set is entirely beta, so the token path alone admits
	zero acme under a tenant=="acme" filter. The brute-force top-up must still
	return the full top_k=3 of acme docs -- the same result the no-.tann
	(brute-force) path yields.
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
strcpy(buffer, "/tmp/ant_v6_underfill_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

static ATIRE_segment_index *build_fixture(char **dir_out)
{
char *dir = make_index_dir();
*dir_out = dir;
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
ix->set_candidate_multiplier(1);   // filtered eff_top_p = token_top_p(32)*1 -> 32 nearest tokens

ANT_attribute_schema s;
s.add_field("tenant", ANT_attribute_schema::TYPE_STRING, 0);
CHECK(ix->set_attributes_config(s) == 0);

// 40 beta docs clustered near (1,0,0,0); tiny distinct perturbations keep them the nearest tokens
for (int i = 0; i < 40; i++)
	{
	float v[4] = { 1.0f, 0.001f * (float)i, 0.0f, 0.0f };
	char key[32]; sprintf(key, "beta_%02d", i);
	char doc[64]; sprintf(doc, "<DOC>%s</DOC>", key);
	ANT_attribute_set A(ix->attribute_schema());
	A.set_string(0, "beta");
	CHECK(ix->add_document(key, doc, NULL, v, 1, &A) >= 0);
	}
// 3 acme docs at the orthogonal direction (0,1,0,0) -> far from the query, never in the nearest-32
struct { const char *key; float v[4]; } acme[3] =
	{
	{ "acme_a", {0.0f, 1.0f,  0.0f,   0.0f} },
	{ "acme_b", {0.0f, 0.99f, 0.10f,  0.0f} },
	{ "acme_c", {0.0f, 0.99f, 0.0f,   0.10f} },
	};
for (int i = 0; i < 3; i++)
	{
	char doc[64]; sprintf(doc, "<DOC>%s</DOC>", acme[i].key);
	ANT_attribute_set A(ix->attribute_schema());
	A.set_string(0, "acme");
	CHECK(ix->add_document(acme[i].key, doc, NULL, acme[i].v, 1, &A) >= 0);
	}
CHECK(ix->flush() == 0);
CHECK(ix->build_token_index() == 0);   // build .tann -> token-ANN path active for this segment
return ix;
}

static void test_selective_filter_no_under_fill(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
ANT_filter *f = ANT_filter::eq_string("tenant", "acme");
CHECK(f->build(ix->attribute_schema()) == 0);

// 3 acme docs match; token-ANN alone returns 0 (all nearest-32 are beta) -> top-up must fill to 3
long long n = ix->search_multivector(q, 1, 3, f);
CHECK(n == 3);
for (long long i = 0; i < n; i++)
	CHECK(strncmp(ix->get_hit(i)->filename, "acme_", 5) == 0);

delete f;
delete ix;
delete [] dir;
printf("test_selective_filter_no_under_fill OK\n");
}

static void test_unfiltered_ann_unchanged(void)
{
char *dir;
ATIRE_segment_index *ix = build_fixture(&dir);

// unfiltered small top_k: pure ANN path (no top-up, fbits==NULL) still returns top hits, nearest first
float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
long long n = ix->search_multivector(q, 1, 5);
CHECK(n == 5);
for (long long i = 0; i < n; i++)
	CHECK(strncmp(ix->get_hit(i)->filename, "beta_", 5) == 0);   // all 5 nearest are beta
CHECK(strcmp(ix->get_hit(0)->filename, "beta_00") == 0);         // exact (1,0,0,0) is the max-dot doc

delete ix;
delete [] dir;
printf("test_unfiltered_ann_unchanged OK\n");
}

int main(void)
{
test_selective_filter_no_under_fill();
test_unfiltered_ann_unchanged();
printf("ALL TESTS PASSED\n");
return 0;
}

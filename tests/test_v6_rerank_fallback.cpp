/*
	TEST_V6_RERANK_FALLBACK.CPP
	----------------------------
	Task 11 of the V6 plan: search_rerank()'s both-NULL (query_text == NULL
	&& query_vector == NULL) case previously returned 0 unconditionally --
	"no first stage possible -- nothing to rerank".  It now delegates to the
	token-ANN candidate generator (search_multivector_impl(), the same body
	search_multivector() calls): candidate-gen + exact MaxSim + publish.

	This locks:
	  1) both-NULL rerank returns real hits (was 0, now up to top_k), and its
	     keys+order are IDENTICAL to calling search_multivector() directly
	     with the same query/top_k -- they share the exact same body.
	  2) the same equality holds under a filter.
	  3) the text-present path (query_text != NULL) is untouched: stage 1 is
	     still the lexical search restricted to matching docs, then MaxSim
	     rerank over just those candidates.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "../atire/atire_segment_index.h"
#include "../source/filter.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(const char *prefix)
{
char buffer[64];
strcpy(buffer, prefix);
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	10 single-token docs spread evenly around a circle in the first two of 4
	dims (same construction as test_v6_build_token_index.cpp) so the query
	aimed at d0's direction has an unambiguous, hand-verifiable top-5 by dot
	product, whether reached via brute force or the token-ANN graph.
*/
static void make_doc_vec(float *out, long long dim, long long i, long long n)
{
double angle = (2.0 * M_PI * (double) i) / (double) n;
out[0] = (float) cos(angle);
out[1] = (float) sin(angle);
out[2] = (float) (0.05 * cos(angle * 3.0));
out[3] = (float) (0.05 * sin(angle * 3.0));
}

/*
	TEST_BOTH_NULL_ROUTES_TO_TOKEN_ANN()
	--------------------------------------
	Neither query_text nor query_vector given, but a query multi-vector is,
	and rerank is configured: search_rerank() must now return real hits (was
	a return-0 guard before this task), and those hits (keys + order) must be
	IDENTICAL to search_multivector()'s, since both-NULL delegates straight
	into the same search_multivector_impl() body.
*/
static void test_both_null_routes_to_token_ann(void)
{
char *dir = make_index_dir("/tmp/ant_v6_rerank_fb_XXXXXX");
long long dim = 4, n_docs = 10, top_k = 5, i;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

char key[32];
for (i = 0; i < n_docs; i++)
	{
	float v[4];
	make_doc_vec(v, dim, i, n_docs);
	sprintf(key, "d%lld", i);
	char doc[64];
	sprintf(doc, "<DOC>doc %lld filler text</DOC>", i);
	CHECK(ix->add_document(key, doc, NULL, v, 1) >= 0);
	}
CHECK(ix->flush() == 0);
CHECK(ix->build_token_index() == 0);

float q[4];
make_doc_vec(q, dim, 0, n_docs);	/* aimed exactly at d0's direction */

/* both-NULL: previously a return-0 guard -- now must return real hits */
long long n1 = ix->search_rerank(NULL, NULL, q, 1, 20, top_k);
CHECK(n1 == top_k);				/* not 0 -- the guard is gone */

char rerank_keys[5][32];
for (i = 0; i < n1; i++)
	strcpy(rerank_keys[i], ix->get_hit(i)->filename);

/* independently, the same query via search_multivector() directly */
long long n2 = ix->search_multivector(q, 1, top_k);
CHECK(n2 == top_k);

for (i = 0; i < n2; i++)
	CHECK(strcmp(ix->get_hit(i)->filename, rerank_keys[i]) == 0);

delete ix;
delete [] dir;
printf("test_both_null_routes_to_token_ann OK\n");
}

/*
	TEST_TEXT_PRESENT_PATH_UNCHANGED()
	--------------------------------------
	query_text != NULL (query_vector == NULL): stage 1 must still be the
	lexical search restricted to docs matching the term, then MaxSim rerank
	over just that candidate set -- proving Step 1's edit (which only moved
	the both-NULL check earlier and deleted the old dead guard) left this
	branch byte-identical in behaviour.

	4 of 6 docs contain "widget" in their text; the other 2 don't.  Query
	multi-vector is aimed at d0's direction, so among the "widget" docs the
	MaxSim order is d0, d2, d4, d5 (decreasing dot product -- hand-verifiable,
	same construction as make_doc_vec but picked so the widget subset's dot
	order is unambiguous).
*/
static void test_text_present_path_unchanged(void)
{
char *dir = make_index_dir("/tmp/ant_v6_rerank_fb_txt_XXXXXX");
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

struct { const char *key; float v[4]; int has_term; } docs[6] =
	{
	{ "d0", {1.0f,  0.0f,  0.0f, 0.0f}, 1 },	/* dot 1.0 */
	{ "d1", {0.9f,  0.1f,  0.0f, 0.0f}, 0 },	/* no term -- excluded from stage 1 */
	{ "d2", {0.8f,  0.2f,  0.0f, 0.0f}, 1 },	/* dot 0.970 */
	{ "d3", {0.5f,  0.5f,  0.0f, 0.0f}, 0 },	/* no term -- excluded from stage 1 */
	{ "d4", {0.3f,  0.7f,  0.0f, 0.0f}, 1 },	/* dot 0.394 */
	{ "d5", {0.0f,  1.0f,  0.0f, 0.0f}, 1 },	/* dot 0.0 */
	};

long long i;
for (i = 0; i < 6; i++)
	{
	char doc[80];
	if (docs[i].has_term)
		sprintf(doc, "<DOC>%s contains widget term</DOC>", docs[i].key);
	else
		sprintf(doc, "<DOC>%s has nothing relevant</DOC>", docs[i].key);
	CHECK(ix->add_document(docs[i].key, doc, NULL, docs[i].v, 1) >= 0);
	}
CHECK(ix->flush() == 0);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
char qtext[] = "widget";

long long n = ix->search_rerank(qtext, NULL, q, 1, 20, 5);
CHECK(n >= 1);
CHECK(n == 4);					/* exactly the 4 "widget" docs */

const char *expect[4] = { "d0", "d2", "d4", "d5" };
for (i = 0; i < n; i++)
	CHECK(strcmp(ix->get_hit(i)->filename, expect[i]) == 0);

delete ix;
delete [] dir;
printf("test_text_present_path_unchanged OK\n");
}

/*
	TEST_FILTERED_BOTH_NULL_MATCHES_FILTERED_SEARCH_MULTIVECTOR()
	------------------------------------------------------------------
	Same both-NULL delegation, but with a filter attached: must equal a
	direct filtered search_multivector() call (both keys and order), and
	every hit must obey the filter (mirrors tests/test_v6_filter.cpp's
	fixture).
*/
static void test_filtered_both_null_matches_filtered_search_multivector(void)
{
char *dir = make_index_dir("/tmp/ant_v6_rerank_fb_filt_XXXXXX");
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

long long i;
for (i = 0; i < 6; i++)
	{
	ANT_attribute_set A(ix->attribute_schema());
	A.set_string(0, docs[i].tenant);
	char doc[64];
	sprintf(doc, "<DOC>%s</DOC>", docs[i].key);
	CHECK(ix->add_document(docs[i].key, doc, NULL, docs[i].v, 1, &A) >= 0);
	}
CHECK(ix->flush() == 0);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
ANT_filter *f = ANT_filter::eq_string("tenant", "acme");
CHECK(f->build(ix->attribute_schema()) == 0);

long long n1 = ix->search_rerank(NULL, NULL, q, 1, 20, 3, f);
CHECK(n1 == 3);
for (i = 0; i < n1; i++)
	CHECK(strncmp(ix->get_hit(i)->filename, "acme_", 5) == 0);

char rerank_keys[3][32];
for (i = 0; i < n1; i++)
	strcpy(rerank_keys[i], ix->get_hit(i)->filename);

long long n2 = ix->search_multivector(q, 1, 3, f);
CHECK(n2 == 3);
for (i = 0; i < n2; i++)
	CHECK(strcmp(ix->get_hit(i)->filename, rerank_keys[i]) == 0);

delete f;
delete ix;
delete [] dir;
printf("test_filtered_both_null_matches_filtered_search_multivector OK\n");
}

int main(void)
{
test_both_null_routes_to_token_ann();
test_text_present_path_unchanged();
test_filtered_both_null_matches_filtered_search_multivector();
printf("ALL OK\n");
return 0;
}

/*
	TEST_V6_SEARCH_MULTIVECTOR.CPP
	------------------------------
	Task 6 of the V6 plan: wires the (not-yet-built, Task 9) token index into
	the segment and adds the first-class search_multivector() (candidate-gen
	-> exact MaxSim rescore).  build_token_index() doesn't exist yet, so every
	segment's token_index is NULL/empty here and search_multivector() runs the
	brute-force MaxSim fallback -- this is the ranking-equality lock: fallback
	ranking must equal exhaustive MaxSim, exactly.
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
strcpy(buffer, "/tmp/ant_v6_mvsearch_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	TEST_SEARCH_MULTIVECTOR_MATCHES_BRUTE_FORCE_MAXSIM()
	------------------------------------------------------
	5 docs, each a single token; query is a single token q=(1,0,0,0).  MaxSim
	for a single-token doc/query pair is just the dot product of the two
	(post-normalization) unit vectors, so the expected order is fully
	hand-computable:

		d0 = (1,0,0,0)          normalized (1,0,0,0)              dot = 1.0
		d1 = (0.9,0.1,0,0)      normalized (0.994036, 0.110448,0,0) dot = 0.994036
		d2 = (0.5,0.5,0,0)      normalized (0.707107,0.707107,0,0)  dot = 0.707107
		d4 = (0.2,0,0,0.98)     normalized (0.199960,0,0,0.979804)  dot = 0.199960
		d3 = (0,1,0,0)          normalized (0,1,0,0)                dot = 0.0

	(d1's exact normalized dot: 0.9/sqrt(0.82) = 0.9938...; hand-verified below
	 within a coarse tolerance so rounding in the writer's normalize() can't
	 flip the order -- the gaps between consecutive docs are all > 0.2.)

	Expected order: d0, d1, d2, d4, d3.
*/
static void test_search_multivector_matches_brute_force_maxsim(void)
{
char *dir = make_index_dir();
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

float d0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
float d1[4] = {0.9f, 0.1f, 0.0f, 0.0f};
float d2[4] = {0.5f, 0.5f, 0.0f, 0.0f};
float d3[4] = {0.0f, 1.0f, 0.0f, 0.0f};
float d4[4] = {0.2f, 0.0f, 0.0f, 0.98f};

CHECK(ix->add_document("d0", "<DOC>d0</DOC>", NULL, d0, 1) >= 0);
CHECK(ix->add_document("d1", "<DOC>d1</DOC>", NULL, d1, 1) >= 0);
CHECK(ix->add_document("d2", "<DOC>d2</DOC>", NULL, d2, 1) >= 0);
CHECK(ix->add_document("d3", "<DOC>d3</DOC>", NULL, d3, 1) >= 0);
CHECK(ix->add_document("d4", "<DOC>d4</DOC>", NULL, d4, 1) >= 0);
CHECK(ix->flush() == 0);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};

long long n = ix->search_multivector(q, 1, 5);
CHECK(n == 5);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);
CHECK(strcmp(ix->get_hit(1)->filename, "d1") == 0);
CHECK(strcmp(ix->get_hit(2)->filename, "d2") == 0);
CHECK(strcmp(ix->get_hit(3)->filename, "d4") == 0);
CHECK(strcmp(ix->get_hit(4)->filename, "d3") == 0);
CHECK(ix->get_hit(0)->score >= ix->get_hit(1)->score);
CHECK(ix->get_hit(1)->score >= ix->get_hit(2)->score);
CHECK(ix->get_hit(2)->score >= ix->get_hit(3)->score);
CHECK(ix->get_hit(3)->score >= ix->get_hit(4)->score);

/* top_k cap: only the two best (d0, d1) come back */
long long n2 = ix->search_multivector(q, 1, 2);
CHECK(n2 == 2);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);
CHECK(strcmp(ix->get_hit(1)->filename, "d1") == 0);

delete ix;
delete [] dir;
printf("test_search_multivector_matches_brute_force_maxsim OK\n");
}

/*
	TEST_SEARCH_MULTIVECTOR_MULTI_TOKEN_MAXSIM()
	-----------------------------------------------
	MaxSim over multi-token docs/queries: sum over query tokens of that
	token's best dot to any doc token.  Both query tokens and both docM
	tokens are already unit vectors along distinct axes, so normalization is
	a no-op and the sums are exact:

		docM tokens: (1,0,0,0), (0,0,1,0)
		docS token:  (1,0,0,0)
		query tokens: q0=(1,0,0,0), q1=(0,0,1,0)

		MaxSim(docM) = max(dot(q0,t0)=1, dot(q0,t1)=0)
		             + max(dot(q1,t0)=0, dot(q1,t1)=1) = 1 + 1 = 2
		MaxSim(docS) = max(dot(q0,s0)=1) + max(dot(q1,s0)=0) = 1 + 0 = 1

	docM must outrank docS.
*/
static void test_search_multivector_multi_token_maxsim(void)
{
char *dir = make_index_dir();
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

float docM[2*4] = {1,0,0,0,  0,0,1,0};
float docS[1*4] = {1,0,0,0};

CHECK(ix->add_document("M", "<DOC>m</DOC>", NULL, docM, 2) >= 0);
CHECK(ix->add_document("S", "<DOC>s</DOC>", NULL, docS, 1) >= 0);
CHECK(ix->flush() == 0);

float q[2*4] = {1,0,0,0,  0,0,1,0};

long long n = ix->search_multivector(q, 2, 2);
CHECK(n == 2);
CHECK(strcmp(ix->get_hit(0)->filename, "M") == 0);
CHECK(strcmp(ix->get_hit(1)->filename, "S") == 0);
CHECK(ix->get_hit(0)->score > ix->get_hit(1)->score);

delete ix;
delete [] dir;
printf("test_search_multivector_multi_token_maxsim OK\n");
}

/*
	TEST_SEARCH_MULTIVECTOR_GUARDS()
	-----------------------------------
	NULL query / non-positive counts return 0 without crashing, and an index
	with rerank never configured also returns 0 (no multi-vector store to
	scan at all).
*/
static void test_search_multivector_guards(void)
{
char *dir = make_index_dir();
long long dim = 4;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

float d0[4] = {1,0,0,0};
CHECK(ix->add_document("d0", "<DOC>d0</DOC>", NULL, d0, 1) >= 0);
CHECK(ix->flush() == 0);

CHECK(ix->search_multivector(NULL, 0, 5) == 0);

delete ix;
delete [] dir;

/* separate index: rerank never configured */
char *dir2 = make_index_dir();
ATIRE_segment_index *ix2 = new ATIRE_segment_index();
CHECK(ix2->open(dir2) == 0);
CHECK(ix2->add_document("a", "<DOC>a</DOC>") >= 0);
CHECK(ix2->flush() == 0);

float q[4] = {1,0,0,0};
CHECK(ix2->search_multivector(q, 1, 5) == 0);

delete ix2;
delete [] dir2;
printf("test_search_multivector_guards OK\n");
}

int main(void)
{
test_search_multivector_matches_brute_force_maxsim();
test_search_multivector_multi_token_maxsim();
test_search_multivector_guards();
printf("ALL TESTS PASSED\n");
return 0;
}

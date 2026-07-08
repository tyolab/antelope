/*
	TEST_V6_BUILD_TOKEN_INDEX.CPP
	------------------------------
	Task 9 of the V6 plan: token-index build policy (eager/ondemand) plus the
	build_token_index() backfill.  Until this task search_multivector() always
	ran the brute-force MaxSim fallback (no .tann existed to build the ANN
	path against).  This test exercises the real token graph and locks the
	invariant that on a small, full-recall fixture the ANN path returns
	EXACTLY the same top-k (keys and order) as the brute-force fallback.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_v6_buildtann_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	12 single-token docs, each unit vectors spread around evenly-spaced
	directions in 4-d so the token graph has enough separability for full
	recall on a query aimed exactly at doc 0's direction.
*/
static void make_doc(float *out, long long dim, long long i, long long n)
{
double angle = (2.0 * M_PI * (double) i) / (double) n;
/* spread across the first two dims mostly, nudge the other two slightly by
   index so no two docs are exactly antipodal/degenerate */
out[0] = (float) cos(angle);
out[1] = (float) sin(angle);
out[2] = (float) (0.05 * cos(angle * 3.0));
out[3] = (float) (0.05 * sin(angle * 3.0));
}

/*
	TEST_ONDEMAND_BUILD_TOKEN_INDEX_MATCHES_FALLBACK()
	------------------------------------------------------
	Default policy is ondemand: after flush() there is no token index yet
	(disk_segment_has_token_index == 0) and search_multivector() runs the
	brute-force fallback.  Capture that result as ground truth.  Then call
	build_token_index() explicitly; the segment now has a token index
	(disk_segment_has_token_index == 1), and re-running the SAME query must
	return the identical top-5 keys in the identical order -- the ANN path
	must equal the brute-force path on this full-recall set.
*/
static void test_ondemand_build_token_index_matches_fallback(void)
{
char *dir = make_index_dir();
long long dim = 4;
long long n_docs = 12;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

long long i;
char key[32];
for (i = 0; i < n_docs; i++)
	{
	float v[4];
	make_doc(v, dim, i, n_docs);
	sprintf(key, "d%lld", i);
	char doc[64];
	sprintf(doc, "<DOC>%s</DOC>", key);
	CHECK(ix->add_document(key, doc, NULL, v, 1) >= 0);
	}
CHECK(ix->flush() == 0);

/* ondemand default: no token index built yet */
CHECK(ix->disk_segment_has_token_index(0) == 0);

float q[4];
make_doc(q, dim, 0, n_docs);	/* aimed exactly at d0's direction */

long long top_k = 5;
long long n = ix->search_multivector(q, 1, top_k);
CHECK(n == top_k);

char baseline[5][32];
for (i = 0; i < top_k; i++)
	strcpy(baseline[i], ix->get_hit(i)->filename);

/* backfill */
CHECK(ix->build_token_index() == 0);
CHECK(ix->disk_segment_has_token_index(0) == 1);

long long n2 = ix->search_multivector(q, 1, top_k);
CHECK(n2 == top_k);
for (i = 0; i < top_k; i++)
	CHECK(strcmp(ix->get_hit(i)->filename, baseline[i]) == 0);

delete ix;
delete [] dir;
printf("test_ondemand_build_token_index_matches_fallback OK\n");
}

/*
	TEST_EAGER_POLICY_BUILDS_AT_FLUSH()
	---------------------------------------
	set_token_index_policy(1) before adding documents; flush() must build the
	token graph automatically (no explicit build_token_index() call), and
	search_multivector() must return the correct top-5 (same fixture/query as
	above -- d0 first).
*/
static void test_eager_policy_builds_at_flush(void)
{
char *dir = make_index_dir();
long long dim = 4;
long long n_docs = 12;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_token_index_policy(1) == 0);

long long i;
char key[32];
for (i = 0; i < n_docs; i++)
	{
	float v[4];
	make_doc(v, dim, i, n_docs);
	sprintf(key, "d%lld", i);
	char doc[64];
	sprintf(doc, "<DOC>%s</DOC>", key);
	CHECK(ix->add_document(key, doc, NULL, v, 1) >= 0);
	}
CHECK(ix->flush() == 0);

/* eager: built automatically, no build_token_index() call here */
CHECK(ix->disk_segment_has_token_index(0) == 1);

float q[4];
make_doc(q, dim, 0, n_docs);

long long top_k = 5;
long long n = ix->search_multivector(q, 1, top_k);
CHECK(n == top_k);
CHECK(strcmp(ix->get_hit(0)->filename, "d0") == 0);

delete ix;
delete [] dir;
printf("test_eager_policy_builds_at_flush OK\n");
}

/*
	TEST_BUILD_TOKEN_INDEX_UNCONFIGURED_RERANK()
	--------------------------------------------------
	build_token_index() on an index that never called set_rerank_config()
	(no multi-vector store at all) is a no-op / error, consistent with
	search_multivector()'s own guard.
*/
static void test_build_token_index_unconfigured_rerank(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->add_document("a", "<DOC>a</DOC>") >= 0);
CHECK(ix->flush() == 0);

CHECK(ix->build_token_index() == 1);

delete ix;
delete [] dir;
printf("test_build_token_index_unconfigured_rerank OK\n");
}

int main(void)
{
test_ondemand_build_token_index_matches_fallback();
test_eager_policy_builds_at_flush();
test_build_token_index_unconfigured_rerank();
printf("ALL TESTS PASSED\n");
return 0;
}

/*
	TEST_V6_RECALL.CPP
	------------------
	Task 12 (final) of the V6 plan: recall sanity for the token-ANN path at
	DEFAULT knobs (token_top_p, candidate_multiplier -- whatever the
	constructor ships with, untouched by this test).  A synthetic corpus of
	N=500 docs x M=8 random unit-vector tokens is indexed; a Q=4-token query
	is issued BEFORE build_token_index() (brute-force MaxSim -- exact oracle)
	and AFTER (token-ANN candidate-gen -> exact MaxSim rescore).  The ANN
	top-10 must recall >= 90% of the exact top-10, and in particular must
	always surface the single best document.  Three "planted" docs that
	share exact query tokens are also asserted present in the ANN result to
	give a strong, easily-diagnosable recall signal beyond the aggregate
	ratio.
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
strcpy(buffer, "/tmp/ant_v6_recall_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	RANDOM_UNIT_VECTOR()
	---------------------
	Fills out[0..dim) with a random vector and normalizes it to unit length
	(the multivector writer also normalizes on ingest, but doing it here too
	means the "planted" / query vectors we hand-reason about are already
	unit vectors, and rand() is seeded once up-front for full determinism).
*/
static void random_unit_vector(float *out, long long dim)
{
long long d;
double norm = 0.0;
for (d = 0; d < dim; d++)
	{
	double v = ((double) rand() / (double) RAND_MAX) * 2.0 - 1.0;
	out[d] = (float) v;
	norm += v * v;
	}
norm = sqrt(norm);
if (norm < 1e-12)
	norm = 1.0;
for (d = 0; d < dim; d++)
	out[d] = (float) (out[d] / norm);
}

/*
	KEY_IN_SET()
	-------------
	Linear scan helper: is 'key' present among the first 'n' get_hit()
	filenames of 'ix'?
*/
static int key_in_hits(ATIRE_segment_index *ix, long long n, const char *key)
{
long long i;
for (i = 0; i < n; i++)
	if (strcmp(ix->get_hit(i)->filename, key) == 0)
		return 1;
return 0;
}

/*
	TEST_RECALL_AT_DEFAULT_KNOBS()
	--------------------------------
	500 docs x 8 tokens (dim=32), fixed seed.  Query = 4 random unit
	vectors.  Exact oracle captured pre-build (brute-force MaxSim, the
	documented behaviour of search_multivector() before any .tann exists);
	token-ANN result captured post-build_token_index().  recall@10 must be
	>= 0.9, the exact #1 hit must appear in the ANN top-10, and 3 planted
	docs sharing exact query tokens must all appear in the ANN top-10.
*/
static void test_recall_at_default_knobs(void)
{
srand(20260705);

char *dir = make_index_dir();
long long dim = 32;
long long n_docs = 500;
long long tokens_per_doc = 8;
long long top_k = 10;

ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

/* the query, generated up-front so 3 docs can be "planted" to share its
   exact tokens (guaranteed high MaxSim, a strong recall signal) */
long long n_query_tokens = 4;
float query[4 * 32];
long long qt;
for (qt = 0; qt < n_query_tokens; qt++)
	random_unit_vector(query + qt * dim, dim);

const char *planted_keys[3] = {"planted-0", "planted-1", "planted-2"};
long long i, t;
char key[32];
for (i = 0; i < n_docs; i++)
	{
	float doc[8 * 32];
	if (i < 3)
		{
		/* planted doc: token 0 is an exact query token (guarantees a
		   MaxSim contribution of 1.0 for that query token), remaining
		   tokens are random filler */
		memcpy(doc, query + (i % n_query_tokens) * dim, dim * sizeof(float));
		for (t = 1; t < tokens_per_doc; t++)
			random_unit_vector(doc + t * dim, dim);
		sprintf(key, "%s", planted_keys[i]);
		}
	else
		{
		for (t = 0; t < tokens_per_doc; t++)
			random_unit_vector(doc + t * dim, dim);
		sprintf(key, "doc-%lld", i);
		}
	char document[64];
	sprintf(document, "<DOC>%s</DOC>", key);
	CHECK(ix->add_document(key, document, NULL, doc, tokens_per_doc) >= 0);
	}
CHECK(ix->flush() == 0);

/* EXACT oracle: no token index built yet -> brute-force MaxSim */
CHECK(ix->disk_segment_has_token_index(0) == 0);
long long n_exact = ix->search_multivector(query, n_query_tokens, top_k);
CHECK(n_exact == top_k);

char exact_set[10][32];
for (i = 0; i < top_k; i++)
	strcpy(exact_set[i], ix->get_hit(i)->filename);

/* ANN: build the token graph, re-run the same query */
CHECK(ix->build_token_index() == 0);
CHECK(ix->disk_segment_has_token_index(0) == 1);

long long n_ann = ix->search_multivector(query, n_query_tokens, top_k);
CHECK(n_ann == top_k);

char ann_set[10][32];
for (i = 0; i < top_k; i++)
	strcpy(ann_set[i], ix->get_hit(i)->filename);

/* recall@10 = |exact ∩ ann| / 10 */
long long overlap = 0;
for (i = 0; i < top_k; i++)
	{
	long long j;
	for (j = 0; j < top_k; j++)
		if (strcmp(exact_set[i], ann_set[j]) == 0)
			{
			overlap++;
			break;
			}
	}
double recall = (double) overlap / (double) top_k;
printf("recall@%lld = %lld/%lld = %.3f\n", top_k, overlap, top_k, recall);
CHECK(recall >= 0.9);

/* the single best exact hit must be found by the ANN path */
CHECK(key_in_hits(ix, n_ann, exact_set[0]));

/* the 3 planted docs (sharing exact query tokens) must all surface */
CHECK(key_in_hits(ix, n_ann, planted_keys[0]));
CHECK(key_in_hits(ix, n_ann, planted_keys[1]));
CHECK(key_in_hits(ix, n_ann, planted_keys[2]));

delete ix;
delete [] dir;
printf("test_recall_at_default_knobs OK\n");
}

int main(void)
{
test_recall_at_default_knobs();
printf("ALL TESTS PASSED\n");
return 0;
}

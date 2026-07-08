/*
	TEST_V6_COMPACTION.CPP
	-----------------------
	Task 10 of the V6 plan: compaction rebuilds the merged segment's .tann
	token-ANN index over its merged .mvec sidecar.  Before this task, a
	compact() merging two segments that each had a .tann left the OUTPUT
	segment token-index-less (search_multivector silently fell back to
	brute-force MaxSim) -- correct, but the ANN path went cold on every
	merge.  This test locks:

	  1. Renumbering correctness: post-merge search_multivector() returns the
	     SAME top-5 keys in the SAME order as the pre-merge baseline.
	  2. The .tann is actually rebuilt (not just silently falling back):
	     disk_segment_has_token_index() is true for the merged segment.
	  3. Fallback-after-loss: deleting the merged segment's .tann file and
	     reopening the index still answers the same query correctly via the
	     brute-force MaxSim path (proves correctness holds without the ANN
	     sidecar too).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_v6_compaction_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	12 single-token docs, each dominated by a strong x-component that
	strictly decreases with i and a tiny, strictly increasing y-perturbation,
	so dot(q, d_i) for q = (1,0,0,0) is strictly decreasing in i -- no ties,
	so the ranking is fully deterministic even across the docid renumbering
	a merge performs.
*/
static void make_doc(float *out, long long dim, long long i)
{
out[0] = (float)(10 - i);
out[1] = (float)(i) * 0.01f;
out[2] = 0.0f;
out[3] = 0.0f;
(void)dim;
}

/*
	TEST_COMPACTION_REBUILDS_TANN()
	--------------------------------
*/
static void test_compaction_rebuilds_tann(void)
{
char *dir = make_index_dir();
long long dim = 4;
long long n_docs = 12;
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

long long i;
char key[32], doc[64];
float v[4];

/* first flush: 6 docs */
for (i = 0; i < 6; i++)
	{
	make_doc(v, dim, i);
	sprintf(key, "d%lld", i);
	sprintf(doc, "<DOC>%s</DOC>", key);
	CHECK(ix->add_document(key, doc, NULL, v, 1) >= 0);
	}
CHECK(ix->flush() == 0);

/* second flush: 6 more docs -> a second disk segment */
for (i = 6; i < n_docs; i++)
	{
	make_doc(v, dim, i);
	sprintf(key, "d%lld", i);
	sprintf(doc, "<DOC>%s</DOC>", key);
	CHECK(ix->add_document(key, doc, NULL, v, 1) >= 0);
	}
CHECK(ix->flush() == 0);

CHECK(ix->disk_segment_count() == 2);

/* build .tann on both pre-merge segments */
CHECK(ix->build_token_index() == 0);
CHECK(ix->disk_segment_has_token_index(0) == 1);
CHECK(ix->disk_segment_has_token_index(1) == 1);

float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
long long top_k = 5;

long long n = ix->search_multivector(q, 1, top_k);
CHECK(n == top_k);
char baseline[5][32];
for (i = 0; i < top_k; i++)
	strcpy(baseline[i], ix->get_hit(i)->filename);

/* expected: d0..d4, strictly decreasing dot product */
CHECK(strcmp(baseline[0], "d0") == 0);
CHECK(strcmp(baseline[1], "d1") == 0);
CHECK(strcmp(baseline[2], "d2") == 0);
CHECK(strcmp(baseline[3], "d3") == 0);
CHECK(strcmp(baseline[4], "d4") == 0);

/* trigger a merge of the two disk segments */
long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
CHECK(ix->compact(gens, 2) == 0);
CHECK(ix->disk_segment_count() == 1);

/* the merged segment's .tann was rebuilt (not just fallen back) */
CHECK(ix->disk_segment_has_token_index(0) == 1);

/* renumbering correctness: same top-5 keys, same order */
long long n2 = ix->search_multivector(q, 1, top_k);
CHECK(n2 == top_k);
for (i = 0; i < top_k; i++)
	CHECK(strcmp(ix->get_hit(i)->filename, baseline[i]) == 0);

long long out_gen = ix->disk_segment_generation(0);
delete ix;

/*
	Fallback-after-.tann-loss: delete the merged segment's .tann sidecar,
	then reopen the index.  search_multivector() must still return the
	correct top-5 via the brute-force MaxSim fallback (no ANN graph to lean
	on -- proves correctness holds independent of the ANN path).
*/
char tann_name[4096];
snprintf(tann_name, sizeof(tann_name), "%s/seg_%06lld.tann", dir, out_gen);
remove(tann_name);
/* the .tann's companion graph-topology sidecar (ANT_hnsw::save appends ".g") */
char tann_graph_name[4160];
snprintf(tann_graph_name, sizeof(tann_graph_name), "%s.g", tann_name);
remove(tann_graph_name);

ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);
CHECK(reopened->set_rerank_config(dim, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(reopened->disk_segment_count() == 1);
CHECK(reopened->disk_segment_has_token_index(0) == 0);		/* no .tann on disk -> brute-force fallback */

long long n3 = reopened->search_multivector(q, 1, top_k);
CHECK(n3 == top_k);
for (i = 0; i < top_k; i++)
	CHECK(strcmp(reopened->get_hit(i)->filename, baseline[i]) == 0);

delete reopened;
delete [] dir;
printf("test_compaction_rebuilds_tann OK\n");
}

int main(void)
{
test_compaction_rebuilds_tann();
printf("ALL TESTS PASSED\n");
return 0;
}

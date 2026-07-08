/*
	TEST_PQ_COMPACTION.CPP
	----------------------
	compact() rebuilds the merged segment's .pq (retrain + renumber); PQ search
	stays correct after merge, and falls back to resident float if the .pq is lost.

	This test also exercises the compaction shuffle-teardown of PQ-built input
	segments: run it under ASan with ASAN_OPTIONS=detect_leaks=1 to guard against
	regressing the pq_vectors teardown free (a leak, not a crash -- invisible in a
	plain functional run).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define DIM 16

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_pqcompact_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/* doc i: dominant coordinate on unique axis i (i must be < DIM for uniqueness) */
static void make_vec(long long i, float *v)
{
for (int d = 0; d < DIM; d++)
	v[d] = 0.01f * (float)(((i * 3 + d) % 7) - 3);
v[i % DIM] += 5.0f;
}

static void add_docs(ATIRE_segment_index *ix, long long from, long long to)
{
float v[DIM]; char key[32], body[64];
for (long long i = from; i < to; i++)
	{ make_vec(i, v); sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld text</DOC>", i); CHECK(ix->add_document(key, body, v) >= 0); }
}

static void test_compaction_rebuilds_pq(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

add_docs(ix, 0, 6);
CHECK(ix->flush() == 0);
add_docs(ix, 6, 12);
CHECK(ix->flush() == 0);
CHECK(ix->disk_segment_count() == 2);
CHECK(ix->build_pq() == 0);
CHECK(ix->disk_segment_has_pq(0) == 1);
CHECK(ix->disk_segment_has_pq(1) == 1);

/* query near doc-7 (axis 7); capture pre-merge top-1 */
float q[DIM]; make_vec(7, q);
CHECK(ix->search_vector(q, 5) >= 1);
char pre_top1[32]; strcpy(pre_top1, ix->get_hit(0)->filename);
CHECK(strcmp(pre_top1, "doc-7") == 0);

/* merge the two segments */
long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
CHECK(ix->compact(gens, 2) == 0);
CHECK(ix->disk_segment_count() == 1);
CHECK(ix->disk_segment_has_pq(0) == 1);				/* .pq rebuilt, not just fallen back */

/* renumbering correctness: same top-1 after merge */
CHECK(ix->search_vector(q, 5) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "doc-7") == 0);

long long out_gen = ix->disk_segment_generation(0);
delete ix;

/* delete the merged .pq, reopen: PQ search still correct via resident float fallback */
char pq_name[4096];
snprintf(pq_name, sizeof(pq_name), "%s/seg_%06lld.pq", dir, out_gen);
remove(pq_name);

ATIRE_segment_index *re = new ATIRE_segment_index();
CHECK(re->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(re->open(dir) == 0);
CHECK(re->disk_segment_count() == 1);
CHECK(re->disk_segment_has_pq(0) == 0);				/* no .pq on disk -> float fallback */
CHECK(re->search_vector(q, 5) >= 1);
CHECK(strcmp(re->get_hit(0)->filename, "doc-7") == 0);	/* still correct */
delete re;

delete [] dir;
printf("test_compaction_rebuilds_pq OK\n");
}

int main(void)
{
test_compaction_rebuilds_pq();
printf("test_pq_compaction PASSED\n");
return 0;
}

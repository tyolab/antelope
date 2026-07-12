/*
	TEST_PQ_METRICS.CPP
	-------------------
	#21 M2: cosine/L2 end-to-end PQ search (replace recall floor, rerank exact top-1)
	and the mixed PQ/float-fallback mid-query ordering. Existing test_pq_search/recall/
	rerank cover only VECTOR_METRIC_DOT.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define DIM 16

static char *make_index_dir(void)
{
char buffer[64]; strcpy(buffer, "/tmp/ant_pqmetrics_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1]; strcpy(result, dir); return result;
}

/* well-separated cluster: doc i dominant on axis (i % DIM) */
static void make_vec(long long i, float *v)
{
for (int d = 0; d < DIM; d++) v[d] = 0.02f * (float)(((i * 7 + d) % 5) - 2);
v[i % DIM] += 3.0f;
}

static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{
float v[DIM]; char key[32], body[64];
for (long long i = lo; i < hi; i++)
	{ make_vec(i, v); sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld z</DOC>", i); CHECK(ix->add_document(key, body, v) >= 0); }
}

static long in_topk(ATIRE_segment_index *ix, long long n, const char *want)
{
for (long long i = 0; i < n; i++) if (strcmp(ix->get_hit(i)->filename, want) == 0) return 1;
return 0;
}

/* replace posture: planted nearest is recalled in top-k (ADC is approximate). */
static void test_replace_recall(long metric, const char *label)
{
const long long N = 12;					/* N <= DIM so each dominant axis is unique */
char *dp = make_index_dir();
ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->set_vector_config(DIM, metric) == 0);
CHECK(pq->open(dp) == 0);
CHECK(pq->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
add_docs(pq, 0, N); CHECK(pq->flush() == 0);
CHECK(pq->build_pq() == 0); CHECK(pq->disk_segment_has_pq(0) == 1);
float q[DIM]; make_vec(5, q);
long long n = pq->search_vector(q, 5);
CHECK(n >= 1);
CHECK(in_topk(pq, n, "doc-5"));				/* planted nearest recalled under `metric` */
delete pq; delete [] dp;
printf("test_replace_recall[%s] OK\n", label);
}

/* rerank posture: top-1 equals the exact float index (rerank rescores through resident float). */
static void test_rerank_exact(long metric, const char *label)
{
const long long N = 12;
char *de = make_index_dir();
ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->set_vector_config(DIM, metric) == 0);
CHECK(ex->open(de) == 0);
add_docs(ex, 0, N); CHECK(ex->flush() == 0);

char *dp = make_index_dir();
ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->set_vector_config(DIM, metric) == 0);
CHECK(pq->open(dp) == 0);
CHECK(pq->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
add_docs(pq, 0, N); CHECK(pq->flush() == 0);
CHECK(pq->build_pq() == 0);

float q[DIM]; make_vec(7, q);
CHECK(ex->search_vector(q, 5) >= 1);
CHECK(pq->search_vector(q, 5) >= 1);
CHECK(strcmp(pq->get_hit(0)->filename, ex->get_hit(0)->filename) == 0);	/* rerank top-1 == exact top-1 */
delete ex; delete pq; delete [] de; delete [] dp;
printf("test_rerank_exact[%s] OK\n", label);
}

/* mixed: segment 0 has .pq (ADC), segment 1 has none (float scan fallback) in one query.
   The global nearest is planted in segment 1, so a sign/scale mismatch between the ADC and
   float-scan scoring paths would demote it out of top-k. */
static void test_mixed_pq_float_fallback(long metric, const char *label)
{
char *dp = make_index_dir();
ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->set_vector_config(DIM, metric) == 0);
CHECK(pq->open(dp) == 0);
CHECK(pq->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
add_docs(pq, 0, 6); CHECK(pq->flush() == 0);		/* segment 0 */
CHECK(pq->build_pq() == 0);				/* seg 0 -> .pq */
add_docs(pq, 6, 12); CHECK(pq->flush() == 0);		/* segment 1, build_pq NOT re-run */
CHECK(pq->disk_segment_has_pq(0) == 1);
CHECK(pq->disk_segment_has_pq(1) == 0);			/* mixed state: seg1 falls back to float scan */

float q[DIM]; make_vec(8, q);				/* nearest = doc-8, which lives in segment 1 (float-fallback) */
long long n = pq->search_vector(q, 5);
CHECK(n >= 1);
CHECK(in_topk(pq, n, "doc-8"));				/* float-fallback segment's nearest survives the merge */
delete pq; delete [] dp;
printf("test_mixed_pq_float_fallback[%s] OK\n", label);
}

int main(void)
{
test_replace_recall(ATIRE_segment_index::VECTOR_METRIC_COSINE, "cosine");
test_replace_recall(ATIRE_segment_index::VECTOR_METRIC_L2, "l2");
test_rerank_exact(ATIRE_segment_index::VECTOR_METRIC_COSINE, "cosine");
test_rerank_exact(ATIRE_segment_index::VECTOR_METRIC_L2, "l2");
test_mixed_pq_float_fallback(ATIRE_segment_index::VECTOR_METRIC_COSINE, "cosine");
test_mixed_pq_float_fallback(ATIRE_segment_index::VECTOR_METRIC_L2, "l2");
printf("ALL test_pq_metrics PASSED\n");
return 0;
}

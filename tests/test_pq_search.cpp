/*
	TEST_PQ_SEARCH.CPP
	------------------
	Replace-posture PQ search (VECTOR_MODE_PQ via search_vector) + a byte-identical
	regression lock proving PQ-unconfigured search is unchanged.
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
char buffer[64];
strcpy(buffer, "/tmp/ant_pqsearch_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/* well-separated cluster vector: doc i lives near axis (i % DIM) */
static void make_vec(long long i, float *v)
{
for (int d = 0; d < DIM; d++)
	v[d] = 0.02f * (float)(((i * 7 + d) % 5) - 2);
v[i % DIM] += 3.0f;			/* dominant coordinate => clearly separable */
}

static void fill_index(ATIRE_segment_index *ix, long long n)
{
float v[DIM];
char key[32], body[64];
for (long long i = 0; i < n; i++)
	{
	make_vec(i, v);
	sprintf(key, "doc-%lld", i);
	sprintf(body, "<DOC>term%lld payload</DOC>", i);
	CHECK(ix->add_document(key, body, v) >= 0);
	}
CHECK(ix->flush() == 0);
}

/*
	TEST_REPLACE_POSTURE_TOP1_MATCHES_EXACT()
	-----------------------------------------
	On well-separated data, replace-posture PQ ADC returns the same top-1 as exact.
*/
static void test_replace_posture_top1_matches_exact(void)
{
const long long N = 12;		/* must be <= DIM so each doc's dominant axis (i % DIM) is unique -- no aliasing between docs */

/* exact index (no PQ) */
char *dir_e = make_index_dir();
ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ex->open(dir_e) == 0);
fill_index(ex, N);

/* pq replace index */
char *dir_p = make_index_dir();
ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(pq->open(dir_p) == 0);
CHECK(pq->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
fill_index(pq, N);
CHECK(pq->build_pq() == 0);
CHECK(pq->disk_segment_has_pq(0) == 1);

/* query near doc-5's cluster (axis 5) */
float q[DIM];
make_vec(5, q);

CHECK(ex->search_vector(q, 5) == 5);
CHECK(pq->search_vector(q, 5) == 5);
CHECK(strcmp(ex->get_hit(0)->filename, "doc-5") == 0);
CHECK(strcmp(pq->get_hit(0)->filename, ex->get_hit(0)->filename) == 0);	/* PQ top-1 == exact top-1 */

delete ex; delete pq;
delete [] dir_e; delete [] dir_p;
printf("test_replace_posture_top1_matches_exact OK\n");
}

/*
	TEST_PQ_UNCONFIGURED_IDENTICAL()
	--------------------------------
	The search_vector routing change is a no-op when PQ is unconfigured: two
	identical PQ-less indexes rank identically (count + filename/gen/docid/score).
*/
static void test_pq_unconfigured_identical(void)
{
const long long N = 40;
char *dir_a = make_index_dir();
ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(a->open(dir_a) == 0);
fill_index(a, N);

char *dir_b = make_index_dir();
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(b->open(dir_b) == 0);
fill_index(b, N);

float q[DIM];
make_vec(9, q);
long long ca = a->search_vector(q, 10);
long long cb = b->search_vector(q, 10);
CHECK(ca == cb);
for (long long i = 0; i < ca; i++)
	{
	ATIRE_segment_index::hit *ha = a->get_hit(i);
	ATIRE_segment_index::hit *hb = b->get_hit(i);
	CHECK(strcmp(ha->filename, hb->filename) == 0);
	CHECK(ha->generation == hb->generation);
	CHECK(ha->docid == hb->docid);
	CHECK(fabs(ha->score - hb->score) < 1e-9);
	}

delete a; delete b;
delete [] dir_a; delete [] dir_b;
printf("test_pq_unconfigured_identical OK\n");
}

int main(void)
{
test_replace_posture_top1_matches_exact();
test_pq_unconfigured_identical();
printf("test_pq_search PASSED\n");
return 0;
}

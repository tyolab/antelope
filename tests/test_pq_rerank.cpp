/*
	TEST_PQ_RERANK.CPP
	------------------
	Rerank posture: PQ ADC shortlist -> exact resident-float rescore. Recall@10
	>= replace posture and >= 0.9; exact tier fixes ADC mis-rankings.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define DIM 32

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_pqrerank_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/* pseudo-random unit-ish vector seeded by i (deterministic) */
static unsigned long long rng_state;
static double next_rand(void) { rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL; return (double)((rng_state >> 33) & 0x7fffffff) / (double)0x7fffffff; }

static void make_vec(long long i, float *v)
{
rng_state = (unsigned long long)(i + 1) * 2654435761ULL;
double norm = 0.0;
for (int d = 0; d < DIM; d++)
	{ v[d] = (float)(next_rand() * 2.0 - 1.0); norm += (double)v[d] * v[d]; }
norm = sqrt(norm) + 1e-9;
for (int d = 0; d < DIM; d++) v[d] = (float)(v[d] / norm);
}

static void fill(ATIRE_segment_index *ix, long long n)
{
float v[DIM]; char key[32], body[64];
for (long long i = 0; i < n; i++)
	{ make_vec(i, v); sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld body</DOC>", i); CHECK(ix->add_document(key, body, v) >= 0); }
CHECK(ix->flush() == 0);
}

/* collect top-k docids (parsed from "doc-<n>") into out[] */
static void topk_ids(ATIRE_segment_index *ix, const float *q, long long k, long long *out, long long *n)
{
long long c = ix->search_vector(q, k);
*n = c;
for (long long i = 0; i < c; i++)
	out[i] = atoll(ix->get_hit(i)->filename + 4);
}

static void build_index(char **dir_out, ATIRE_segment_index **ix_out, long posture, long rerank_quant, long long n)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_pq_config(8, posture, rerank_quant) == 0);
fill(ix, n);
CHECK(ix->build_pq() == 0);
CHECK(ix->disk_segment_has_pq(0) == 1);
*dir_out = dir; *ix_out = ix;
}

static double recall_at_10(ATIRE_segment_index *cand, ATIRE_segment_index *exact, long long n_queries)
{
long long hit = 0, total = 0;
float q[DIM];
for (long long qi = 0; qi < n_queries; qi++)
	{
	make_vec(10000 + qi, q);
	long long e_ids[10], c_ids[10], en = 0, cn = 0;
	topk_ids(exact, q, 10, e_ids, &en);
	topk_ids(cand, q, 10, c_ids, &cn);
	for (long long a = 0; a < en; a++)
		{
		total++;
		for (long long b = 0; b < cn; b++)
			if (e_ids[a] == c_ids[b]) { hit++; break; }
		}
	}
return total > 0 ? (double)hit / (double)total : 0.0;
}

static void test_rerank_recall_beats_replace(void)
{
const long long N = 300;
char *dir_e; ATIRE_segment_index *exact;
{ /* exact reference: no PQ */
char *d = make_index_dir(); exact = new ATIRE_segment_index();
CHECK(exact->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(exact->open(d) == 0); fill(exact, N); dir_e = d;
}
char *dir_rep; ATIRE_segment_index *rep; build_index(&dir_rep, &rep, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT, N);
char *dir_rr; ATIRE_segment_index *rr; build_index(&dir_rr, &rr, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT, N);

double r_replace = recall_at_10(rep, exact, 30);
double r_rerank = recall_at_10(rr, exact, 30);
printf("  replace recall@10=%.3f  rerank recall@10=%.3f\n", r_replace, r_rerank);
CHECK(r_rerank >= 0.9);
CHECK(r_rerank >= r_replace - 1e-9);

delete exact; delete rep; delete rr;
delete [] dir_e; delete [] dir_rep; delete [] dir_rr;
printf("test_rerank_recall_beats_replace OK\n");
}

static void test_rerank_int8_config_recall(void)
{
const long long N = 300;
char *dir_e; ATIRE_segment_index *exact;
{ char *d = make_index_dir(); exact = new ATIRE_segment_index();
CHECK(exact->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(exact->open(d) == 0); fill(exact, N); dir_e = d; }
char *dir_rr; ATIRE_segment_index *rr; build_index(&dir_rr, &rr, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_INT8, N);
double r = recall_at_10(rr, exact, 30);
printf("  rerank(int8-config) recall@10=%.3f\n", r);
CHECK(r >= 0.9);
delete exact; delete rr; delete [] dir_e; delete [] dir_rr;
printf("test_rerank_int8_config_recall OK\n");
}

int main(void)
{
test_rerank_recall_beats_replace();
test_rerank_int8_config_recall();
printf("test_pq_rerank PASSED\n");
return 0;
}

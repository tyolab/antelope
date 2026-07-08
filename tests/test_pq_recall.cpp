/*
	TEST_PQ_RECALL.CPP
	------------------
	Recall sanity at the DEFAULT m (set_pq_config(0,...)): rerank posture recall@10
	vs exact >= 0.9 on 500 dim-32 vectors; 3 planted near-query docs all recalled.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define DIM 32
#define NDOCS 500

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_pqrecall_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

static unsigned long long rng;
static double nextf(void) { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return (double)((rng >> 33) & 0x7fffffff) / (double)0x7fffffff; }

static void unit_vec(long long seed, float *v)
{
rng = (unsigned long long)(seed + 1) * 2654435761ULL;
double norm = 0.0;
for (int d = 0; d < DIM; d++) { v[d] = (float)(nextf() * 2.0 - 1.0); norm += (double)v[d]*v[d]; }
norm = sqrt(norm) + 1e-9;
for (int d = 0; d < DIM; d++) v[d] = (float)(v[d]/norm);
}

/* fixed query direction */
static void query_vec(float *v)
{
for (int d = 0; d < DIM; d++) v[d] = 0.0f;
v[0] = v[1] = v[2] = 0.577f;		/* arbitrary fixed direction */
}

/* planted doc: query + small perturbation, re-normalized */
static void planted_vec(long long k, float *v)
{
query_vec(v);
rng = (unsigned long long)(90000 + k) * 2654435761ULL;
double norm = 0.0;
for (int d = 0; d < DIM; d++) { v[d] += (float)((nextf()*2.0-1.0) * 0.05); norm += (double)v[d]*v[d]; }
norm = sqrt(norm) + 1e-9;
for (int d = 0; d < DIM; d++) v[d] = (float)(v[d]/norm);
}

static void fill(ATIRE_segment_index *ix)
{
float v[DIM]; char key[32], body[64];
long long i;
for (i = 0; i < NDOCS; i++)
	{ unit_vec(i, v); sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld body</DOC>", i); CHECK(ix->add_document(key, body, v) >= 0); }
for (long long k = 0; k < 3; k++)
	{ planted_vec(k, v); sprintf(key, "planted-%lld", k); sprintf(body, "<DOC>plantterm%lld here</DOC>", k); CHECK(ix->add_document(key, body, v) >= 0); }
CHECK(ix->flush() == 0);
}

static ATIRE_segment_index *open_exact(char **dir_out)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
fill(ix);
*dir_out = dir;
return ix;
}

static ATIRE_segment_index *open_pq(char **dir_out, long posture)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_pq_config(0, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* DEFAULT m */
fill(ix);
CHECK(ix->build_pq() == 0);
CHECK(ix->disk_segment_has_pq(0) == 1);
*dir_out = dir;
return ix;
}

static void topk_keys(ATIRE_segment_index *ix, const float *q, long long k, char keys[][32], long long *n)
{
long long c = ix->search_vector(q, k);
*n = c;
for (long long i = 0; i < c; i++) strncpy(keys[i], ix->get_hit(i)->filename, 31), keys[i][31] = 0;
}

static double recall10(ATIRE_segment_index *cand, ATIRE_segment_index *exact, long long nq)
{
long long hit = 0, total = 0; float q[DIM];
for (long long qi = 0; qi < nq; qi++)
	{
	unit_vec(50000 + qi, q);
	char ek[10][32], ck[10][32]; long long en = 0, cn = 0;
	topk_keys(exact, q, 10, ek, &en);
	topk_keys(cand, q, 10, ck, &cn);
	for (long long a = 0; a < en; a++)
		{ total++; for (long long b = 0; b < cn; b++) if (strcmp(ek[a], ck[b]) == 0) { hit++; break; } }
	}
return total ? (double)hit/(double)total : 0.0;
}

int main(void)
{
char *de, *dp, *dr;
ATIRE_segment_index *exact = open_exact(&de);
ATIRE_segment_index *rep = open_pq(&dp, ATIRE_segment_index::PQ_POSTURE_REPLACE);
ATIRE_segment_index *rr = open_pq(&dr, ATIRE_segment_index::PQ_POSTURE_RERANK);

CHECK(rr->pq_m() == 16);		/* default_pq_m(32) == 16 */

double r_rep = recall10(rep, exact, 40);
double r_rr = recall10(rr, exact, 40);
printf("  default m=%lld  replace recall@10=%.3f  rerank recall@10=%.3f\n", (long long)rr->pq_m(), r_rep, r_rr);
CHECK(r_rr >= 0.9);

/* planted docs all in rerank top-10 for the planted query */
float pq_query[DIM]; query_vec(pq_query);
char pk[10][32]; long long pn = 0;
topk_keys(rr, pq_query, 10, pk, &pn);
for (long long k = 0; k < 3; k++)
	{
	char want[32]; sprintf(want, "planted-%lld", k);
	long long found = 0;
	for (long long i = 0; i < pn; i++) if (strcmp(pk[i], want) == 0) { found = 1; break; }
	CHECK(found);
	}

delete exact; delete rep; delete rr;
delete [] de; delete [] dp; delete [] dr;
printf("test_pq_recall PASSED\n");
return 0;
}

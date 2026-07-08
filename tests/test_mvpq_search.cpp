/*
	TEST_MVPQ_SEARCH.CPP -- replace posture: search_multivector scores via ADC-MaxSim.
	Top-1 matches exact on well-separated data; V5/V6-unconfigured search unchanged.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 16

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqs_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }

/* doc i: single token dominant on unique axis i (i < DIM) -> unambiguous top-1 */
static void doc_vec(long long i, float *v)
{ for (int d=0;d<DIM;d++) v[d]=0.01f*(float)(((i*3+d)%7)-3); v[i%DIM]+=5.0f;
  double n=0; for(int d=0;d<DIM;d++) n+=(double)v[d]*v[d]; n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }

static void fill(ATIRE_segment_index *ix, long long n)
{ float t[DIM]; char k[32], b[64]; for (long long i=0;i<n;i++){ doc_vec(i,t); sprintf(k,"doc-%lld",i); sprintf(b,"<DOC>term%lld x</DOC>",i); CHECK(ix->add_document(k,b,NULL,t,1)>=0);} CHECK(ix->flush()==0); }

int main(void)
{
const long long N = 12;			/* <= DIM: unique dominant axes */
char *de = dir_(); ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->open(de) == 0);
CHECK(ex->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
fill(ex, N);

char *dp = dir_(); ATIRE_segment_index *pq = new ATIRE_segment_index();
CHECK(pq->open(dp) == 0);
CHECK(pq->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(pq->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
fill(pq, N);
CHECK(pq->build_multivector_pq() == 0);
CHECK(pq->disk_segment_has_multivector_pq(0) == 1);

float q[DIM]; doc_vec(5, q);
CHECK(ex->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(ex->get_hit(0)->filename, "doc-5") == 0);
CHECK(pq->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(pq->get_hit(0)->filename, "doc-5") == 0);	/* ADC-MaxSim top-1 == exact */

/* V5/V6-unconfigured byte-identical: two PQ-less indexes agree rank-for-rank */
char *da = dir_(); ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->open(da) == 0); CHECK(a->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0); fill(a, N);
char *db = dir_(); ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(db) == 0); CHECK(b->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0); fill(b, N);
float q2[DIM]; doc_vec(9, q2);
long long ca = a->search_multivector(q2, 1, 10), cb = b->search_multivector(q2, 1, 10);
CHECK(ca == cb);
for (long long i = 0; i < ca; i++)
	{ CHECK(strcmp(a->get_hit(i)->filename, b->get_hit(i)->filename) == 0); CHECK(fabs(a->get_hit(i)->score - b->get_hit(i)->score) < 1e-9); }

delete ex; delete pq; delete a; delete b; delete [] de; delete [] dp; delete [] da; delete [] db;
printf("test_mvpq_search PASSED\n");
return 0;
}

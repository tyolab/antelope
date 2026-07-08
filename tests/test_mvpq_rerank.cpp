/*
	TEST_MVPQ_RERANK.CPP -- rerank posture: float token-HNSW candidates -> exact
	float MaxSim rescore. recall@10 vs exact >= replace and >= 0.9; replace ADC
	recall has a sanity floor (proves the ADC-MaxSim replace path works).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 32

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqr_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }
static unsigned long long R;
static double nf(void){ R=R*6364136223846793005ULL+1442695040888963407ULL; return (double)((R>>33)&0x7fffffff)/(double)0x7fffffff; }
static void tok(long long seed, float *v){ R=(unsigned long long)(seed+1)*2654435761ULL; double n=0; for(int d=0;d<DIM;d++){v[d]=(float)(nf()*2.0-1.0);n+=(double)v[d]*v[d];} n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }

static void fill(ATIRE_segment_index *ix, long long n)
{ float doc[3*DIM]; char k[32], b[64]; long long seed=0;
  for(long long i=0;i<n;i++){ long long md=(i%3)+1; for(long long t=0;t<md;t++) tok(seed++, doc+t*DIM); sprintf(k,"doc-%lld",i); sprintf(b,"<DOC>term%lld z</DOC>",i); CHECK(ix->add_document(k,b,NULL,doc,md)>=0);} CHECK(ix->flush()==0); }

static ATIRE_segment_index *mk(char **d, long has_pq, long posture)
{ *d=dir_(); ATIRE_segment_index *ix=new ATIRE_segment_index(); CHECK(ix->open(*d)==0); CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0);
  if (has_pq) CHECK(ix->set_multivector_pq_config(8, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT)==0); fill(ix, 300); if (has_pq){ CHECK(ix->build_multivector_pq()==0); CHECK(ix->build_token_index()==0);} return ix; }

static double recall(ATIRE_segment_index *cand, ATIRE_segment_index *ex, long long nq)
{ long long hit=0,tot=0; float q[3*DIM];
  for(long long qi=0;qi<nq;qi++){ long long m=(qi%3)+1; for(long long t=0;t<m;t++) tok(50000+qi*3+t, q+t*DIM);
    long long en=ex->search_multivector(q,m,10); long long e[10]; for(long long i=0;i<en;i++) e[i]=atoll(ex->get_hit(i)->filename+4);
    long long cn=cand->search_multivector(q,m,10); long long c[10]; for(long long i=0;i<cn;i++) c[i]=atoll(cand->get_hit(i)->filename+4);
    for(long long a=0;a<en;a++){ tot++; for(long long bb=0;bb<cn;bb++) if(e[a]==c[bb]){hit++;break;} } }
  return tot? (double)hit/(double)tot : 0.0; }

int main(void)
{
char *de; ATIRE_segment_index *ex = mk(&de, 0, 0);
char *dp; ATIRE_segment_index *rep = mk(&dp, 1, ATIRE_segment_index::PQ_POSTURE_REPLACE);
char *dr; ATIRE_segment_index *rr = mk(&dr, 1, ATIRE_segment_index::PQ_POSTURE_RERANK);
double r_rep = recall(rep, ex, 30), r_rr = recall(rr, ex, 30);
printf("  replace recall@10=%.3f  rerank recall@10=%.3f\n", r_rep, r_rr);
CHECK(r_rr >= 0.9);
CHECK(r_rr >= r_rep - 1e-9);
CHECK(r_rep >= 0.5);			/* ADC-MaxSim replace path actually ranks sensibly */
delete ex; delete rep; delete rr; delete [] de; delete [] dp; delete [] dr;
printf("test_mvpq_rerank PASSED\n");
return 0;
}

/*
	TEST_MVPQ_RECALL.CPP -- recall sanity at the DEFAULT m (set_multivector_pq_config(0,...)):
	rerank posture recall@10 vs exact MaxSim >= 0.9 on 400 docs of dim-32 tokens.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 32
#define NDOCS 400

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqrec_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }
static unsigned long long R;
static double nf(void){ R=R*6364136223846793005ULL+1442695040888963407ULL; return (double)((R>>33)&0x7fffffff)/(double)0x7fffffff; }
static void tok(long long seed, float *v){ R=(unsigned long long)(seed+1)*2654435761ULL; double n=0; for(int d=0;d<DIM;d++){v[d]=(float)(nf()*2.0-1.0);n+=(double)v[d]*v[d];} n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }
static void fill(ATIRE_segment_index *ix){ float doc[3*DIM]; char k[32],b[64]; long long seed=0; for(long long i=0;i<NDOCS;i++){ long long md=(i%3)+1; for(long long t=0;t<md;t++) tok(seed++, doc+t*DIM); sprintf(k,"doc-%lld",i); sprintf(b,"<DOC>term%lld z</DOC>",i); CHECK(ix->add_document(k,b,NULL,doc,md)>=0);} CHECK(ix->flush()==0); }

static double recall(ATIRE_segment_index *cand, ATIRE_segment_index *ex, long long nq)
{ long long hit=0,tot=0; float q[3*DIM];
  for(long long qi=0;qi<nq;qi++){ long long m=(qi%3)+1; for(long long t=0;t<m;t++) tok(70000+qi*3+t, q+t*DIM);
    long long en=ex->search_multivector(q,m,10); long long e[10]; for(long long i=0;i<en;i++) e[i]=atoll(ex->get_hit(i)->filename+4);
    long long cn=cand->search_multivector(q,m,10); long long c[10]; for(long long i=0;i<cn;i++) c[i]=atoll(cand->get_hit(i)->filename+4);
    for(long long a=0;a<en;a++){ tot++; for(long long bb=0;bb<cn;bb++) if(e[a]==c[bb]){hit++;break;} } }
  return tot? (double)hit/(double)tot : 0.0; }

int main(void)
{
char *de = dir_(); ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->open(de) == 0); CHECK(ex->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0); fill(ex);
char *dr = dir_(); ATIRE_segment_index *rr = new ATIRE_segment_index();
CHECK(rr->open(dr) == 0); CHECK(rr->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(rr->set_multivector_pq_config(0, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* default m */
CHECK(rr->multivector_pq_m() == 16);	/* default_pq_m(32) */
fill(rr); CHECK(rr->build_multivector_pq() == 0); CHECK(rr->build_token_index() == 0);
double r = recall(rr, ex, 40);
printf("  default m=%lld  rerank recall@10=%.3f\n", (long long)rr->multivector_pq_m(), r);
CHECK(r >= 0.9);
delete ex; delete rr; delete [] de; delete [] dr;
printf("test_mvpq_recall PASSED\n");
return 0;
}

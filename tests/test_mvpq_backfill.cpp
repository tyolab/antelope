/*
	TEST_MVPQ_BACKFILL.CPP -- build_multivector_pq() creates .mvpq for a flushed
	segment, disk_segment_has_multivector_pq reflects it, reopen loads it, eager builds at flush.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 16

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqbf_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }

static void mv(long long seed, float *v)
{ unsigned long long s=(unsigned long long)(seed+1)*2654435761ULL; double n=0; for (int d=0;d<DIM;d++){s=s*6364136223846793005ULL+1; v[d]=(float)((double)((s>>33)&0x7fffffff)/(double)0x7fffffff*2.0-1.0); n+=(double)v[d]*v[d];} n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }

static void fill(ATIRE_segment_index *ix, long long n)
{
float doc[4*DIM]; char key[32], body[64]; long long seed = 0;
for (long long i = 0; i < n; i++)
	{
	long long md = (i%3)+1; for (long long t=0;t<md;t++) mv(seed++, doc+t*DIM);
	sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld body</DOC>", i);
	CHECK(ix->add_document(key, body, NULL, doc, md) >= 0);
	}
CHECK(ix->flush() == 0);
}

int main(void)
{
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
fill(ix, 40);
CHECK(ix->disk_segment_has_multivector_pq(0) == 0);
CHECK(ix->build_multivector_pq() == 0);
CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
delete ix;
ATIRE_segment_index *re = new ATIRE_segment_index();
CHECK(re->open(d) == 0);
CHECK(re->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(re->multivector_pq_configured());
CHECK(re->disk_segment_has_multivector_pq(0) == 1);
delete re; delete [] d;
}
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->build_multivector_pq() == 1);		/* unconfigured -> no-op nonzero */
delete ix; delete [] d;
}
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_policy(1) == 0);
fill(ix, 40);
CHECK(ix->disk_segment_has_multivector_pq(0) == 1);		/* eager built at flush */
delete ix; delete [] d;
}
printf("test_mvpq_backfill PASSED\n");
return 0;
}

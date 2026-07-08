/*
	TEST_MVPQ_COMPACTION.CPP -- compact() rebuilds the merged .mvpq (retrain +
	renumber); search_multivector stays correct; float fallback after .mvpq loss.
	Run under ASan detect_leaks=1 to guard the shuffle-teardown free of
	multivector_pq (the dense-PQ C1 leak lesson).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 16

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqc_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }
static void dv(long long i, float *v){ for(int d=0;d<DIM;d++) v[d]=0.01f*(float)(((i*3+d)%7)-3); v[i%DIM]+=5.0f; double n=0; for(int d=0;d<DIM;d++) n+=(double)v[d]*v[d]; n=sqrt(n)+1e-9; for(int d=0;d<DIM;d++) v[d]=(float)(v[d]/n); }
static void add(ATIRE_segment_index *ix, long long from, long long to){ float t[DIM]; char k[32],b[64]; for(long long i=from;i<to;i++){ dv(i,t); sprintf(k,"doc-%lld",i); sprintf(b,"<DOC>term%lld q</DOC>",i); CHECK(ix->add_document(k,b,NULL,t,1)>=0);} }

int main(void)
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
add(ix, 0, 6); CHECK(ix->flush() == 0);
add(ix, 6, 12); CHECK(ix->flush() == 0);
CHECK(ix->disk_segment_count() == 2);
CHECK(ix->build_multivector_pq() == 0);
CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
CHECK(ix->disk_segment_has_multivector_pq(1) == 1);

float q[DIM]; dv(7, q);
CHECK(ix->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "doc-7") == 0);

long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
CHECK(ix->compact(gens, 2) == 0);
CHECK(ix->disk_segment_count() == 1);
CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
CHECK(ix->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(ix->get_hit(0)->filename, "doc-7") == 0);		/* renumbering correct */

long long out_gen = ix->disk_segment_generation(0);
delete ix;

char mvpq[4096]; snprintf(mvpq, sizeof(mvpq), "%s/seg_%06lld.mvpq", d, out_gen); remove(mvpq);
ATIRE_segment_index *re = new ATIRE_segment_index();
CHECK(re->open(d) == 0);
CHECK(re->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(re->disk_segment_has_multivector_pq(0) == 0);			/* no .mvpq -> float fallback */
CHECK(re->search_multivector(q, 1, 5) >= 1);
CHECK(strcmp(re->get_hit(0)->filename, "doc-7") == 0);		/* still correct via .mvec */
delete re; delete [] d;
printf("test_mvpq_compaction PASSED\n");
return 0;
}

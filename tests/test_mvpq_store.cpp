/*
	TEST_MVPQ_STORE.CPP
	-------------------
	ANT_multivector_pq_store: ragged .mvpq round-trip, forgiving load, ADC-MaxSim
	vs exact-float MaxSim within tolerance, token-pool codebook determinism.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/multivector_pq_store.h"
#include "../source/multivector_store.h"
#include "../source/pq_codec.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define DIM 16

static void norm_vec(long long seed, float *v)
{
unsigned long long s = (unsigned long long)(seed + 1) * 2654435761ULL;
double n = 0.0;
for (int d = 0; d < DIM; d++) { s = s*6364136223846793005ULL+1; v[d] = (float)((double)((s>>33)&0x7fffffff)/(double)0x7fffffff*2.0-1.0); n += (double)v[d]*v[d]; }
n = sqrt(n)+1e-9; for (int d = 0; d < DIM; d++) v[d] = (float)(v[d]/n);
}

static void write_pool(const char *path, long long ndocs)
{
ANT_multivector_pq_store_writer w;
CHECK(w.create(path, DIM, 4, ANT_pq_codec::METRIC_DOT) == 0);
float row[8*DIM];
long long seed = 0;
for (long long d = 0; d < ndocs; d++)
	{
	long long md = (d == 2) ? 0 : (d % 3) + 1;	/* doc 2 has no tokens */
	for (long long t = 0; t < md; t++) norm_vec(seed++, row + t*DIM);
	CHECK(w.append(md > 0 ? row : NULL, md) == 0);
	}
CHECK(w.finish() == 0);
}

static void test_roundtrip_and_ragged(void)
{
char path[] = "/tmp/ant_mvpq_XXXXXX";
int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
const long long N = 12;
write_pool(path, N);

ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, DIM, N, ANT_pq_codec::METRIC_DOT);
CHECK(s->document_count() == N);
CHECK(s->get_m() == 4);
CHECK(s->has(0));
CHECK(!s->has(2));					/* empty doc */
CHECK(s->vector_count(0) == 1);
CHECK(s->vector_count(1) == 2);
CHECK(s->token_docid_of(0) == 0);
long long total = 0; for (long long d = 0; d < N; d++) total += s->vector_count(d);
CHECK(s->token_count() == total);
delete s;
remove(path);
printf("test_roundtrip_and_ragged OK\n");
}

static void test_adc_maxsim_vs_exact(void)
{
char pq_path[] = "/tmp/ant_mvpq_a_XXXXXX";  int a = mkstemp(pq_path); CHECK(a>=0); close(a);
char mv_path[] = "/tmp/ant_mvf_a_XXXXXX";   int b = mkstemp(mv_path); CHECK(b>=0); close(b);
const long long N = 30;

ANT_multivector_pq_store_writer pw; CHECK(pw.create(pq_path, DIM, 4, ANT_pq_codec::METRIC_DOT) == 0);
ANT_multivector_store_writer fw;    CHECK(fw.create(mv_path, DIM) == 0);
float row[8*DIM]; long long seed = 0;
for (long long d = 0; d < N; d++)
	{ long long md = (d%4)+1; for (long long t=0;t<md;t++) norm_vec(seed++, row+t*DIM); CHECK(pw.append(row,md)==0); CHECK(fw.append(row,md)==0); }
CHECK(pw.finish() == 0); CHECK(fw.finish() == 0);

ANT_multivector_pq_store *pq = ANT_multivector_pq_store::load(pq_path, DIM, N, ANT_pq_codec::METRIC_DOT);
ANT_multivector_store *mv = ANT_multivector_store::load(mv_path, DIM, N);
CHECK(pq->token_count() == mv->token_count());

float q[3*DIM]; norm_vec(9999, q); norm_vec(9998, q+DIM); norm_vec(9997, q+2*DIM);
double max_abs_err = 0.0;
for (long long d = 0; d < N; d++)
	{
	double e = mv->maxsim(d, q, 3), a2 = pq->maxsim(d, q, 3);
	double err = fabs(e - a2); if (err > max_abs_err) max_abs_err = err;
	}
printf("  ADC-MaxSim max abs err vs exact = %.4f\n", max_abs_err);
CHECK(max_abs_err < 0.30);
delete pq; delete mv; remove(pq_path); remove(mv_path);
printf("test_adc_maxsim_vs_exact OK\n");
}

static void test_determinism_and_forgiving(void)
{
char p1[] = "/tmp/ant_mvpq_d1_XXXXXX"; int a=mkstemp(p1); CHECK(a>=0); close(a);
char p2[] = "/tmp/ant_mvpq_d2_XXXXXX"; int b=mkstemp(p2); CHECK(b>=0); close(b);
write_pool(p1, 20); write_pool(p2, 20);
FILE *f1 = fopen(p1,"rb"), *f2 = fopen(p2,"rb");
fseek(f1,0,SEEK_END); fseek(f2,0,SEEK_END); long l1=ftell(f1), l2=ftell(f2);
CHECK(l1 == l2 && l1 > 52);
fseek(f1,0,SEEK_SET); fseek(f2,0,SEEK_SET);
unsigned char *b1=(unsigned char*)malloc(l1), *b2=(unsigned char*)malloc(l2);
CHECK(fread(b1,1,l1,f1)==(size_t)l1); CHECK(fread(b2,1,l2,f2)==(size_t)l2);
CHECK(memcmp(b1,b2,l1) == 0);
fclose(f1); fclose(f2); free(b1); free(b2);

ANT_multivector_pq_store *miss = ANT_multivector_pq_store::load("/tmp/ant_mvpq_nope_zzz", DIM, 20, ANT_pq_codec::METRIC_DOT);
CHECK(miss->token_count() == 0 && miss->document_count() == 0); delete miss;

FILE *tf = fopen(p1,"rb+"); CHECK(tf!=NULL); CHECK(ftruncate(fileno(tf), 40)==0); fclose(tf);
ANT_multivector_pq_store *trunc = ANT_multivector_pq_store::load(p1, DIM, 20, ANT_pq_codec::METRIC_DOT);
CHECK(trunc->token_count() == 0); delete trunc;

ANT_multivector_pq_store *wd = ANT_multivector_pq_store::load(p2, DIM+1, 20, ANT_pq_codec::METRIC_DOT);
CHECK(wd->token_count() == 0); delete wd;

remove(p1); remove(p2);
printf("test_determinism_and_forgiving OK\n");
}

int main(void)
{
test_roundtrip_and_ragged();
test_adc_maxsim_vs_exact();
test_determinism_and_forgiving();
printf("test_mvpq_store PASSED\n");
return 0;
}

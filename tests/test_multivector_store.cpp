/*
	TEST_MULTIVECTOR_STORE.CPP
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "multivector_store.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static double ref_maxsim(const float *doc, long long m, const float *q, long long n, long long dim)
{
double total = 0.0;
for (long long i = 0; i < n; i++)
	{
	double best = -1e30;
	for (long long j = 0; j < m; j++)
		{
		double dot = 0.0;
		for (long long d = 0; d < dim; d++) dot += (double)q[i*dim+d] * (double)doc[j*dim+d];
		if (dot > best) best = dot;
		}
	total += best;
	}
return total;
}

static void l2norm(float *v, long long dim)
{
double s = 0.0; for (long long d = 0; d < dim; d++) s += (double)v[d]*v[d];
s = sqrt(s); if (s > 0) for (long long d = 0; d < dim; d++) v[d] = (float)(v[d]/s);
}

static void roundtrip_and_maxsim(int quantized)
{
long long dim = 16, ndocs = 5, i;
long long counts[5] = { 3, 0, 1, 4, 2 };
long long total = 0; for (i = 0; i < ndocs; i++) total += counts[i];
float *all = new float[total * dim];
srand(7);
for (i = 0; i < total * dim; i++) all[i] = (float)(rand()%2000-1000)/500.0f;
for (i = 0; i < total; i++) l2norm(all + i*dim, dim);

char path[64]; strcpy(path, "/tmp/ant_mvec_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_multivector_store_writer w;
CHECK(w.create(path, dim) == 0);
if (quantized) w.set_quantization(ANT_multivector_store_writer::QUANT_INT8);
long long off = 0;
for (i = 0; i < ndocs; i++)
	{ CHECK(w.append(counts[i] ? all + off*dim : NULL, counts[i]) == 0); off += counts[i]; }
CHECK(w.finish() == 0);

ANT_multivector_store *s = ANT_multivector_store::load(path, dim, ndocs);
CHECK(s != NULL);
CHECK(s->document_count() == ndocs);
CHECK(s->get_dimension() == dim);
CHECK(!s->has(1) && s->vector_count(1) == 0);
CHECK(s->has(0) && s->vector_count(0) == 3);
CHECK(s->has(3) && s->vector_count(3) == 4);

float q[3*16]; for (i = 0; i < 3*16; i++) q[i] = (float)(rand()%2000-1000)/500.0f;
for (i = 0; i < 3; i++) l2norm(q + i*dim, dim);
off = 0;
for (i = 0; i < ndocs; i++)
	{
	double got = s->maxsim(i, q, 3);
	double ref = counts[i] ? ref_maxsim(all + off*dim, counts[i], q, 3, dim) : 0.0;
	if (quantized) CHECK(fabs(got - ref) < 0.05 * 3);
	else CHECK(fabs(got - ref) < 1e-4);
	off += counts[i];
	}
CHECK(s->maxsim(1, q, 3) == 0.0);
delete s; delete [] all; unlink(path);
printf("roundtrip_and_maxsim(quant=%d) OK\n", quantized);
}

static void load_validation(void)
{
char p[64]; strcpy(p, "/tmp/ant_mvbad_XXXXXX"); { int fd=mkstemp(p); if(fd>=0) close(fd); }
FILE *f = fopen(p, "wb"); fputs("not a multivector store", f); fclose(f);
ANT_multivector_store *bad = ANT_multivector_store::load(p, 16, 5);
CHECK(bad != NULL && !bad->has(0) && bad->document_count() == 0);
delete bad;

/* size bomb: REAL magic + valid dim but total_vectors = 2^40 with nothing behind it */
f = fopen(p, "wb");
unsigned long long magic = 0;
memcpy(&magic, "ANTMVEC1", 8);					/* the exact 8-byte magic your writer emits */
long long dim = 16, docs = 4, total = 1LL << 40; int quant = 0;
fwrite(&magic, 8, 1, f); fwrite(&dim, 8, 1, f); fwrite(&docs, 8, 1, f);
fwrite(&total, 8, 1, f); fwrite(&quant, 4, 1, f);
fclose(f);
ANT_multivector_store *bomb = ANT_multivector_store::load(p, 16, 4);
CHECK(bomb != NULL && !bomb->has(0));			/* size check rejects; degrades to empty (no huge alloc) */
delete bomb; unlink(p);
printf("load_validation OK\n");
}

static void maxsim_edges(void)
{
/* n=0 query -> score 0; single normalized doc vec vs identical query vec -> exact dot 1.0 */
long long dim = 8;
char p[64]; strcpy(p, "/tmp/ant_mv1_XXXXXX"); { int fd=mkstemp(p); if(fd>=0) close(fd); }
float v[8] = {1,0,0,0,0,0,0,0};
ANT_multivector_store_writer w; CHECK(w.create(p, dim) == 0);
CHECK(w.append(v, 1) == 0); CHECK(w.finish() == 0);
ANT_multivector_store *s = ANT_multivector_store::load(p, dim, 1);
CHECK(s != NULL && s->has(0));
float q[8] = {1,0,0,0,0,0,0,0};
CHECK(fabs(s->maxsim(0, q, 1) - 1.0) < 1e-6);		/* identical normalized -> 1 */
CHECK(s->maxsim(0, q, 0) == 0.0);					/* no query vectors -> 0 */
CHECK(s->maxsim(5, q, 1) == 0.0);					/* out-of-range docid -> 0 */
delete s; unlink(p);
printf("maxsim_edges OK\n");
}

int main(void)
{
roundtrip_and_maxsim(0);
roundtrip_and_maxsim(1);
load_validation();
maxsim_edges();
printf("PASSED\n");
return 0;
}

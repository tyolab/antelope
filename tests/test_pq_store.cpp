/*
	TEST_PQ_STORE.CPP
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_store.h"
#include "../source/pq_codec.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static double dotf(const float *a, const float *b, long long dim)
{
double s = 0.0;
for (long long d = 0; d < dim; d++) s += (double)a[d] * (double)b[d];
return s;
}

static void build_and_roundtrip(void)
{
long long dim = 16, m = 4, i;
float docs[4][16];

srand(42);
for (i = 0; i < 4; i++)
	for (long long d = 0; d < dim; d++)
		docs[i][d] = (float)(rand() % 2000 - 1000) / 500.0f;

char path[64]; strcpy(path, "/tmp/ant_pq_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }

ANT_pq_store_writer w;
CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT) == 0);
CHECK(w.append(docs[0]) == 0);
CHECK(w.append(docs[1]) == 0);
CHECK(w.append(docs[2]) == 0);
CHECK(w.append(NULL) == 0);		// absent doc 3
CHECK(w.finish() == 0);

ANT_pq_store *s = ANT_pq_store::load(path, dim, 4, ANT_pq_codec::METRIC_DOT);
CHECK(s);
CHECK(s->document_count() == 4);
CHECK(s->get_dimension() == 16);
CHECK(s->get_m() == 4);

CHECK(s->has(0) && s->has(2) && !s->has(3) && !s->has(4));

float out[16];
s->reconstruct(0, out);

/* PQ is lossy; check reconstruction is in the right ballpark via cosine similarity. */
double dot_recon_doc0 = dotf(out, docs[0], dim);
double norm_recon = sqrt(dotf(out, out, dim));
double norm_doc0 = sqrt(dotf(docs[0], docs[0], dim));
CHECK(norm_recon > 0.0 && norm_doc0 > 0.0);
double cosine = dot_recon_doc0 / (norm_recon * norm_doc0);
CHECK(cosine > 0.7);		// generous tolerance -- PQ is lossy but should be strongly aligned

/* ADC score should match dot(doc0, reconstruct(doc0)) within a small tolerance. */
double adc = s->score(0, docs[0], ANT_pq_codec::METRIC_DOT);
CHECK(fabs(adc - dot_recon_doc0) < 1e-4);

delete s;
unlink(path);
printf("build_and_roundtrip OK\n");
}

static void forgiving_load(void)
{
long long dim = 16, m = 4, i;
float docs[4][16];

srand(43);
for (i = 0; i < 4; i++)
	for (long long d = 0; d < dim; d++)
		docs[i][d] = (float)(rand() % 2000 - 1000) / 500.0f;

char path[64]; strcpy(path, "/tmp/ant_pq2_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }

ANT_pq_store_writer w;
CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT) == 0);
CHECK(w.append(docs[0]) == 0);
CHECK(w.append(docs[1]) == 0);
CHECK(w.append(docs[2]) == 0);
CHECK(w.append(NULL) == 0);
CHECK(w.finish() == 0);

/* missing file */
ANT_pq_store *missing = ANT_pq_store::load("/tmp/nope_pq_does_not_exist", dim, 4, ANT_pq_codec::METRIC_DOT);
CHECK(missing != NULL && missing->document_count() == 0);
delete missing;

/* truncated file */
char trunc_path[64]; strcpy(trunc_path, "/tmp/ant_pq3_XXXXXX"); { int fd = mkstemp(trunc_path); if (fd >= 0) close(fd); }
{
FILE *in = fopen(path, "rb");
FILE *out_f = fopen(trunc_path, "wb");
char buf[20];
size_t got = fread(buf, 1, 20, in);
fwrite(buf, 1, got, out_f);
fclose(in);
fclose(out_f);
}
ANT_pq_store *trunc = ANT_pq_store::load(trunc_path, dim, 4, ANT_pq_codec::METRIC_DOT);
CHECK(trunc != NULL && trunc->document_count() == 0);
delete trunc;
unlink(trunc_path);

/* wrong dimension */
ANT_pq_store *wrong_dim = ANT_pq_store::load(path, 8, 4, ANT_pq_codec::METRIC_DOT);
CHECK(wrong_dim != NULL && wrong_dim->document_count() == 0);
delete wrong_dim;

/* wrong document count */
ANT_pq_store *wrong_docs = ANT_pq_store::load(path, dim, 99, ANT_pq_codec::METRIC_DOT);
CHECK(wrong_docs != NULL && wrong_docs->document_count() == 0);
delete wrong_docs;

unlink(path);
printf("forgiving_load OK\n");
}

int main(void)
{
build_and_roundtrip();
forgiving_load();
printf("test_pq_store PASSED\n");
return 0;
}

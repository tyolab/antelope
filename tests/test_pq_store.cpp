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
#include "../source/vector_store.h"
#include "../source/index_tombstones.h"

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

/* bad magic: a byte-identical-size copy with the 8 magic bytes clobbered -> degraded empty */
char badmagic_path[64]; strcpy(badmagic_path, "/tmp/ant_pqbm_XXXXXX"); { int fd = mkstemp(badmagic_path); if (fd >= 0) close(fd); }
{
FILE *in = fopen(path, "rb"); FILE *out_f = fopen(badmagic_path, "wb");
CHECK(in != NULL && out_f != NULL);
fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, SEEK_SET);
char *buf = new char[sz]; CHECK(fread(buf, 1, sz, in) == (size_t)sz);
memcpy(buf, "BADMAGIC", 8);				/* clobber magic, keep size */
CHECK(fwrite(buf, 1, sz, out_f) == (size_t)sz);
delete [] buf; fclose(in); fclose(out_f);
}
ANT_pq_store *bad_magic = ANT_pq_store::load(badmagic_path, dim, 4, ANT_pq_codec::METRIC_DOT);
CHECK(bad_magic != NULL && bad_magic->document_count() == 0);
delete bad_magic; unlink(badmagic_path);

/* size-consistent invalid m: same file size, but the header's m field flipped to 6 (16 % 6 != 0).
   Rejection here comes from the m-divides-dimension field check, NOT the exact-size gate. */
char badm_path[64]; strcpy(badm_path, "/tmp/ant_pqim_XXXXXX"); { int fd = mkstemp(badm_path); if (fd >= 0) close(fd); }
{
FILE *in = fopen(path, "rb"); FILE *out_f = fopen(badm_path, "wb");
CHECK(in != NULL && out_f != NULL);
fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, SEEK_SET);
char *buf = new char[sz]; CHECK(fread(buf, 1, sz, in) == (size_t)sz);
long long bad_m = 6; memcpy(buf + 28, &bad_m, 8);	/* m field at header offset 28; size unchanged */
CHECK(fwrite(buf, 1, sz, out_f) == (size_t)sz);
delete [] buf; fclose(in); fclose(out_f);
}
ANT_pq_store *bad_m = ANT_pq_store::load(badm_path, dim, 4, ANT_pq_codec::METRIC_DOT);
CHECK(bad_m != NULL && bad_m->document_count() == 0);
delete bad_m; unlink(badm_path);

unlink(path);
printf("forgiving_load OK\n");
}

static void scan_adc_test(void)
{
long long dim = 16, m = 4, n = 8, i;
float docs[8][16];

srand(101);
for (i = 0; i < n; i++)
	for (long long d = 0; d < dim; d++)
		docs[i][d] = (float)(rand() % 2000 - 1000) / 500.0f;

char path[64]; strcpy(path, "/tmp/ant_pq_scan_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }

ANT_pq_store_writer w;
CHECK(w.create(path, dim, m, ANT_pq_codec::METRIC_DOT) == 0);
for (i = 0; i < n; i++)
	CHECK(w.append(docs[i]) == 0);
CHECK(w.finish() == 0);

ANT_pq_store *s = ANT_pq_store::load(path, dim, n, ANT_pq_codec::METRIC_DOT);
CHECK(s);
CHECK(s->document_count() == n);

float query[16];
srand(202);
for (long long d = 0; d < dim; d++) query[d] = (float)(rand() % 2000 - 1000) / 500.0f;

/* independent brute-force ranking via score() -- authoritative expectation */
double scores[8];
for (i = 0; i < n; i++) scores[i] = s->score(i, query, ANT_pq_codec::METRIC_DOT);
long long ranked[8];
for (i = 0; i < n; i++) ranked[i] = i;
for (i = 0; i < n; i++)
	for (long long j = i + 1; j < n; j++)
		if (scores[ranked[j]] > scores[ranked[i]])
			{ long long t = ranked[i]; ranked[i] = ranked[j]; ranked[j] = t; }

/* plain top-3 scan */
{
ANT_vector_candidate best[3]; long long count = 0;
s->scan_adc(query, ANT_pq_codec::METRIC_DOT, NULL, 7 /*gen*/, best, &count, 3, NULL);
CHECK(count == 3);
for (i = 0; i < 3; i++)
	{
	CHECK(best[i].generation == 7);
	long long found = 0, j;
	for (j = 0; j < 3; j++) if (best[i].docid == ranked[j]) found = 1;
	CHECK(found);
	}
}

/* filter case: admit only two docids -> only those two come back */
{
unsigned char filter_bits[1] = { 0 };
long long admit_a = ranked[1], admit_b = ranked[4];		// arbitrary pair, not the global top
filter_bits[0] = (unsigned char)((1 << admit_a) | (1 << admit_b));
ANT_vector_candidate best[3]; long long count = 0;
s->scan_adc(query, ANT_pq_codec::METRIC_DOT, NULL, 7, best, &count, 3, filter_bits);
CHECK(count == 2);
CHECK(best[0].docid == admit_a || best[0].docid == admit_b);
CHECK(best[1].docid == admit_a || best[1].docid == admit_b);
CHECK(best[0].docid != best[1].docid);
}

/* tombstone case: delete the top-ranked doc -> it must not appear in top-3 */
{
ANT_index_tombstones stones(s->document_count());
stones.set_deleted(ranked[0]);
ANT_vector_candidate best[3]; long long count = 0;
s->scan_adc(query, ANT_pq_codec::METRIC_DOT, &stones, 7, best, &count, 3, NULL);
CHECK(count == 3);
for (i = 0; i < 3; i++)
	{
	CHECK(best[i].docid != ranked[0]);
	long long found = 0, j;
	for (j = 1; j <= 3; j++) if (best[i].docid == ranked[j]) found = 1;
	CHECK(found);
	}
}

delete s;
unlink(path);
printf("scan_adc_test OK\n");
}

int main(void)
{
build_and_roundtrip();
forgiving_load();
scan_adc_test();
printf("test_pq_store PASSED\n");
return 0;
}

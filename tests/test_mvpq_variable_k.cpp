/*
	TEST_MVPQ_VARIABLE_K.CPP -- token epic 3/4 Task 1: .mvpq v3 variable code-width.
	A writer created at k<256 produces a v3 sidecar with bit-packed rows
	(row_bytes = (m*bits+7)/8); it round-trips (reconstruct/maxsim sane) and a
	k==256 writer stays byte-identical to the pre-feature v1/v2 layout.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define DIM 8
#define MM 4			/* sub = 2 */

static void fill(long long seed, float *v)
{
	double n = 0;
	for (int j = 0; j < DIM; j++) { v[j] = (float)(((seed*7 + j*3) % 13) - 6) / 6.0f; n += v[j]*v[j]; }
	n = sqrt(n) + 1e-9;
	for (int j = 0; j < DIM; j++) v[j] /= (float)n;
}

/* build a .mvpq at the given k over NDOC docs (2 tokens each); return path via out */
static void build(const char *path, long long k, long long ndoc)
{
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, DIM, MM, k, ANT_pq_codec::METRIC_DOT, 0) == 0);
	for (long long d = 0; d < ndoc; d++)
		{
		float rows[2*DIM];
		fill(d*3 + 0, rows); fill(d*3 + 1, rows + DIM);
		CHECK(w.append(rows, 2) == 0);
		}
	CHECK(w.finish() == 0);
}

static void test_variable_k_roundtrip(void)
{
	char path[] = "/tmp/ant_mvk_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	const long long k = 16, ndoc = 20;
	build(path, k, ndoc);

	/* lock the packed v3 on-disk contract (mirror of test_k256_byte_identical for v3):
	   version 3 at offset 8, and total size == the v3 expected_size for k=16.
	   v3 header is 60 (always carries the opq i64); opq off -> no R block.
	   row_bytes = (MM*bits_for_k(16)+7)/8 = (4*4+7)/8 = 2. */
	{
	FILE *fp = fopen(path, "rb"); CHECK(fp != NULL);
	unsigned int version; fseek(fp, 8, SEEK_SET); CHECK(fread(&version, 4, 1, fp) == 1 && version == 3u);
	fseek(fp, 0, SEEK_END);
	long long actual = ftell(fp);
	long long bits = ANT_pq_codec::bits_for_k(k);
	long long row_bytes = (MM*bits + 7) / 8;
	CHECK(row_bytes == 2);
	long long toks = 2*ndoc;
	long long expect = 60 + ndoc*4 + toks*row_bytes + k*DIM*4;	/* header(60) + counts + packed codes + k*dim codebook, opq off */
	CHECK(actual == expect);
	fclose(fp);
	}

	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, DIM, ndoc, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == 2*ndoc);
	CHECK(s->get_k() == k);
	/* reconstruct error is bounded (k=16 over 8 dims is lossy but finite) */
	float probe[2*DIM]; fill(0, probe); fill(1, probe + DIM);
	float rec[DIM]; s->token_reconstruct(0, rec);
	double err = 0; for (int j = 0; j < DIM; j++) { double d = rec[j] - probe[j]; err += d*d; }
	CHECK(err < 1.0);							/* not garbage */
	/* maxsim of doc 0 against its own two tokens is positive and finite */
	double ms = s->maxsim(0, probe, 2);
	CHECK(ms > 0.0 && ms < 100.0);
	remove(path);
	printf("test_variable_k_roundtrip OK\n");
}

/* k==256 writer must be byte-identical to the legacy v1 layout: version==1,
   row_bytes==m, codebook 256*dim floats. Assert the on-disk header + size. */
static void test_k256_byte_identical(void)
{
	char path[] = "/tmp/ant_mvk256_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	const long long ndoc = 12;
	build(path, 256, ndoc);

	FILE *fp = fopen(path, "rb"); CHECK(fp != NULL);
	char magic[8]; unsigned int version; long long dim, docs, toks, m, k;
	CHECK(fread(magic, 1, 8, fp) == 8 && memcmp(magic, "ANTMVPQ1", 8) == 0);
	CHECK(fread(&version, 4, 1, fp) == 1 && version == 1u);		/* k==256, no opq -> v1 */
	CHECK(fread(&dim, 8, 1, fp) == 1 && dim == DIM);
	CHECK(fread(&docs, 8, 1, fp) == 1 && docs == ndoc);
	CHECK(fread(&toks, 8, 1, fp) == 1 && toks == 2*ndoc);
	CHECK(fread(&m, 8, 1, fp) == 1 && m == MM);
	CHECK(fread(&k, 8, 1, fp) == 1 && k == 256);
	fseek(fp, 0, SEEK_END);
	long long actual = ftell(fp);
	/* v1 header 52 + counts + codes(toks*m, row_bytes==m) + codebook(256*dim) */
	long long expect = 52 + docs*4 + toks*m + 256*dim*4;
	CHECK(actual == expect);
	fclose(fp);
	remove(path);
	printf("test_k256_byte_identical OK\n");
}

int main(void)
{
	test_variable_k_roundtrip();
	test_k256_byte_identical();
	printf("ALL test_mvpq_variable_k PASSED\n");
	return 0;
}

/*
	TEST_PQ_GLOBAL.CPP -- #22.2 global codebook. Task 1 locks the writer's
	external-codebook seam: finish() with a supplied codebook skips training,
	encodes against it, and embeds it (so load round-trips identically).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

// Train a standalone codebook over `n` D-dim rows (no OPQ), for use as an "external" codebook.
static float *train_codebook(const float *vecs, long long D, long long m, long long n)
{
	long long sub = D / m, floats = m * (long long)ANT_pq_codec::K * sub;
	float *cb = new float[floats];
	CHECK(ANT_pq_codec::train(vecs, D, m, n, cb) == 0);
	return cb;
}

static void test_external_codebook_encodes_and_embeds(void)
{
	const long long D = 8, m = 4, n = 50;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*7 + d*13) % 17) - 8);
	float *ext_cb = train_codebook(vecs, D, m, n);

	char path[] = "/tmp/ant_gcb_XXXXXX"; CHECK(mkstemp(path) >= 0);
	ANT_pq_store_writer w;
	CHECK(w.create(path, D, m, ANT_pq_codec::METRIC_L2, 0) == 0);
	w.set_external_codebook(ext_cb, NULL);              // supply codebook, no OPQ rotation
	for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
	CHECK(w.finish() == 0);

	ANT_pq_store *s = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2);
	CHECK(s != NULL && s->document_count() == n);
	// the embedded codebook must equal the supplied one (finish did not retrain)
	CHECK(memcmp(s->get_codebook(), ext_cb, (size_t)(m*(long long)ANT_pq_codec::K*(D/m))*sizeof(float)) == 0);
	// codes must equal a direct encode against the same codebook
	for (long long doc = 0; doc < n; doc++)
		{
		unsigned char expect[4];
		ANT_pq_codec::encode(vecs + doc*D, D, m, ext_cb, expect);
		CHECK(memcmp(s->codes_for(doc), expect, (size_t)m) == 0);
		}
	delete s; remove(path); delete [] ext_cb; delete [] vecs;
	printf("test_external_codebook_encodes_and_embeds OK\n");
}

static void test_non_external_path_unchanged(void)
{
	// A writer with NO external codebook trains its own — same file two runs => byte-identical (deterministic).
	const long long D = 6, m = 3, n = 40;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*5 + d*11) % 13) - 6);
	char a[] = "/tmp/ant_gna_XXXXXX", b[] = "/tmp/ant_gnb_XXXXXX";
	CHECK(mkstemp(a) >= 0); CHECK(mkstemp(b) >= 0);
	for (int pass = 0; pass < 2; pass++)
		{
		ANT_pq_store_writer w;
		CHECK(w.create(pass ? b : a, D, m, ANT_pq_codec::METRIC_L2, 0) == 0);
		for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
		CHECK(w.finish() == 0);
		}
	FILE *fa = fopen(a,"rb"), *fb = fopen(b,"rb"); CHECK(fa && fb);
	fseek(fa,0,SEEK_END); long la = ftell(fa); fseek(fb,0,SEEK_END); long lb = ftell(fb);
	CHECK(la == lb && la > 0); rewind(fa); rewind(fb);
	unsigned char *ba = new unsigned char[la], *bb = new unsigned char[lb];
	CHECK(fread(ba,1,la,fa)==(size_t)la && fread(bb,1,lb,fb)==(size_t)lb);
	CHECK(memcmp(ba, bb, la) == 0);
	fclose(fa); fclose(fb); delete[] ba; delete[] bb;
	remove(a); remove(b); delete [] vecs;
	printf("test_non_external_path_unchanged OK\n");
}

int main(void)
{
	test_external_codebook_encodes_and_embeds();
	test_non_external_path_unchanged();
	printf("ALL TESTS PASSED\n");
	return 0;
}

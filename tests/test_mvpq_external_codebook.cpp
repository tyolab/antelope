/*
	TEST_MVPQ_EXTERNAL_CODEBOOK.CPP -- token global-codebook groundwork
	(token epic 2/4): a writer handed an external codebook (+optional R)
	skips training, encodes/embeds against it, and loads back identically;
	the non-external path stays byte-identical (deterministic).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void fill(float *v, long long n, unsigned seed)
{ srand(seed); for (long long i = 0; i < n; i++) v[i] = (float)(((rand()%2000)-1000)/500.0); }

/* train a standalone codebook over a flat token pool (no OPQ) to use as "external" */
static float *train_ext(const float *pool, long long D, long long m, long long ntok)
{
	float *cb = new float[m * (long long)ANT_pq_codec::K * (D/m)];
	CHECK(ANT_pq_codec::train(pool, D, m, ANT_pq_codec::K, ntok, cb) == 0);
	return cb;
}

static void test_external_encodes_and_embeds(void)
{
	const long long D = 8, m = 4;
	int counts[3] = { 10, 6, 8 }; long long ntok = 24;
	float *pool = new float[ntok * D]; fill(pool, ntok*D, 5);
	float *ext = train_ext(pool, D, m, ntok);

	char path[] = "/tmp/mvpq_ext_XXXXXX"; CHECK(mkstemp(path) >= 0);
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, D, m, 256, ANT_pq_codec::METRIC_DOT, /*opq*/0) == 0);
	w.set_external_codebook(ext, NULL);					// supply codebook, no OPQ rotation
	long long off = 0;
	for (int d = 0; d < 3; d++) { CHECK(w.append(pool + off*D, counts[d]) == 0); off += counts[d]; }
	CHECK(w.finish() == 0);

	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, D, 3, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == ntok);
	// embedded codebook must equal the supplied one (finish did not retrain)
	long long cb_floats = m * (long long)ANT_pq_codec::K * (D/m);
	CHECK(memcmp(s->get_codebook(), ext, (size_t)cb_floats * sizeof(float)) == 0);
	// each token's code must equal a direct encode against ext
	for (long long t = 0; t < ntok; t++)
		{
		unsigned char expect[4];
		ANT_pq_codec::encode(pool + t*D, D, m, ANT_pq_codec::K, ext, expect);
		CHECK(memcmp(s->token_codes(t), expect, (size_t)m) == 0);
		}
	delete s; remove(path); delete [] ext; delete [] pool;
	printf("test_external_encodes_and_embeds OK\n");
}

static void test_non_external_unchanged(void)
{
	// no external codebook -> trains its own -> two runs byte-identical (deterministic).
	const long long D = 6, m = 3;
	int counts[2] = { 8, 8 }; long long ntok = 16;
	float *pool = new float[ntok * D]; fill(pool, ntok*D, 9);
	char a[] = "/tmp/mvpq_na_XXXXXX", b[] = "/tmp/mvpq_nb_XXXXXX";
	CHECK(mkstemp(a) >= 0); CHECK(mkstemp(b) >= 0);
	for (int pass = 0; pass < 2; pass++)
		{
		ANT_multivector_pq_store_writer w;
		CHECK(w.create(pass ? b : a, D, m, 256, ANT_pq_codec::METRIC_DOT, 0) == 0);
		long long off = 0;
		for (int d = 0; d < 2; d++) { CHECK(w.append(pool + off*D, counts[d]) == 0); off += counts[d]; }
		CHECK(w.finish() == 0);
		}
	FILE *fa = fopen(a,"rb"), *fb = fopen(b,"rb"); CHECK(fa && fb);
	fseek(fa,0,SEEK_END); long la = ftell(fa); fseek(fb,0,SEEK_END); long lb = ftell(fb);
	CHECK(la == lb && la > 0); rewind(fa); rewind(fb);
	unsigned char *ba = new unsigned char[la], *bb = new unsigned char[lb];
	CHECK(fread(ba,1,la,fa)==(size_t)la && fread(bb,1,lb,fb)==(size_t)lb);
	CHECK(memcmp(ba, bb, la) == 0);
	fclose(fa); fclose(fb); delete[] ba; delete[] bb; remove(a); remove(b); delete [] pool;
	printf("test_non_external_unchanged OK\n");
}

int main(void)
{
	test_external_encodes_and_embeds();
	test_non_external_unchanged();
	printf("ALL test_mvpq_external_codebook PASSED\n");
	return 0;
}

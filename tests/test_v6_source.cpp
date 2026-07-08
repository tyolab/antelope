/*
	TEST_V6_SOURCE.CPP
	------------------
	Exercises ANT_multivector_source: the .mvec token pool exposed as an
	ANT_vector_source so ANT_hnsw can index individual tokens (node == token
	index in the flattened pool).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/multivector_store.h"
#include "../source/vector_store.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void l2norm(float *v, long long dim)
{
double s = 0.0;
for (long long d = 0; d < dim; d++) s += (double)v[d] * v[d];
s = sqrt(s);
if (s > 0) for (long long d = 0; d < dim; d++) v[d] = (float)(v[d] / s);
}

static double dot(const float *a, const float *b, long long dim)
{
double s = 0.0;
for (long long d = 0; d < dim; d++) s += (double)a[d] * (double)b[d];
return s;
}

/*
	build_store()
	-------------
	dim=4, doc0 has 2 tokens, doc1 has 3 tokens (5 tokens total).
	Returns the raw (pre-normalization) token vectors via `raw_out` (5*4 floats)
	so the caller can compute expected normalized dot products.
*/
static void build_store(const char *path, int quantize, float *raw_out)
{
long long dim = 4;
float doc0[2*4] =
	{
	1, 0, 0, 0,
	0, 1, 0, 0
	};
float doc1[3*4] =
	{
	0, 0, 1, 0,
	0, 0, 0, 1,
	1, 1, 0, 0
	};

memcpy(raw_out + 0*dim, doc0, sizeof(doc0));
memcpy(raw_out + 2*dim, doc1, sizeof(doc1));

ANT_multivector_store_writer w;
CHECK(w.create(path, dim) == 0);
if (quantize)
	w.set_quantization(ANT_multivector_store_writer::QUANT_INT8);
CHECK(w.append(doc0, 2) == 0);
CHECK(w.append(doc1, 3) == 0);
CHECK(w.finish() == 0);
}

static void float_source_test(void)
{
long long dim = 4, ndocs = 2, ntokens = 5, i;
float raw[5*4];
char path[64]; strcpy(path, "/tmp/ant_v6src_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }

build_store(path, 0, raw);

ANT_multivector_store *store = ANT_multivector_store::load(path, dim, ndocs);
CHECK(store != NULL);

ANT_multivector_source src(store);

CHECK(src.document_count() == ntokens);
CHECK(src.get_dimension() == dim);
CHECK(src.has(0) && src.has(4) && !src.has(5) && !src.has(-1));
CHECK(!src.is_quantized());

/* expected normalized token vectors */
float normed[5*4];
memcpy(normed, raw, sizeof(normed));
for (i = 0; i < ntokens; i++) l2norm(normed + i*dim, dim);

float q[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
for (i = 0; i < ntokens; i++)
	{
	double expected = dot(q, normed + i*dim, dim);
	double got = src.score(i, q, ANT_vector_store::METRIC_DOT);
	CHECK(fabs(got - expected) < 1e-5);
	}

/* token_get should return the raw float pool pointer for a float store */
const float *tok2 = store->token_get(2);
CHECK(tok2 != NULL);
for (i = 0; i < dim; i++)
	CHECK(fabs(tok2[i] - normed[2*dim + i]) < 1e-6);

/* reconstruct matches token_get for the float case */
float rec[4];
store->token_reconstruct(2, rec);
for (i = 0; i < dim; i++)
	CHECK(fabs(rec[i] - normed[2*dim + i]) < 1e-6);

/* token_docid_of */
CHECK(store->token_docid_of(0) == 0);
CHECK(store->token_docid_of(1) == 0);
CHECK(store->token_docid_of(2) == 1);
CHECK(store->token_docid_of(4) == 1);
CHECK(store->token_docid_of(5) == -1);
CHECK(store->token_docid_of(-1) == -1);

delete store;
unlink(path);
printf("float_source_test OK\n");
}

static void quantized_source_test(void)
{
long long dim = 4, ndocs = 2, ntokens = 5, i;
float raw[5*4];
char path[64]; strcpy(path, "/tmp/ant_v6srcq_XXXXXX"); { int fd = mkstemp(path); if (fd >= 0) close(fd); }

build_store(path, 1, raw);

ANT_multivector_store *store = ANT_multivector_store::load(path, dim, ndocs);
CHECK(store != NULL);

ANT_multivector_source src(store);

CHECK(src.document_count() == ntokens);
CHECK(src.is_quantized());
CHECK(src.get(0) == NULL);		/* no raw float pointer when quantized */

float normed[5*4];
memcpy(normed, raw, sizeof(normed));
for (i = 0; i < ntokens; i++) l2norm(normed + i*dim, dim);

float q[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
for (i = 0; i < ntokens; i++)
	{
	double expected = dot(q, normed + i*dim, dim);
	double got = src.score(i, q, ANT_vector_store::METRIC_DOT);
	CHECK(fabs(got - expected) < 0.05);		/* int8 quantization tolerance */
	}

float rec[4];
src.reconstruct(3, rec);
for (i = 0; i < dim; i++)
	CHECK(fabs(rec[i] - normed[3*dim + i]) < 0.05);

delete store;
unlink(path);
printf("quantized_source_test OK\n");
}

int main(void)
{
float_source_test();
quantized_source_test();
printf("PASSED\n");
return 0;
}

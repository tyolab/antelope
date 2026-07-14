/*
	MULTIVECTOR_PQ_STORE.CPP -- see header. Ragged PQ token pool, reuses ANT_pq_codec.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multivector_pq_store.h"
#include "pq_codec.h"

ANT_multivector_pq_store::ANT_multivector_pq_store() :
	dimension(0), documents(0), total_tokens(0), m(0), metric(0),
	counts(0), offsets(0), codebook(0), codes(0), adc_table_builds(0) {}

ANT_multivector_pq_store::~ANT_multivector_pq_store()
{
delete [] counts; delete [] offsets; delete [] codebook; delete [] codes;
}

long long ANT_multivector_pq_store::max_vector_count(void)
{
long long best = 0;
for (long long d = 0; d < documents; d++) if (counts[d] > best) best = counts[d];
return best;
}

long long ANT_multivector_pq_store::token_docid_of(long long t)
{
if (!token_has(t)) return -1;
long long lo = 0, hi = documents;
while (lo + 1 < hi) { long long mid = (lo+hi)/2; if (offsets[mid] <= t) lo = mid; else hi = mid; }
return lo;
}

void ANT_multivector_pq_store::token_reconstruct(long long t, float *out)
{
if (!token_has(t)) { memset(out, 0, (size_t)(dimension*sizeof(float))); return; }
ANT_pq_codec::reconstruct(codes + t*m, dimension, m, ANT_pq_codec::K, codebook, out);
}

double ANT_multivector_pq_store::token_score(long long t, const float *query, long)
{
if (!token_has(t)) return 0.0;
double *table = new double[(size_t)(m * ANT_pq_codec::K)];
ANT_pq_codec::adc_table(query, dimension, m, ANT_pq_codec::K, codebook, metric, table);
adc_table_builds++;
double s = ANT_pq_codec::adc_score(codes + t*m, m, ANT_pq_codec::K, table);
delete [] table;
return s;
}

void *ANT_multivector_pq_store::token_prepare_query(const float *query)
{
if (total_tokens == 0 || codebook == 0)
	return 0;								/* degraded store: ctx==NULL -> score_prepared falls back */
double *table = new double[(size_t)(m * ANT_pq_codec::K)];
ANT_pq_codec::adc_table(query, dimension, m, ANT_pq_codec::K, codebook, metric, table);
adc_table_builds++;
return table;
}

double ANT_multivector_pq_store::token_score_prepared(long long t, const float *query, void *ctx)
{
if (ctx == 0)
	return token_score(t, query, metric);	/* no prepared table -> per-call build (build path) */
if (!token_has(t))
	return 0.0;
return ANT_pq_codec::adc_score(codes + t*m, m, ANT_pq_codec::K, (double *)ctx);
}

void ANT_multivector_pq_store::token_free_query(void *ctx)
{
delete [] (double *)ctx;					/* delete[] NULL is a no-op */
}

double ANT_multivector_pq_store::maxsim(long long docid, const float *query_vecs, long long num_query_vecs)
{
if (!has(docid) || num_query_vecs < 1) return 0.0;
double *tables = new double[(size_t)(num_query_vecs * m * ANT_pq_codec::K)];
for (long long q = 0; q < num_query_vecs; q++)
	ANT_pq_codec::adc_table(query_vecs + q*dimension, dimension, m, ANT_pq_codec::K, codebook, metric, tables + q*(m*ANT_pq_codec::K));

long long begin = offsets[docid], end = offsets[docid] + counts[docid];
double total = 0.0;
for (long long q = 0; q < num_query_vecs; q++)
	{
	const double *table = tables + q*(m*ANT_pq_codec::K);
	double best = 0.0; int seen = 0;
	for (long long t = begin; t < end; t++)
		{ double s = ANT_pq_codec::adc_score(codes + t*m, m, ANT_pq_codec::K, table); if (!seen || s > best) { best = s; seen = 1; } }
	total += best;
	}
delete [] tables;
return total;
}

static long long read_i64(const unsigned char *p) { long long v; memcpy(&v, p, 8); return v; }

ANT_multivector_pq_store *ANT_multivector_pq_store::load(const char *filename, long long expected_dimension, long long expected_documents, long metric)
{
ANT_multivector_pq_store *s = new ANT_multivector_pq_store();
s->metric = metric;
FILE *in = fopen(filename, "rb");
if (in == NULL) return s;

unsigned char hdr[52];
if (fread(hdr, 1, 52, in) != 52) { fclose(in); return s; }
if (memcmp(hdr, "ANTMVPQ1", 8) != 0) { fclose(in); return s; }
unsigned int version; memcpy(&version, hdr+8, 4);
if (version != 1) { fclose(in); return s; }
long long dim = read_i64(hdr+12), docs = read_i64(hdr+20), toks = read_i64(hdr+28), mm = read_i64(hdr+36), kk = read_i64(hdr+44);
if (dim != expected_dimension || docs != expected_documents || kk != ANT_pq_codec::K) { fclose(in); return s; }
if (mm < 1 || dim < 1 || mm > dim || dim % mm != 0 || docs < 0 || toks < 0) { fclose(in); return s; }

if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return s; }
long long actual = ftell(in);
/* toks is the only header count without an upper bound; cap it against the real file size so
   toks*mm below cannot signed-overflow (docs/dim are already pinned to expected_* above). */
if (actual < 52 || (mm > 0 && toks > (actual - 52) / mm)) { fclose(in); return s; }
long long expected_size = 52 + docs*4 + toks*mm + 256*dim*4;
if (actual != expected_size) { fclose(in); return s; }
if (fseek(in, 52, SEEK_SET) != 0) { fclose(in); return s; }

int *counts = new int[docs > 0 ? docs : 1];
long long *offsets = new long long[docs + 1];
unsigned char *codes = new unsigned char[toks*mm > 0 ? toks*mm : 1];
long long cb_floats = 256*dim;
float *codebook = new float[cb_floats > 0 ? cb_floats : 1];

long ok = 1;
if (docs > 0 && fread(counts, 4, (size_t)docs, in) != (size_t)docs) ok = 0;
if (ok && toks > 0 && fread(codes, 1, (size_t)(toks*mm), in) != (size_t)(toks*mm)) ok = 0;
if (ok && fread(codebook, sizeof(float), (size_t)cb_floats, in) != (size_t)cb_floats) ok = 0;
fclose(in);

if (ok)
	{
	offsets[0] = 0;
	for (long long d = 0; d < docs; d++)
		{ if (counts[d] < 0) { ok = 0; break; } offsets[d+1] = offsets[d] + counts[d]; }
	if (ok && offsets[docs] != toks) ok = 0;
	}

if (!ok) { delete [] counts; delete [] offsets; delete [] codes; delete [] codebook; return s; }

s->dimension = dim; s->documents = docs; s->total_tokens = toks; s->m = mm;
s->counts = counts; s->offsets = offsets; s->codes = codes; s->codebook = codebook;
return s;
}

ANT_multivector_pq_store_writer::ANT_multivector_pq_store_writer() :
	filename(0), dimension(0), m(0), metric(0), buffer(0), capacity(0), total_tokens(0),
	counts(0), counts_capacity(0), documents(0) {}

ANT_multivector_pq_store_writer::~ANT_multivector_pq_store_writer() { abandon(); }

long ANT_multivector_pq_store_writer::create(const char *path, long long dim, long long mm, long met)
{
if (dim < 1 || mm < 1 || mm > dim || dim % mm != 0) return 1;
abandon();
filename = new char[strlen(path)+1]; strcpy(filename, path);
dimension = dim; m = mm; metric = met;
capacity = 1024; buffer = new float[capacity * dimension];
counts_capacity = 256; counts = new int[counts_capacity];
total_tokens = 0; documents = 0;
return 0;
}

long ANT_multivector_pq_store_writer::append(const float *vectors, long long num_vectors)
{
if (filename == NULL) return 1;
if (num_vectors < 0) num_vectors = 0;
if (documents >= counts_capacity)
	{ long long nc = counts_capacity*2; int *n = new int[nc]; memcpy(n, counts, (size_t)(documents*sizeof(int))); delete [] counts; counts = n; counts_capacity = nc; }
if (total_tokens + num_vectors > capacity)
	{ long long nc = capacity; while (total_tokens + num_vectors > nc) nc *= 2; float *n = new float[nc*dimension]; memcpy(n, buffer, (size_t)(total_tokens*dimension*sizeof(float))); delete [] buffer; buffer = n; capacity = nc; }
if (num_vectors > 0 && vectors != NULL)
	memcpy(buffer + total_tokens*dimension, vectors, (size_t)(num_vectors*dimension*sizeof(float)));
counts[documents++] = (int)num_vectors;
total_tokens += num_vectors;
return 0;
}

long ANT_multivector_pq_store_writer::finish(void)
{
if (filename == NULL) return 1;
long long cb_floats = 256*dimension;
float *codebook = new float[cb_floats];
if (ANT_pq_codec::train(buffer, dimension, m, ANT_pq_codec::K, total_tokens, codebook) != 0) { delete [] codebook; return 1; }

unsigned char *codes = new unsigned char[total_tokens*m > 0 ? total_tokens*m : 1];
for (long long t = 0; t < total_tokens; t++)
	ANT_pq_codec::encode(buffer + t*dimension, dimension, m, ANT_pq_codec::K, codebook, codes + t*m);

char *tmp = new char[strlen(filename)+5]; strcpy(tmp, filename); strcat(tmp, ".tmp");
FILE *out = fopen(tmp, "wb");
long ok = out != NULL;
if (ok)
	{
	unsigned int version = 1; long long k = 256;
	ok = fwrite("ANTMVPQ1", 1, 8, out) == 8
		&& fwrite(&version, 4, 1, out) == 1
		&& fwrite(&dimension, 8, 1, out) == 1
		&& fwrite(&documents, 8, 1, out) == 1
		&& fwrite(&total_tokens, 8, 1, out) == 1
		&& fwrite(&m, 8, 1, out) == 1
		&& fwrite(&k, 8, 1, out) == 1
		&& (documents == 0 || fwrite(counts, 4, (size_t)documents, out) == (size_t)documents)
		&& (total_tokens == 0 || fwrite(codes, 1, (size_t)(total_tokens*m), out) == (size_t)(total_tokens*m))
		&& fwrite(codebook, sizeof(float), (size_t)cb_floats, out) == (size_t)cb_floats;
	if (fclose(out) != 0) ok = 0;
	}
if (ok && rename(tmp, filename) != 0) ok = 0;
if (!ok) remove(tmp);
delete [] tmp; delete [] codes; delete [] codebook;
if (ok) abandon();
return ok ? 0 : 1;
}

void ANT_multivector_pq_store_writer::abandon(void)
{
delete [] filename; delete [] buffer; delete [] counts;
filename = 0; buffer = 0; counts = 0; capacity = 0; counts_capacity = 0; total_tokens = 0; documents = 0;
}

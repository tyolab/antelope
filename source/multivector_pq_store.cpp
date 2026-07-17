/*
	MULTIVECTOR_PQ_STORE.CPP -- see header. Ragged PQ token pool, reuses ANT_pq_codec.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multivector_pq_store.h"
#include "pq_codec.h"

enum { MVPQ_CODE_STACK_CAP = 4096 };		// bytes: inline unpacked/encoded per-row code buffer (m-cap); heap above this

ANT_multivector_pq_store::ANT_multivector_pq_store() :
	dimension(0), documents(0), total_tokens(0), m(0), k(256), row_bytes(0), metric(0),
	counts(0), offsets(0), codebook(0), codes(0), rotation(0), owns_codebook(1), owns_rotation(1), adc_table_builds(0) {}

/* unpack token t's packed row into an m-byte scratch (caller supplies buf of size >= m) */
static inline void mvpq_unpack_row(const unsigned char *codes, long long t, long long row_bytes, long long m, long long bits, unsigned char *buf)
{ ANT_pq_codec::unpack_codes(codes + t*row_bytes, m, bits, buf); }

ANT_multivector_pq_store::~ANT_multivector_pq_store()
{
delete [] counts; delete [] offsets; delete [] codes;
if (owns_codebook) delete [] codebook;
if (owns_rotation) delete [] rotation;
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
long long bits = ANT_pq_codec::bits_for_k(k);
unsigned char cb[MVPQ_CODE_STACK_CAP];
unsigned char *codebuf = (m <= MVPQ_CODE_STACK_CAP) ? cb : new unsigned char[m];
mvpq_unpack_row(codes, t, row_bytes, m, bits, codebuf);
if (rotation != NULL)
	{
	float *tmp = new float[dimension];
	ANT_pq_codec::reconstruct(codebuf, dimension, m, k, codebook, tmp);
	ANT_pq_codec::apply_rotation_transpose(tmp, dimension, rotation, out);
	delete [] tmp;
	}
else
	ANT_pq_codec::reconstruct(codebuf, dimension, m, k, codebook, out);
if (m > MVPQ_CODE_STACK_CAP) delete [] codebuf;
}

/* rotate `query` into R-space if OPQ is on; returns a buffer the caller must delete[],
   or NULL when no rotation (caller then uses the original query). */
static float *rotate_query_or_null(const float *query, long long dimension, const float *rotation)
{
if (rotation == NULL) return NULL;
float *rq = new float[dimension];
ANT_pq_codec::apply_rotation(query, dimension, rotation, rq);
return rq;
}

double ANT_multivector_pq_store::token_score(long long t, const float *query, long)
{
if (!token_has(t)) return 0.0;
double *table = new double[(size_t)(m * k)];
float *rq = rotate_query_or_null(query, dimension, rotation);
ANT_pq_codec::adc_table(rq ? rq : query, dimension, m, k, codebook, metric, table);
delete [] rq;
adc_table_builds++;
long long bits = ANT_pq_codec::bits_for_k(k);
unsigned char rb[MVPQ_CODE_STACK_CAP]; unsigned char *rbuf = (m <= MVPQ_CODE_STACK_CAP) ? rb : new unsigned char[m];
mvpq_unpack_row(codes, t, row_bytes, m, bits, rbuf);
double s = ANT_pq_codec::adc_score(rbuf, m, k, table);
if (m > MVPQ_CODE_STACK_CAP) delete [] rbuf;
delete [] table;
return s;
}

void *ANT_multivector_pq_store::token_prepare_query(const float *query)
{
if (total_tokens == 0 || codebook == 0)
	return 0;								/* degraded store: ctx==NULL -> score_prepared falls back */
double *table = new double[(size_t)(m * k)];
float *rq = rotate_query_or_null(query, dimension, rotation);
ANT_pq_codec::adc_table(rq ? rq : query, dimension, m, k, codebook, metric, table);
delete [] rq;
adc_table_builds++;
return table;
}

double ANT_multivector_pq_store::token_score_prepared(long long t, const float *query, void *ctx)
{
if (ctx == 0)
	return token_score(t, query, metric);	/* no prepared table -> per-call build (build path) */
if (!token_has(t))
	return 0.0;
long long bits = ANT_pq_codec::bits_for_k(k);
unsigned char rb[MVPQ_CODE_STACK_CAP]; unsigned char *rbuf = (m <= MVPQ_CODE_STACK_CAP) ? rb : new unsigned char[m];
mvpq_unpack_row(codes, t, row_bytes, m, bits, rbuf);
double s = ANT_pq_codec::adc_score(rbuf, m, k, (double *)ctx);
if (m > MVPQ_CODE_STACK_CAP) delete [] rbuf;
return s;
}

void ANT_multivector_pq_store::token_free_query(void *ctx)
{
delete [] (double *)ctx;					/* delete[] NULL is a no-op */
}

double ANT_multivector_pq_store::maxsim(long long docid, const float *query_vecs, long long num_query_vecs)
{
if (!has(docid) || num_query_vecs < 1) return 0.0;
double *tables = new double[(size_t)(num_query_vecs * m * k)];
for (long long q = 0; q < num_query_vecs; q++)
	{
	float *rq = rotate_query_or_null(query_vecs + q*dimension, dimension, rotation);
	ANT_pq_codec::adc_table(rq ? rq : (query_vecs + q*dimension), dimension, m, k, codebook, metric, tables + q*(m*k));
	delete [] rq;
	}

long long bits = ANT_pq_codec::bits_for_k(k);
unsigned char rb[MVPQ_CODE_STACK_CAP]; unsigned char *rbuf = (m <= MVPQ_CODE_STACK_CAP) ? rb : new unsigned char[m];
long long begin = offsets[docid], end = offsets[docid] + counts[docid];
double total = 0.0;
for (long long q = 0; q < num_query_vecs; q++)
	{
	const double *table = tables + q*(m*k);
	double best = 0.0; int seen = 0;
	for (long long t = begin; t < end; t++)
		{ mvpq_unpack_row(codes, t, row_bytes, m, bits, rbuf); double s = ANT_pq_codec::adc_score(rbuf, m, k, table); if (!seen || s > best) { best = s; seen = 1; } }
	total += best;
	}
if (m > MVPQ_CODE_STACK_CAP) delete [] rbuf;
delete [] tables;
return total;
}

static long long read_i64(const unsigned char *p) { long long v; memcpy(&v, p, 8); return v; }

ANT_multivector_pq_store *ANT_multivector_pq_store::load(const char *filename, long long expected_dimension, long long expected_documents, long metric, const float *borrowed_codebook, const float *borrowed_rotation)
{
ANT_multivector_pq_store *s = new ANT_multivector_pq_store();
s->metric = metric;
FILE *in = fopen(filename, "rb");
if (in == NULL) return s;

unsigned char hdr[52];
if (fread(hdr, 1, 52, in) != 52) { fclose(in); return s; }
if (memcmp(hdr, "ANTMVPQ1", 8) != 0) { fclose(in); return s; }
unsigned int version; memcpy(&version, hdr+8, 4);
if (version != 1 && version != 2 && version != 3) { fclose(in); return s; }
long long dim = read_i64(hdr+12), docs = read_i64(hdr+20), toks = read_i64(hdr+28), mm = read_i64(hdr+36), kk = read_i64(hdr+44);
long long bits;
if (version == 3)
	bits = ANT_pq_codec::bits_for_k(kk);						/* any power of two in [2,256] */
else
	bits = (kk == ANT_pq_codec::K) ? 8 : -1;					/* v1/v2: k must be 256 */
if (dim != expected_dimension || docs != expected_documents || bits < 0) { fclose(in); return s; }
if (mm < 1 || dim < 1 || mm > dim || dim % mm != 0 || docs < 0 || toks < 0) { fclose(in); return s; }

long long opq = 0, header_size = 52;
if (version == 2 || version == 3)
	{
	unsigned char ohdr[8];
	if (fread(ohdr, 1, 8, in) != 8) { fclose(in); return s; }
	opq = read_i64(ohdr);
	if (opq != 0 && opq != 1) { fclose(in); return s; }
	header_size = 60;
	}

long long row_bytes = (mm*bits + 7) / 8;
if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return s; }
long long actual = ftell(in);
/* toks is the only header count without an upper bound; cap it against the real file size so
   toks*row_bytes below cannot signed-overflow (docs/dim are already pinned to expected_* above). */
if (actual < header_size || (row_bytes > 0 && toks > (actual - header_size) / row_bytes)) { fclose(in); return s; }
long long rot_floats = opq ? dim*dim : 0;					/* dim pinned to expected: dim*dim*4 bounded, no overflow */
long long cb_floats = kk*dim;								/* k*dim (was 256*dim) */
long long expected_size = header_size + docs*4 + toks*row_bytes + cb_floats*4 + rot_floats*4;
if (actual != expected_size) { fclose(in); return s; }
if (fseek(in, header_size, SEEK_SET) != 0) { fclose(in); return s; }

/*
	Approach A (single-resident): if a borrowed codebook is supplied AND the
	file's opq flag is consistent with the supplied rotation (rotation present
	iff opq==1), point at the borrowed buffers and SKIP reading the embedded
	codebook/rotation into RAM (fseek past them). Otherwise own an embedded copy
	exactly as before. Header dims/m/k were already validated against expected_*;
	global mode guarantees the resident codebook is the one every .mvpq embeds,
	so no byte-compare is needed.
*/
int borrow = (borrowed_codebook != NULL) && ((opq == 1) == (borrowed_rotation != NULL));

int *counts = new int[docs > 0 ? docs : 1];
long long *offsets = new long long[docs + 1];
unsigned char *codes = new unsigned char[toks*row_bytes > 0 ? toks*row_bytes : 1];
float *codebook = borrow ? NULL : new float[cb_floats > 0 ? cb_floats : 1];
float *rotation = (borrow || rot_floats == 0) ? NULL : new float[rot_floats];

long ok = 1;
if (docs > 0 && fread(counts, 4, (size_t)docs, in) != (size_t)docs) ok = 0;
if (ok && toks > 0 && fread(codes, 1, (size_t)(toks*row_bytes), in) != (size_t)(toks*row_bytes)) ok = 0;
if (ok && cb_floats > 0)					/* codebook block: read OR skip */
	{
	if (borrow)
		{ if (fseek(in, (long)(cb_floats * (long long)sizeof(float)), SEEK_CUR) != 0) ok = 0; }
	else if (fread(codebook, sizeof(float), (size_t)cb_floats, in) != (size_t)cb_floats) ok = 0;
	}
if (ok && rot_floats > 0)					/* rotation block: read OR skip */
	{
	if (borrow)
		{ if (fseek(in, (long)(rot_floats * (long long)sizeof(float)), SEEK_CUR) != 0) ok = 0; }
	else if (fread(rotation, sizeof(float), (size_t)rot_floats, in) != (size_t)rot_floats) ok = 0;
	}
fclose(in);

if (ok)
	{
	offsets[0] = 0;
	for (long long d = 0; d < docs; d++)
		{ if (counts[d] < 0) { ok = 0; break; } offsets[d+1] = offsets[d] + counts[d]; }
	if (ok && offsets[docs] != toks) ok = 0;
	}

if (!ok) { delete [] counts; delete [] offsets; delete [] codes; delete [] codebook; delete [] rotation; return s; }

s->dimension = dim; s->documents = docs; s->total_tokens = toks; s->m = mm;
s->k = kk; s->row_bytes = row_bytes;
s->counts = counts; s->offsets = offsets; s->codes = codes;
if (borrow)
	{
	s->codebook = (float *)borrowed_codebook; s->owns_codebook = 0;
	s->rotation = (opq == 1) ? (float *)borrowed_rotation : NULL; s->owns_rotation = 0;
	}
else
	{
	s->codebook = codebook; s->owns_codebook = 1;
	s->rotation = rotation; s->owns_rotation = 1;	/* rotation NULL when opq==0 */
	}
return s;
}

ANT_multivector_pq_store_writer::ANT_multivector_pq_store_writer() :
	filename(0), dimension(0), m(0), k(256), metric(0), opq(0), ext_codebook(0), ext_rotation(0),
	buffer(0), capacity(0), total_tokens(0),
	counts(0), counts_capacity(0), documents(0) {}

ANT_multivector_pq_store_writer::~ANT_multivector_pq_store_writer() { abandon(); }

void ANT_multivector_pq_store_writer::set_external_codebook(const float *codebook, const float *rotation)
{
ext_codebook = codebook;
ext_rotation = rotation;
}

long ANT_multivector_pq_store_writer::create(const char *path, long long dim, long long mm, long long kk, long met, long op)
{
if (dim < 1 || mm < 1 || mm > dim || dim % mm != 0) return 1;
if (ANT_pq_codec::bits_for_k(kk) < 0) return 1;
abandon();
ext_codebook = NULL;
ext_rotation = NULL;
filename = new char[strlen(path)+1]; strcpy(filename, path);
dimension = dim; m = mm; k = kk; metric = met; opq = op ? 1 : 0;
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
long long bits = ANT_pq_codec::bits_for_k(k);
long long row_bytes = (m*bits + 7) / 8;
long long cb_floats = k*dimension;

/* --- rotation: external (borrowed) OR trained (owned) OR none --- */
float *owned_rotation = NULL;
const float *rotation = NULL;
if (ext_codebook != NULL)
	rotation = ext_rotation;						/* borrowed (may be NULL for non-OPQ global) */
else if (opq && total_tokens > 0)
	{
	owned_rotation = new float[dimension * dimension];
	if (ANT_pq_codec::train_rotation(buffer, dimension, m, total_tokens, owned_rotation) != 0)
		{ delete [] owned_rotation; owned_rotation = NULL; }
	else
		rotation = owned_rotation;
	}
if (rotation != NULL)								/* rotate the whole pool in place */
	{
	float *tmp = new float[dimension];
	for (long long t = 0; t < total_tokens; t++)
		{
		ANT_pq_codec::apply_rotation(buffer + t*dimension, dimension, rotation, tmp);
		memcpy(buffer + t*dimension, tmp, (size_t)(dimension*sizeof(float)));
		}
	delete [] tmp;
	}

/* --- codebook: external (borrowed) OR trained (owned) --- */
float *owned_codebook = NULL;
const float *codebook = NULL;
if (ext_codebook != NULL)
	codebook = ext_codebook;						/* borrowed; do NOT free */
else
	{
	owned_codebook = new float[cb_floats];
	if (ANT_pq_codec::train(buffer, dimension, m, k, total_tokens, owned_codebook) != 0)
		{ delete [] owned_codebook; delete [] owned_rotation; return 1; }
	codebook = owned_codebook;
	}

unsigned char *codes = new unsigned char[total_tokens*row_bytes > 0 ? total_tokens*row_bytes : 1];
	{
	unsigned char tmpcodes[MVPQ_CODE_STACK_CAP]; unsigned char *tbuf = (m <= MVPQ_CODE_STACK_CAP) ? tmpcodes : new unsigned char[m];
	for (long long t = 0; t < total_tokens; t++)
		{
		ANT_pq_codec::encode(buffer + t*dimension, dimension, m, k, codebook, tbuf);
		ANT_pq_codec::pack_codes(tbuf, m, bits, codes + t*row_bytes);
		}
	if (m > MVPQ_CODE_STACK_CAP) delete [] tbuf;
	}

char *tmp = new char[strlen(filename)+5]; strcpy(tmp, filename); strcat(tmp, ".tmp");
FILE *out = fopen(tmp, "wb");
long ok = out != NULL;
if (ok)
	{
	long long opq_flag = (rotation != NULL) ? 1 : 0;			/* derive from the R actually used */
	unsigned int version = (k == 256) ? (opq_flag ? 2u : 1u) : 3u;
	ok = fwrite("ANTMVPQ1", 1, 8, out) == 8
		&& fwrite(&version, 4, 1, out) == 1
		&& fwrite(&dimension, 8, 1, out) == 1
		&& fwrite(&documents, 8, 1, out) == 1
		&& fwrite(&total_tokens, 8, 1, out) == 1
		&& fwrite(&m, 8, 1, out) == 1
		&& fwrite(&k, 8, 1, out) == 1
		&& (version == 1u || fwrite(&opq_flag, 8, 1, out) == 1)	/* v2/v3 always carry the opq field (v3 even when 0) */
		&& (documents == 0 || fwrite(counts, 4, (size_t)documents, out) == (size_t)documents)
		&& (total_tokens == 0 || fwrite(codes, 1, (size_t)(total_tokens*row_bytes), out) == (size_t)(total_tokens*row_bytes))
		&& fwrite(codebook, sizeof(float), (size_t)cb_floats, out) == (size_t)cb_floats
		&& (opq_flag == 0 || fwrite(rotation, sizeof(float), (size_t)(dimension*dimension), out) == (size_t)(dimension*dimension));
	if (fclose(out) != 0) ok = 0;
	}
if (ok && rename(tmp, filename) != 0) ok = 0;
if (!ok) remove(tmp);
delete [] tmp; delete [] codes; delete [] owned_codebook; delete [] owned_rotation;	/* NEVER free ext_* */
if (ok) abandon();
return ok ? 0 : 1;
}

void ANT_multivector_pq_store_writer::abandon(void)
{
delete [] filename; delete [] buffer; delete [] counts;
filename = 0; buffer = 0; counts = 0; capacity = 0; counts_capacity = 0; total_tokens = 0; documents = 0;
}

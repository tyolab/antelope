/*
	VECTOR_STORE.CPP
	----------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vector_store.h"
#include "index_tombstones.h"
#include "vector_quantize.h"

static const unsigned long long ANT_VECTOR_STORE_MAGIC = 0x3130434556544E41ULL;	// "ANTVEC01" little-endian
static const unsigned long long ANT_QVECTOR_STORE_MAGIC = 0x3130435651544E41ULL;	// "ANTQVC01" little-endian

/*
	ANT_VECTOR_CANDIDATE_INSERT()
	-----------------------------
*/
void ANT_vector_candidate_insert(ANT_vector_candidate *best, long long *best_count, long long top_k, double score, long long generation, long long docid)
{
long long which, weakest;

if (*best_count < top_k)
	{
	best[*best_count].score = score;
	best[*best_count].generation = generation;
	best[*best_count].docid = docid;
	(*best_count)++;
	return;
	}
weakest = 0;
for (which = 1; which < *best_count; which++)
	if (best[which].score < best[weakest].score)
		weakest = which;
if (score > best[weakest].score)
	{
	best[weakest].score = score;
	best[weakest].generation = generation;
	best[weakest].docid = docid;
	}
}

/*
	ANT_VECTOR_STORE::ANT_VECTOR_STORE()
	------------------------------------
*/
ANT_vector_store::ANT_vector_store()
{
dimension = 0;
documents = 0;
presence = NULL;
vectors = NULL;
codes = NULL;
qmin = NULL;
qmax = NULL;
quantized = 0;
}

/*
	ANT_VECTOR_STORE::~ANT_VECTOR_STORE()
	-------------------------------------
*/
ANT_vector_store::~ANT_vector_store()
{
delete [] presence;
delete [] vectors;
delete [] codes;
delete [] qmin;
delete [] qmax;
}

/*
	ANT_VECTOR_STORE::LOAD()
	------------------------
	Any validation failure returns a degraded empty store: the segment keeps
	working lexically, it just has no vectors.
*/
ANT_vector_store *ANT_vector_store::load(const char *filename, long long expected_dimension, long long expected_documents)
{
FILE *fp;
unsigned long long magic;
long long stored_dimension, stored_documents, presence_bytes, file_size, expected_size;
ANT_vector_store *result = new ANT_vector_store();

if ((fp = fopen(filename, "rb")) == NULL)
	return result;

if (fread(&magic, sizeof(magic), 1, fp) != 1 || (magic != ANT_VECTOR_STORE_MAGIC && magic != ANT_QVECTOR_STORE_MAGIC))
	{
	fclose(fp);
	return result;
	}

if (fread(&stored_dimension, sizeof(stored_dimension), 1, fp) != 1
	|| fread(&stored_documents, sizeof(stored_documents), 1, fp) != 1
	|| stored_dimension != expected_dimension
	|| stored_documents != expected_documents
	|| stored_dimension < 1 || stored_dimension > 65536
	|| stored_documents < 0 || stored_documents > (1LL << 40))
	{
	fclose(fp);
	return result;
	}

presence_bytes = (stored_documents + 7) / 8;

if (magic == ANT_QVECTOR_STORE_MAGIC)
	{
	/*
		Same validate-before-allocate discipline as the float path below: the
		header's counts drive the allocations, so the exact file size must be
		checked before trusting them.
	*/
	expected_size = 24 + presence_bytes + stored_dimension * (long long)sizeof(float) * 2
		+ stored_documents * stored_dimension * (long long)sizeof(signed char);
	if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) != expected_size || fseek(fp, 24, SEEK_SET) != 0)
		{
		fclose(fp);
		return result;
		}

	unsigned char *presence_buffer = new unsigned char[presence_bytes > 0 ? presence_bytes : 1];
	float *mins_buffer = new float[stored_dimension];
	float *maxs_buffer = new float[stored_dimension];
	signed char *codes_buffer = new signed char[stored_documents * stored_dimension > 0 ? stored_documents * stored_dimension : 1];
	if (fread(presence_buffer, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
		|| fread(mins_buffer, sizeof(float), (size_t)stored_dimension, fp) != (size_t)stored_dimension
		|| fread(maxs_buffer, sizeof(float), (size_t)stored_dimension, fp) != (size_t)stored_dimension
		|| fread(codes_buffer, sizeof(signed char), (size_t)(stored_documents * stored_dimension), fp) != (size_t)(stored_documents * stored_dimension))
		{
		delete [] presence_buffer;
		delete [] mins_buffer;
		delete [] maxs_buffer;
		delete [] codes_buffer;
		fclose(fp);
		return result;
		}
	fclose(fp);

	result->dimension = stored_dimension;
	result->documents = stored_documents;
	result->presence = presence_buffer;
	result->qmin = mins_buffer;
	result->qmax = maxs_buffer;
	result->codes = codes_buffer;
	result->quantized = 1;
	return result;
	}

/*
	The header's counts drive the allocations below, so a corrupt file lying
	about them could trigger an absurd new[] and abort the process.  Verify
	the actual file size matches exactly what the writer would have produced
	before trusting the header.
*/
expected_size = 24 + presence_bytes + stored_documents * stored_dimension * (long long)sizeof(float);
if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) != expected_size || fseek(fp, 24, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

unsigned char *presence_buffer = new unsigned char[presence_bytes > 0 ? presence_bytes : 1];
float *vector_buffer = new float[stored_documents * stored_dimension > 0 ? stored_documents * stored_dimension : 1];
if (fread(presence_buffer, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
	|| fread(vector_buffer, sizeof(float), (size_t)(stored_documents * stored_dimension), fp) != (size_t)(stored_documents * stored_dimension))
	{
	delete [] presence_buffer;
	delete [] vector_buffer;
	fclose(fp);
	return result;
	}
fclose(fp);

result->dimension = stored_dimension;
result->documents = stored_documents;
result->presence = presence_buffer;
result->vectors = vector_buffer;
return result;
}

/*
	ANT_VECTOR_STORE::RECONSTRUCT()
	--------------------------------
*/
void ANT_vector_store::reconstruct(long long docid, float *out)
{
if (quantized)
	ANT_vector_quantize::reconstruct(codes + docid * dimension, dimension, qmin, qmax, out);
else
	memcpy(out, vectors + docid * dimension, (size_t)(dimension * sizeof(float)));
}

/*
	ANT_VECTOR_STORE::SCORE()
	--------------------------
	Quantized stores reconstruct into a scratch buffer before scoring; a
	stack buffer is used for the common case to avoid a heap round-trip.
*/
double ANT_vector_store::score(long long docid, const float *query, long metric)
{
if (!quantized)
	return kernel(vectors + docid * dimension, query, dimension, metric);

if (dimension <= 512)
	{
	float tmp[512];
	reconstruct(docid, tmp);
	return kernel(tmp, query, dimension, metric);
	}

float *tmp = new float[dimension];
reconstruct(docid, tmp);
double result = kernel(tmp, query, dimension, metric);
delete [] tmp;
return result;
}

/*
	ANT_VECTOR_STORE::KERNEL()
	--------------------------
*/
double ANT_vector_store::kernel(const float *a, const float *b, long long dimension, long metric)
{
long long which;
double sum = 0.0;

if (metric == METRIC_L2)
	{
	for (which = 0; which < dimension; which++)
		{
		double difference = (double)a[which] - (double)b[which];
		sum += difference * difference;
		}
	return -sum;
	}
for (which = 0; which < dimension; which++)
	sum += (double)a[which] * (double)b[which];
return sum;
}

/*
	ANT_VECTOR_STORE::SCAN()
	------------------------
	Exhaustive scan of present, non-tombstoned documents; tombstones filtered
	inline so no over-fetch is needed on the vector side.
*/
void ANT_vector_store::scan(const float *query, long metric, ANT_index_tombstones *tombstones, long long generation, ANT_vector_candidate *best, long long *best_count, long long top_k)
{
long long docid;

if (presence == NULL || (vectors == NULL && codes == NULL))		// degraded only if BOTH backends absent
	return;
for (docid = 0; docid < documents; docid++)
	{
	if (!has(docid))
		continue;
	if (tombstones != NULL && tombstones->is_deleted(docid))
		continue;
	ANT_vector_candidate_insert(best, best_count, top_k, score(docid, query, metric), generation, docid);	// score() handles both backends
	}
}

/*
	ANT_VECTOR_STORE::NORMALIZE()
	-----------------------------
*/
long ANT_vector_store::normalize(float *vector, long long dimension)
{
long long which;
double sum = 0.0;

for (which = 0; which < dimension; which++)
	sum += (double)vector[which] * (double)vector[which];
if (sum <= 0.0)
	return 1;
double scale = 1.0 / sqrt(sum);
for (which = 0; which < dimension; which++)
	vector[which] = (float)(vector[which] * scale);
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::ANT_VECTOR_STORE_WRITER()
	--------------------------------------------------
*/
ANT_vector_store_writer::ANT_vector_store_writer()
{
filename[0] = '\0';
dimension = 0;
documents = 0;
capacity = 0;
presence = NULL;
vectors = NULL;
quant_mode = QUANT_OFF;
}

/*
	ANT_VECTOR_STORE_WRITER::~ANT_VECTOR_STORE_WRITER()
	---------------------------------------------------
*/
ANT_vector_store_writer::~ANT_vector_store_writer()
{
delete [] presence;
delete [] vectors;
}

/*
	ANT_VECTOR_STORE_WRITER::CREATE()
	---------------------------------
	Resets any prior state, so a writer may be reused across create() calls.
*/
long ANT_vector_store_writer::create(const char *name, long long width)
{
if (width < 1 || width > 65536)
	return 1;
if (snprintf(filename, sizeof(filename), "%s", name) >= (int)sizeof(filename))
	return 1;
delete [] presence;
delete [] vectors;
dimension = width;
documents = 0;
capacity = dimension > 4096 ? 64 : 1024;		// keep the up-front buffer modest for very wide vectors
presence = new unsigned char[(capacity + 7) / 8];
memset(presence, 0, (size_t)((capacity + 7) / 8));
vectors = new float[capacity * dimension];
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::GROW()
	-------------------------------
*/
long ANT_vector_store_writer::grow(void)
{
long long new_capacity = capacity * 2;
unsigned char *new_presence = new unsigned char[(new_capacity + 7) / 8];
float *new_vectors = new float[new_capacity * dimension];

memset(new_presence, 0, (size_t)((new_capacity + 7) / 8));
memcpy(new_presence, presence, (size_t)((capacity + 7) / 8));
memcpy(new_vectors, vectors, (size_t)(documents * dimension * sizeof(float)));
delete [] presence;
delete [] vectors;
presence = new_presence;
vectors = new_vectors;
capacity = new_capacity;
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::APPEND()
	---------------------------------
*/
long ANT_vector_store_writer::append(const float *vector_or_null)
{
if (vectors == NULL)
	return 1;
if (documents >= capacity)
	grow();
if (vector_or_null == NULL)
	memset(vectors + documents * dimension, 0, (size_t)(dimension * sizeof(float)));
else
	{
	memcpy(vectors + documents * dimension, vector_or_null, (size_t)(dimension * sizeof(float)));
	presence[documents / 8] |= (unsigned char)(1 << (documents % 8));
	}
documents++;
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::WRITE_QVEC()
	--------------------------------------
	Quantizes the buffered rows (ranged over all rows, absent-zeroed, exactly
	the way the float path stores them -- see class comment) and writes the
	.qvec sidecar atomically (.tmp then rename), mirroring finish() below.
*/
long ANT_vector_store_writer::write_qvec(const char *path)
{
char temp_name[4200];
FILE *fp;
long long presence_bytes = (documents + 7) / 8;
long long which;

if (vectors == NULL)
	return 1;
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", path) >= (int)sizeof(temp_name))
	return 1;

float *mins = new float[dimension];
float *maxs = new float[dimension];
ANT_vector_quantize::compute_ranges(vectors, dimension, documents, mins, maxs);

signed char *codes = new signed char[documents * dimension > 0 ? documents * dimension : 1];
for (which = 0; which < documents; which++)
	ANT_vector_quantize::quantize(vectors + which * dimension, dimension, mins, maxs, codes + which * dimension);

long result = 0;
if ((fp = fopen(temp_name, "wb")) == NULL)
	result = 1;
else
	{
	if (fwrite(&ANT_QVECTOR_STORE_MAGIC, sizeof(ANT_QVECTOR_STORE_MAGIC), 1, fp) != 1
		|| fwrite(&dimension, sizeof(dimension), 1, fp) != 1
		|| fwrite(&documents, sizeof(documents), 1, fp) != 1
		|| fwrite(presence, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
		|| fwrite(mins, sizeof(float), (size_t)dimension, fp) != (size_t)dimension
		|| fwrite(maxs, sizeof(float), (size_t)dimension, fp) != (size_t)dimension
		|| fwrite(codes, sizeof(signed char), (size_t)(documents * dimension), fp) != (size_t)(documents * dimension))
		{
		fclose(fp);
		remove(temp_name);
		result = 1;
		}
	else
		{
		fclose(fp);
		if (rename(temp_name, path) != 0)
			{
			remove(temp_name);
			result = 1;
			}
		}
	}

delete [] mins;
delete [] maxs;
delete [] codes;
return result;
}

/*
	ANT_VECTOR_STORE_WRITER::FINISH()
	---------------------------------
	Write-temp then rename, per the crash-safety convention.  QUANT_REPLACE
	writes the int8 .qvec sidecar to the same path instead of the float .vec
	layout; QUANT_BOTH is not implemented yet (a later task) and falls back
	to the float-only behaviour of QUANT_OFF.
*/
long ANT_vector_store_writer::finish(void)
{
char temp_name[4200];
FILE *fp;
long long presence_bytes = (documents + 7) / 8;

if (vectors == NULL)
	return 1;

if (quant_mode == QUANT_REPLACE)
	return write_qvec(filename);

if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
if (fwrite(&ANT_VECTOR_STORE_MAGIC, sizeof(ANT_VECTOR_STORE_MAGIC), 1, fp) != 1
	|| fwrite(&dimension, sizeof(dimension), 1, fp) != 1
	|| fwrite(&documents, sizeof(documents), 1, fp) != 1
	|| fwrite(presence, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
	|| fwrite(vectors, sizeof(float), (size_t)(documents * dimension), fp) != (size_t)(documents * dimension))
	{
	fclose(fp);
	remove(temp_name);
	return 1;
	}
fclose(fp);
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}

/*
	ANT_VECTOR_STORE_WRITER::ABANDON()
	----------------------------------
*/
void ANT_vector_store_writer::abandon(void)
{
delete [] presence;
delete [] vectors;
presence = NULL;
vectors = NULL;
documents = 0;
}

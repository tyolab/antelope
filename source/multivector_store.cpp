/*
	MULTIVECTOR_STORE.CPP
	----------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "multivector_store.h"
#include "vector_quantize.h"

static const unsigned long long ANT_MULTIVECTOR_STORE_MAGIC = 0x314345564D544E41ULL;	// "ANTMVEC1" little-endian

/*
	ANT_MULTIVECTOR_STORE::ANT_MULTIVECTOR_STORE()
	-----------------------------------------------
*/
ANT_multivector_store::ANT_multivector_store()
{
dimension = 0;
documents = 0;
total_vectors = 0;
quantized = 0;
counts = NULL;
offsets = NULL;
pool_f = NULL;
pool_q = NULL;
qmin = NULL;
qmax = NULL;
}

/*
	ANT_MULTIVECTOR_STORE::~ANT_MULTIVECTOR_STORE()
	-------------------------------------------------
*/
ANT_multivector_store::~ANT_multivector_store()
{
delete [] counts;
delete [] offsets;
delete [] pool_f;
delete [] pool_q;
delete [] qmin;
delete [] qmax;
}

/*
	ANT_MULTIVECTOR_STORE::LOAD()
	-------------------------------
	Any validation failure returns a degraded empty store: the segment keeps
	working, it just has no multi-vectors.  Validation happens strictly before
	any size-driven allocation so a lying header cannot trigger an absurd
	new[] / abort.
*/
ANT_multivector_store *ANT_multivector_store::load(const char *filename, long long expected_dimension, long long expected_documents)
{
FILE *fp;
unsigned long long magic;
long long stored_dimension, stored_documents, stored_total_vectors;
int stored_quant_flag;
long long file_size, expected_size;
ANT_multivector_store *result = new ANT_multivector_store();

if ((fp = fopen(filename, "rb")) == NULL)
	return result;

if (fread(&magic, sizeof(magic), 1, fp) != 1
	|| fread(&stored_dimension, sizeof(stored_dimension), 1, fp) != 1
	|| fread(&stored_documents, sizeof(stored_documents), 1, fp) != 1
	|| fread(&stored_total_vectors, sizeof(stored_total_vectors), 1, fp) != 1
	|| fread(&stored_quant_flag, sizeof(stored_quant_flag), 1, fp) != 1)
	{
	fclose(fp);
	return result;
	}

if (magic != ANT_MULTIVECTOR_STORE_MAGIC
	|| stored_dimension != expected_dimension
	|| stored_documents != expected_documents
	|| stored_documents < 0 || stored_documents > (1LL << 40)
	|| stored_dimension < 1 || stored_dimension > 65536
	|| stored_total_vectors < 0 || stored_total_vectors > (1LL << 42)
	|| (stored_quant_flag != 0 && stored_quant_flag != 1))
	{
	fclose(fp);
	return result;
	}

/*
	The header's counts drive the allocations below, so a corrupt file lying
	about them could trigger an absurd new[] and abort the process.  Verify
	the actual file size matches exactly what the writer would have produced
	before trusting the header.
*/
expected_size = 36
	+ stored_documents * (long long)sizeof(int)
	+ (stored_documents + 1) * (long long)sizeof(long long)
	+ stored_total_vectors * stored_dimension * (long long)(stored_quant_flag ? sizeof(signed char) : sizeof(float))
	+ (stored_quant_flag ? stored_dimension * (long long)sizeof(float) * 2 : 0);

if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) != expected_size || fseek(fp, 36, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

int *counts_buffer = new int[stored_documents > 0 ? stored_documents : 1];
long long *offsets_buffer = new long long[stored_documents + 1];
float *pool_f_buffer = NULL;
signed char *pool_q_buffer = NULL;
float *qmin_buffer = NULL;
float *qmax_buffer = NULL;
long long pool_elements = stored_total_vectors * stored_dimension;

if (stored_quant_flag)
	pool_q_buffer = new signed char[pool_elements > 0 ? pool_elements : 1];
else
	pool_f_buffer = new float[pool_elements > 0 ? pool_elements : 1];

if (fread(counts_buffer, sizeof(int), (size_t)stored_documents, fp) != (size_t)stored_documents
	|| fread(offsets_buffer, sizeof(long long), (size_t)(stored_documents + 1), fp) != (size_t)(stored_documents + 1)
	|| (stored_quant_flag
		? fread(pool_q_buffer, sizeof(signed char), (size_t)pool_elements, fp) != (size_t)pool_elements
		: fread(pool_f_buffer, sizeof(float), (size_t)pool_elements, fp) != (size_t)pool_elements))
	{
	delete [] counts_buffer;
	delete [] offsets_buffer;
	delete [] pool_f_buffer;
	delete [] pool_q_buffer;
	fclose(fp);
	return result;
	}

if (stored_quant_flag)
	{
	qmin_buffer = new float[stored_dimension];
	qmax_buffer = new float[stored_dimension];
	if (fread(qmin_buffer, sizeof(float), (size_t)stored_dimension, fp) != (size_t)stored_dimension
		|| fread(qmax_buffer, sizeof(float), (size_t)stored_dimension, fp) != (size_t)stored_dimension)
		{
		delete [] counts_buffer;
		delete [] offsets_buffer;
		delete [] pool_f_buffer;
		delete [] pool_q_buffer;
		delete [] qmin_buffer;
		delete [] qmax_buffer;
		fclose(fp);
		return result;
		}
	}

/*
	Cross-check the offsets/counts relationship the writer guarantees, so a
	tampered-but-size-consistent file cannot cause an out-of-bounds read in
	maxsim() later.
*/
long ok = 1;
long long which;
if (offsets_buffer[0] != 0 || offsets_buffer[stored_documents] != stored_total_vectors)
	ok = 0;
if (ok)
	for (which = 0; which < stored_documents; which++)
		{
		if (offsets_buffer[which + 1] < offsets_buffer[which])
			{ ok = 0; break; }
		if (offsets_buffer[which + 1] - offsets_buffer[which] != counts_buffer[which])
			{ ok = 0; break; }
		}

if (!ok)
	{
	delete [] counts_buffer;
	delete [] offsets_buffer;
	delete [] pool_f_buffer;
	delete [] pool_q_buffer;
	delete [] qmin_buffer;
	delete [] qmax_buffer;
	fclose(fp);
	return result;
	}

fclose(fp);

result->dimension = stored_dimension;
result->documents = stored_documents;
result->total_vectors = stored_total_vectors;
result->quantized = stored_quant_flag;
result->counts = counts_buffer;
result->offsets = offsets_buffer;
result->pool_f = pool_f_buffer;
result->pool_q = pool_q_buffer;
result->qmin = qmin_buffer;
result->qmax = qmax_buffer;
return result;
}

/*
	ANT_MULTIVECTOR_STORE::MAXSIM()
	----------------------------------
	Late-interaction MaxSim: for each query vector, take the maximum dot
	product against every stored vector for this document, then sum over the
	query vectors.  Vectors are L2-normalized on write, so dot product is the
	appropriate similarity kernel.
*/
double ANT_multivector_store::maxsim(long long docid, const float *query_vecs, long long num_query_vecs)
{
if (!has(docid) || num_query_vecs <= 0)
	return 0.0;

long long start = offsets[docid];
long long end = offsets[docid + 1];
double total = 0.0;
float stack_tmp[512];
float *heap_tmp = NULL;
float *tmp = stack_tmp;

if (quantized && dimension > 512)
	{
	heap_tmp = new float[dimension];
	tmp = heap_tmp;
	}

for (long long i = 0; i < num_query_vecs; i++)
	{
	double best = -1e30;
	const float *q = query_vecs + i * dimension;
	for (long long j = start; j < end; j++)
		{
		const float *v;
		if (quantized)
			{
			ANT_vector_quantize::reconstruct(pool_q + j * dimension, dimension, qmin, qmax, tmp);
			v = tmp;
			}
		else
			v = pool_f + j * dimension;

		double dot = 0.0;
		for (long long d = 0; d < dimension; d++)
			dot += (double)q[d] * (double)v[d];
		if (dot > best)
			best = dot;
		}
	total += best;
	}

delete [] heap_tmp;
return total;
}

/*
	ANT_MULTIVECTOR_STORE_WRITER::ANT_MULTIVECTOR_STORE_WRITER()
	---------------------------------------------------------------
*/
ANT_multivector_store_writer::ANT_multivector_store_writer()
{
filename = NULL;
dimension = 0;
quant_mode = QUANT_OFF;
buffer = NULL;
buffer_capacity = 0;
total_vectors = 0;
counts = NULL;
counts_capacity = 0;
documents = 0;
}

/*
	ANT_MULTIVECTOR_STORE_WRITER::~ANT_MULTIVECTOR_STORE_WRITER()
	------------------------------------------------------------------
*/
ANT_multivector_store_writer::~ANT_multivector_store_writer()
{
abandon();
}

/*
	ANT_MULTIVECTOR_STORE_WRITER::CREATE()
	------------------------------------------
	Resets any prior state, so a writer may be reused across create() calls.
*/
long ANT_multivector_store_writer::create(const char *path, long long dim)
{
if (dim < 1 || dim > 65536)
	return 1;

abandon();
filename = strdup(path);
if (filename == NULL)
	return 1;

dimension = dim;
quant_mode = QUANT_OFF;
buffer_capacity = 1024;
buffer = new float[buffer_capacity * dimension];
counts_capacity = 256;
counts = new int[counts_capacity];
memset(counts, 0, (size_t)(counts_capacity * sizeof(int)));
total_vectors = 0;
documents = 0;
return 0;
}

/*
	ANT_MULTIVECTOR_STORE_WRITER::APPEND()
	-------------------------------------------
*/
long ANT_multivector_store_writer::append(const float *vectors, long long num_vectors)
{
if (buffer == NULL || counts == NULL)
	return 1;

if (documents >= counts_capacity)
	{
	long long new_capacity = counts_capacity * 2;
	int *new_counts = new int[new_capacity];
	memset(new_counts, 0, (size_t)(new_capacity * sizeof(int)));
	memcpy(new_counts, counts, (size_t)(documents * sizeof(int)));
	delete [] counts;
	counts = new_counts;
	counts_capacity = new_capacity;
	}

counts[documents] = (int)num_vectors;

if (num_vectors > 0 && vectors != NULL)
	{
	long long needed = total_vectors + num_vectors;
	if (needed > buffer_capacity)
		{
		long long new_capacity = buffer_capacity;
		while (new_capacity < needed)
			new_capacity *= 2;
		float *new_buffer = new float[new_capacity * dimension];
		memcpy(new_buffer, buffer, (size_t)(total_vectors * dimension * sizeof(float)));
		delete [] buffer;
		buffer = new_buffer;
		buffer_capacity = new_capacity;
		}

	long long which;
	for (which = 0; which < num_vectors; which++)
		{
		float *dest = buffer + (total_vectors + which) * dimension;
		memcpy(dest, vectors + which * dimension, (size_t)(dimension * sizeof(float)));

		double sum = 0.0;
		long long d;
		for (d = 0; d < dimension; d++)
			sum += (double)dest[d] * (double)dest[d];
		if (sum > 0.0)
			{
			double scale = 1.0 / sqrt(sum);
			for (d = 0; d < dimension; d++)
				dest[d] = (float)(dest[d] * scale);
			}
		}
	total_vectors += num_vectors;
	}

documents++;
return 0;
}

/*
	ANT_MULTIVECTOR_STORE_WRITER::FINISH()
	--------------------------------------------
	Write-temp then rename, per the crash-safety convention.  The writer's
	buffer/counts allocations belong to this object (create-reuse contract)
	and are NOT freed here -- only abandon() / the destructor free them.
*/
long ANT_multivector_store_writer::finish(void)
{
if (buffer == NULL || counts == NULL || filename == NULL)
	return 1;

long long *offsets = new long long[documents + 1];
long long which;
offsets[0] = 0;
for (which = 0; which < documents; which++)
	offsets[which + 1] = offsets[which] + counts[which];

float *mins = NULL;
float *maxs = NULL;
signed char *codes = NULL;
int quant_flag = (quant_mode == QUANT_INT8) ? 1 : 0;

if (quant_flag)
	{
	mins = new float[dimension];
	maxs = new float[dimension];
	ANT_vector_quantize::compute_ranges(buffer, dimension, total_vectors, mins, maxs);

	codes = new signed char[total_vectors * dimension > 0 ? total_vectors * dimension : 1];
	for (which = 0; which < total_vectors; which++)
		ANT_vector_quantize::quantize(buffer + which * dimension, dimension, mins, maxs, codes + which * dimension);
	}

char temp_name[4200];
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	{
	delete [] offsets;
	delete [] mins;
	delete [] maxs;
	delete [] codes;
	return 1;
	}

FILE *fp = fopen(temp_name, "wb");
if (fp == NULL)
	{
	delete [] offsets;
	delete [] mins;
	delete [] maxs;
	delete [] codes;
	return 1;
	}

long failed = 0;
if (fwrite(&ANT_MULTIVECTOR_STORE_MAGIC, sizeof(ANT_MULTIVECTOR_STORE_MAGIC), 1, fp) != 1
	|| fwrite(&dimension, sizeof(dimension), 1, fp) != 1
	|| fwrite(&documents, sizeof(documents), 1, fp) != 1
	|| fwrite(&total_vectors, sizeof(total_vectors), 1, fp) != 1
	|| fwrite(&quant_flag, sizeof(quant_flag), 1, fp) != 1
	|| fwrite(counts, sizeof(int), (size_t)documents, fp) != (size_t)documents
	|| fwrite(offsets, sizeof(long long), (size_t)(documents + 1), fp) != (size_t)(documents + 1))
	failed = 1;

if (!failed)
	{
	if (quant_flag)
		{
		if (fwrite(codes, sizeof(signed char), (size_t)(total_vectors * dimension), fp) != (size_t)(total_vectors * dimension))
			failed = 1;
		}
	else
		{
		if (fwrite(buffer, sizeof(float), (size_t)(total_vectors * dimension), fp) != (size_t)(total_vectors * dimension))
			failed = 1;
		}
	}

if (!failed && quant_flag)
	{
	if (fwrite(mins, sizeof(float), (size_t)dimension, fp) != (size_t)dimension
		|| fwrite(maxs, sizeof(float), (size_t)dimension, fp) != (size_t)dimension)
		failed = 1;
	}

if (failed)
	{
	fclose(fp);
	remove(temp_name);
	delete [] offsets;
	delete [] mins;
	delete [] maxs;
	delete [] codes;
	return 1;
	}

fclose(fp);
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	delete [] offsets;
	delete [] mins;
	delete [] maxs;
	delete [] codes;
	return 1;
	}

delete [] offsets;
delete [] mins;
delete [] maxs;
delete [] codes;
return 0;
}

/*
	ANT_MULTIVECTOR_STORE_WRITER::ABANDON()
	---------------------------------------------
*/
void ANT_multivector_store_writer::abandon(void)
{
delete [] buffer;
delete [] counts;
free(filename);
buffer = NULL;
counts = NULL;
filename = NULL;
buffer_capacity = 0;
counts_capacity = 0;
total_vectors = 0;
documents = 0;
}

/*
	PQ_STORE.CPP -- see pq_store.h for the contract.

	On-disk layout (".pq" sidecar), all fields little-endian native:
		magic       8 bytes  "ANTPQ001"
		version     u32      1
		dimension   i64
		documents   i64
		m           i64
		k           i64      (== ANT_pq_codec::K, 256)
		presence    (documents+7)/8 bytes, bit d = 1 iff docid d has a vector
		codebook    m*K*(dimension/m) floats
		codes       documents*m bytes

	Header size is fixed at 44 bytes (8 + 4 + 8*4).  Every document -- present
	or absent -- gets an encoded row in `codes`, but absent rows are encodings
	of an all-zero vector and are never surfaced (has() gates every accessor).
	This keeps codes/presence in lockstep by docid, which simplifies random
	access and the forgiving-load size check below.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pq_store.h"
#include "pq_codec.h"
#include "index_tombstones.h"
#include "vector_store.h"

static const char ANT_PQ_STORE_MAGIC[8] = { 'A', 'N', 'T', 'P', 'Q', '0', '0', '1' };
static const unsigned int ANT_PQ_STORE_VERSION = 1;
enum { ANT_PQ_STORE_HEADER_SIZE = 8 + 4 + 8 + 8 + 8 + 8 };	// magic, version, dimension, documents, m, k

/*
	ANT_PQ_STORE::ANT_PQ_STORE()
	------------------------------
*/
ANT_pq_store::ANT_pq_store()
{
dimension = 0;
documents = 0;
m = 0;
metric = 0;
presence = NULL;
codebook = NULL;
codes = NULL;
}

/*
	ANT_PQ_STORE::~ANT_PQ_STORE()
	--------------------------------
*/
ANT_pq_store::~ANT_pq_store()
{
delete [] presence;
delete [] codebook;
delete [] codes;
}

/*
	ANT_PQ_STORE::LOAD()
	-----------------------
	Forgiving load: any validation failure degrades to an empty store (mirrors
	ANT_multivector_store::load).  Validation happens strictly before any
	size-driven allocation so a lying header cannot trigger an absurd new[].
*/
ANT_pq_store *ANT_pq_store::load(const char *filename, long long expected_dimension, long long expected_documents, long metric)
{
FILE *fp;
char stored_magic[8];
unsigned int stored_version;
long long stored_dimension, stored_documents, stored_m, stored_k;
long long file_size, expected_size;
ANT_pq_store *result = new ANT_pq_store();

if ((fp = fopen(filename, "rb")) == NULL)
	return result;

if (fread(stored_magic, 1, 8, fp) != 8
	|| fread(&stored_version, sizeof(stored_version), 1, fp) != 1
	|| fread(&stored_dimension, sizeof(stored_dimension), 1, fp) != 1
	|| fread(&stored_documents, sizeof(stored_documents), 1, fp) != 1
	|| fread(&stored_m, sizeof(stored_m), 1, fp) != 1
	|| fread(&stored_k, sizeof(stored_k), 1, fp) != 1)
	{
	fclose(fp);
	return result;
	}

if (memcmp(stored_magic, ANT_PQ_STORE_MAGIC, 8) != 0
	|| stored_version != ANT_PQ_STORE_VERSION
	|| stored_dimension != expected_dimension
	|| stored_documents != expected_documents
	|| stored_documents < 0 || stored_documents > (1LL << 40)
	|| stored_dimension < 1 || stored_dimension > 65536
	|| stored_m < 1 || stored_m > stored_dimension
	|| stored_dimension % stored_m != 0
	|| stored_k != ANT_pq_codec::K)
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
long long presence_bytes = (stored_documents + 7) / 8;
long long codebook_floats = stored_m * (long long)ANT_pq_codec::K * (stored_dimension / stored_m);
long long codes_bytes = stored_documents * stored_m;

expected_size = ANT_PQ_STORE_HEADER_SIZE
	+ presence_bytes
	+ codebook_floats * (long long)sizeof(float)
	+ codes_bytes;

if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) != expected_size || fseek(fp, ANT_PQ_STORE_HEADER_SIZE, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

unsigned char *presence_buffer = new unsigned char[presence_bytes > 0 ? presence_bytes : 1];
float *codebook_buffer = new float[codebook_floats > 0 ? codebook_floats : 1];
unsigned char *codes_buffer = new unsigned char[codes_bytes > 0 ? codes_bytes : 1];

if ((presence_bytes > 0 && fread(presence_buffer, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
	|| (codebook_floats > 0 && fread(codebook_buffer, sizeof(float), (size_t)codebook_floats, fp) != (size_t)codebook_floats)
	|| (codes_bytes > 0 && fread(codes_buffer, 1, (size_t)codes_bytes, fp) != (size_t)codes_bytes))
	{
	delete [] presence_buffer;
	delete [] codebook_buffer;
	delete [] codes_buffer;
	fclose(fp);
	return result;
	}

fclose(fp);

result->dimension = stored_dimension;
result->documents = stored_documents;
result->m = stored_m;
result->metric = metric;
result->presence = presence_buffer;
result->codebook = codebook_buffer;
result->codes = codes_buffer;
return result;
}

/*
	ANT_PQ_STORE::RECONSTRUCT()
	------------------------------
*/
void ANT_pq_store::reconstruct(long long docid, float *out)
{
if (!has(docid))
	{
	memset(out, 0, (size_t)dimension * sizeof(float));
	return;
	}
ANT_pq_codec::reconstruct(codes + docid * m, dimension, m, codebook, out);
}

/*
	ANT_PQ_STORE::SCORE()
	-------------------------
	Builds a per-call ADC table using the caller-supplied `metric` (not the
	store's configured metric): the codebook's subspaces are metric-agnostic,
	and the caller (e.g. ANT_hnsw, honoring the graph/query's metric) is the
	authority on which kernel to score with.  Task 3 batches this per-query
	table across candidates instead of rebuilding it on every score() call.
*/
double ANT_pq_store::score(long long docid, const float *query, long metric)
{
if (!has(docid))
	return 0.0;

double stack_table[64 * 256];
double *table = stack_table;
long long table_size = m * (long long)ANT_pq_codec::K;
if (table_size > (long long)(sizeof(stack_table) / sizeof(stack_table[0])))
	table = new double[table_size];

ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);
double result = ANT_pq_codec::adc_score(codes + docid * m, m, table);

if (table != stack_table)
	delete [] table;

return result;
}

/*
	ANT_PQ_STORE::SCAN_ADC()
	----------------------------
	Builds the m*K ADC table once for `query`, then does a single pass over all
	documents inserting into the fixed-capacity top-k candidate set. Mirrors
	ANT_vector_store::scan()'s presence/tombstone/filter-bit gating.
*/
void ANT_pq_store::scan_adc(const float *query, long metric, ANT_index_tombstones *tombstones, long long generation,
	ANT_vector_candidate *best, long long *best_count, long long top_k, const unsigned char *filter_bits)
{
if (documents == 0 || codebook == 0)
	return;

long long K = ANT_pq_codec::K;
double *table = new double[m * K];
ANT_pq_codec::adc_table(query, dimension, m, codebook, metric, table);

for (long long d = 0; d < documents; d++)
	{
	if (!has(d))
		continue;
	if (tombstones != 0 && tombstones->is_deleted(d))
		continue;
	if (filter_bits != 0 && !(filter_bits[d >> 3] & (1 << (d & 7))))
		continue;
	ANT_vector_candidate_insert(best, best_count, top_k, ANT_pq_codec::adc_score(codes + d*m, m, table), generation, d);
	}

delete [] table;
}

/*
	ANT_PQ_STORE_WRITER::ANT_PQ_STORE_WRITER()
	----------------------------------------------
*/
ANT_pq_store_writer::ANT_pq_store_writer()
{
filename = NULL;
dimension = 0;
m = 0;
metric = 0;
buffer = NULL;
capacity = 0;
documents = 0;
presence = NULL;
presence_capacity = 0;
}

/*
	ANT_PQ_STORE_WRITER::~ANT_PQ_STORE_WRITER()
	-----------------------------------------------
*/
ANT_pq_store_writer::~ANT_pq_store_writer()
{
abandon();
}

/*
	ANT_PQ_STORE_WRITER::CREATE()
	---------------------------------
	Resets any prior state, so a writer may be reused across create() calls.
*/
long ANT_pq_store_writer::create(const char *path, long long dim, long long m_arg, long metric_arg)
{
abandon();

if (m_arg < 1 || dim < 1 || dim % m_arg != 0)
	return 1;

filename = strdup(path);
if (filename == NULL)
	return 1;

dimension = dim;
m = m_arg;
metric = metric_arg;
capacity = 1024;
buffer = new float[capacity * dimension];
presence_capacity = (1024 + 7) / 8;
presence = new unsigned char[presence_capacity];
memset(presence, 0, (size_t)presence_capacity);
documents = 0;
return 0;
}

/*
	ANT_PQ_STORE_WRITER::APPEND()
	---------------------------------
*/
long ANT_pq_store_writer::append(const float *vector_or_null)
{
if (buffer == NULL || presence == NULL)
	return 1;

if (documents >= capacity)
	{
	long long new_capacity = capacity * 2;
	float *new_buffer = new float[new_capacity * dimension];
	memcpy(new_buffer, buffer, (size_t)(documents * dimension * sizeof(float)));
	delete [] buffer;
	buffer = new_buffer;
	capacity = new_capacity;
	}

if ((documents / 8) >= presence_capacity)
	{
	long long new_presence_capacity = presence_capacity * 2;
	unsigned char *new_presence = new unsigned char[new_presence_capacity];
	memset(new_presence, 0, (size_t)new_presence_capacity);
	memcpy(new_presence, presence, (size_t)presence_capacity);
	delete [] presence;
	presence = new_presence;
	presence_capacity = new_presence_capacity;
	}

float *dest = buffer + documents * dimension;
if (vector_or_null != NULL)
	{
	memcpy(dest, vector_or_null, (size_t)dimension * sizeof(float));
	presence[documents / 8] |= (unsigned char)(1 << (documents % 8));
	}
else
	{
	memset(dest, 0, (size_t)dimension * sizeof(float));
	presence[documents / 8] &= (unsigned char)~(1 << (documents % 8));
	}

documents++;
return 0;
}

/*
	ANT_PQ_STORE_WRITER::FINISH()
	---------------------------------
	Trains the codebook on the present rows only, then encodes every row
	(present or absent) so codes/presence stay in lockstep by docid.  Writes
	write-temp then rename, per the crash-safety convention.  The writer's
	buffer/presence allocations belong to this object (create-reuse contract)
	and are NOT freed here -- only abandon() / the destructor free them.
*/
long ANT_pq_store_writer::finish(void)
{
if (buffer == NULL || presence == NULL || filename == NULL)
	return 1;

long long sub = dimension / m;
long long codebook_floats = m * (long long)ANT_pq_codec::K * sub;
long long presence_bytes = (documents + 7) / 8;
long long codes_bytes = documents * m;

long long present_count = 0, i;
for (i = 0; i < documents; i++)
	if (presence[i / 8] & (1 << (i % 8)))
		present_count++;

float *present_rows = new float[(present_count > 0 ? present_count : 1) * dimension];
long long w = 0;
for (i = 0; i < documents; i++)
	if (presence[i / 8] & (1 << (i % 8)))
		{
		memcpy(present_rows + w * dimension, buffer + i * dimension, (size_t)dimension * sizeof(float));
		w++;
		}

float *codebook = new float[codebook_floats > 0 ? codebook_floats : 1];
long train_rc = ANT_pq_codec::train(present_rows, dimension, m, present_count, codebook);
delete [] present_rows;

if (train_rc != 0)
	{
	delete [] codebook;
	return 1;
	}

unsigned char *codes = new unsigned char[codes_bytes > 0 ? codes_bytes : 1];
for (i = 0; i < documents; i++)
	ANT_pq_codec::encode(buffer + i * dimension, dimension, m, codebook, codes + i * m);

char temp_name[4200];
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	{
	delete [] codebook;
	delete [] codes;
	return 1;
	}

FILE *fp = fopen(temp_name, "wb");
if (fp == NULL)
	{
	delete [] codebook;
	delete [] codes;
	return 1;
	}

long long k = ANT_pq_codec::K;
unsigned int version = ANT_PQ_STORE_VERSION;
long failed = 0;

if (fwrite(ANT_PQ_STORE_MAGIC, 1, 8, fp) != 8
	|| fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&dimension, sizeof(dimension), 1, fp) != 1
	|| fwrite(&documents, sizeof(documents), 1, fp) != 1
	|| fwrite(&m, sizeof(m), 1, fp) != 1
	|| fwrite(&k, sizeof(k), 1, fp) != 1)
	failed = 1;

if (!failed && presence_bytes > 0 && fwrite(presence, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
	failed = 1;

if (!failed && codebook_floats > 0 && fwrite(codebook, sizeof(float), (size_t)codebook_floats, fp) != (size_t)codebook_floats)
	failed = 1;

if (!failed && codes_bytes > 0 && fwrite(codes, 1, (size_t)codes_bytes, fp) != (size_t)codes_bytes)
	failed = 1;

delete [] codebook;
delete [] codes;

if (failed)
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
	ANT_PQ_STORE_WRITER::ABANDON()
	----------------------------------
*/
void ANT_pq_store_writer::abandon(void)
{
delete [] buffer;
delete [] presence;
free(filename);
buffer = NULL;
presence = NULL;
filename = NULL;
capacity = 0;
presence_capacity = 0;
documents = 0;
}

/*
	PQ_STORE.CPP -- see pq_store.h for the contract.

	On-disk layout (".pq" sidecar), all fields little-endian native:
		magic       8 bytes  "ANTPQ001"
		version     u32      1 (legacy) or 2 (OPQ-capable)
		dimension   i64
		documents   i64
		m           i64
		k           i64      (== ANT_pq_codec::K, 256)
		opq         i64      (v2 only) 0 = no rotation, 1 = R block present
		presence    (documents+7)/8 bytes, bit d = 1 iff docid d has a vector
		codebook    m*K*(dimension/m) floats
		codes       documents*m bytes
		rotation    dimension*dimension floats (only when opq == 1)

	Header size is 44 bytes for v1 (8 + 4 + 8*4) or 52 bytes for v2 (adds the
	opq i64).  Every document -- present or absent -- gets an encoded row in
	`codes`, but absent rows are encodings of an all-zero vector and are never
	surfaced (has() gates every accessor).  This keeps codes/presence in
	lockstep by docid, which simplifies random access and the forgiving-load
	size check below.  Under OPQ the codebook/codes live in the ROTATED space
	(every stored vector was R*x); reconstruct un-rotates via R^T and queries
	are rotated by R before building the ADC table (both metric-preserving).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pq_store.h"
#include "pq_codec.h"
#include "index_tombstones.h"
#include "vector_store.h"

static const char ANT_PQ_STORE_MAGIC[8] = { 'A', 'N', 'T', 'P', 'Q', '0', '0', '1' };
static const unsigned int ANT_PQ_STORE_VERSION = 2;
static const unsigned int ANT_PQ_STORE_VERSION_V1 = 1;
enum { ANT_PQ_STORE_HEADER_SIZE_V1 = 8 + 4 + 8 + 8 + 8 + 8 };		// v1: magic, version, dimension, documents, m, k
enum { ANT_PQ_STORE_HEADER_SIZE = 8 + 4 + 8 + 8 + 8 + 8 + 8 };	// v2: + opq i64

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
rotation = NULL;
adc_table_builds = 0;
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
delete [] rotation;
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
long long stored_dimension, stored_documents, stored_m, stored_k, stored_opq = 0;
long long file_size, expected_size, header_size;
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

/*
	v1 has no opq field (header 44 bytes); v2 adds an opq i64 (header 52 bytes).
	Read the extra field only for v2 so old files still load with rotation=NULL.
*/
if (stored_version == ANT_PQ_STORE_VERSION)
	{
	if (fread(&stored_opq, sizeof(stored_opq), 1, fp) != 1)
		{
		fclose(fp);
		return result;
		}
	header_size = ANT_PQ_STORE_HEADER_SIZE;
	}
else
	{
	stored_opq = 0;
	header_size = ANT_PQ_STORE_HEADER_SIZE_V1;
	}

if (memcmp(stored_magic, ANT_PQ_STORE_MAGIC, 8) != 0
	|| (stored_version != ANT_PQ_STORE_VERSION && stored_version != ANT_PQ_STORE_VERSION_V1)
	|| stored_dimension != expected_dimension
	|| stored_documents != expected_documents
	|| stored_documents < 0 || stored_documents > (1LL << 40)
	|| stored_dimension < 1 || stored_dimension > 65536
	|| stored_m < 1 || stored_m > stored_dimension
	|| stored_dimension % stored_m != 0
	|| stored_k != ANT_pq_codec::K
	|| (stored_opq != 0 && stored_opq != 1))
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
/*
	#25: unlike the token .mvpq path, no extra overflow bound is needed here --
	stored_documents is capped at 2^40 and stored_dimension at 65536 (validated
	above), so stored_documents*stored_m <= 2^56 and codebook_floats*4 <= 2^26,
	both well below LLONG_MAX; the products cannot signed-overflow.
*/
long long presence_bytes = (stored_documents + 7) / 8;
long long codebook_floats = stored_m * (long long)ANT_pq_codec::K * (stored_dimension / stored_m);
long long codes_bytes = stored_documents * stored_m;
/*
	Under OPQ a dimension*dimension float R block follows the codes.  stored_dimension
	is capped at 65536 above, so rotation_floats <= 2^32 and *4 <= 2^34 -- fits i64
	without signed overflow, and is validated (below) against the actual file size
	before any allocation.
*/
long long rotation_floats = stored_opq ? stored_dimension * stored_dimension : 0;

expected_size = header_size
	+ presence_bytes
	+ codebook_floats * (long long)sizeof(float)
	+ codes_bytes
	+ rotation_floats * (long long)sizeof(float);

if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) != expected_size || fseek(fp, header_size, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

unsigned char *presence_buffer = new unsigned char[presence_bytes > 0 ? presence_bytes : 1];
float *codebook_buffer = new float[codebook_floats > 0 ? codebook_floats : 1];
unsigned char *codes_buffer = new unsigned char[codes_bytes > 0 ? codes_bytes : 1];
float *rotation_buffer = rotation_floats > 0 ? new float[rotation_floats] : NULL;

if ((presence_bytes > 0 && fread(presence_buffer, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
	|| (codebook_floats > 0 && fread(codebook_buffer, sizeof(float), (size_t)codebook_floats, fp) != (size_t)codebook_floats)
	|| (codes_bytes > 0 && fread(codes_buffer, 1, (size_t)codes_bytes, fp) != (size_t)codes_bytes)
	|| (rotation_floats > 0 && fread(rotation_buffer, sizeof(float), (size_t)rotation_floats, fp) != (size_t)rotation_floats))
	{
	delete [] presence_buffer;
	delete [] codebook_buffer;
	delete [] codes_buffer;
	delete [] rotation_buffer;
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
result->rotation = rotation_buffer;
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
if (rotation != NULL)
	{
	/* codes live in rotated space -> reconstruct there, then un-rotate via R^T into original space */
	float *tmp = new float[dimension];
	ANT_pq_codec::reconstruct(codes + docid * m, dimension, m, codebook, tmp);
	ANT_pq_codec::apply_rotation_transpose(tmp, dimension, rotation, out);
	delete [] tmp;
	}
else
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

double stack_table[PQ_SCORE_STACK_CAP];			/* 16 KB, safe on worker stacks */
double *table = stack_table;
long long table_size = m * (long long)ANT_pq_codec::K;
if (table_size > (long long)PQ_SCORE_STACK_CAP)
	table = new double[table_size];				/* larger m: heap the ADC table */

const float *q = query;
float *rq = NULL;
if (rotation != NULL)
	{
	rq = new float[dimension];
	ANT_pq_codec::apply_rotation(query, dimension, rotation, rq);
	q = rq;
	}
ANT_pq_codec::adc_table(q, dimension, m, codebook, metric, table);
delete [] rq;					/* delete[] NULL is a no-op */
adc_table_builds++;
double result = ANT_pq_codec::adc_score(codes + docid * m, m, table);

if (table != stack_table)
	delete [] table;

return result;
}

/*
	ANT_PQ_STORE::PREPARE_QUERY / SCORE_PREPARED / FREE_QUERY
	--------------------------------------------------------
	Build the m*K ADC table once per query (prepare_query), reuse it across every
	node (score_prepared), free it once (free_query). ANT_hnsw::search threads the
	returned ctx through distance() so NONE-tier navigation builds the table once
	per search instead of once per visited node. The table encodes (query, metric);
	score_prepared ignores its own query/metric when ctx != NULL, which is sound
	because a single search uses one fixed query and metric.
*/
void *ANT_pq_store::prepare_query(const float *query, long metric)
{
if (documents == 0 || codebook == 0)
	return 0;								/* degraded store: ctx==NULL -> score_prepared falls back */
double *table = new double[m * (long long)ANT_pq_codec::K];
const float *q = query;
float *rq = NULL;
if (rotation != NULL)
	{
	rq = new float[dimension];			/* one D-matvec per search, not per node */
	ANT_pq_codec::apply_rotation(query, dimension, rotation, rq);
	q = rq;
	}
ANT_pq_codec::adc_table(q, dimension, m, codebook, metric, table);
delete [] rq;
adc_table_builds++;
return table;
}

double ANT_pq_store::score_prepared(long long docid, const float *query, long metric, void *ctx)
{
if (ctx == 0)
	return score(docid, query, metric);		/* no prepared table -> per-call build (e.g. build path) */
if (!has(docid))
	return 0.0;
return ANT_pq_codec::adc_score(codes + docid * m, m, (double *)ctx);
}

void ANT_pq_store::free_query(void *ctx)
{
delete [] (double *)ctx;					/* delete[] NULL is a no-op */
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
const float *q = query;
float *rq = NULL;
if (rotation != NULL)
	{
	rq = new float[dimension];
	ANT_pq_codec::apply_rotation(query, dimension, rotation, rq);
	q = rq;
	}
ANT_pq_codec::adc_table(q, dimension, m, codebook, metric, table);
delete [] rq;

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
opq = 0;
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
long ANT_pq_store_writer::create(const char *path, long long dim, long long m_arg, long metric_arg, long opq_arg)
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
opq = opq_arg ? 1 : 0;
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

/*
	OPQ: learn R over the present rows (original space), then rotate BOTH the
	codebook-training rows (present_rows) and every encode row (buffer) in place
	so the codebook is trained on -- and every code encodes -- R*x.  Absent buffer
	rows are all-zero and rotate to zero (harmless).  The non-OPQ path skips all of
	this and stays byte-identical to the pre-OPQ writer.
*/
float *rotation = NULL;
if (opq)
	{
	rotation = new float[dimension * dimension];
	if (ANT_pq_codec::train_rotation(present_rows, dimension, m, present_count, rotation) != 0)
		{
		delete [] rotation;
		delete [] present_rows;
		return 1;
		}
	float *tmp = new float[dimension];
	for (i = 0; i < present_count; i++)
		{
		ANT_pq_codec::apply_rotation(present_rows + i * dimension, dimension, rotation, tmp);
		memcpy(present_rows + i * dimension, tmp, (size_t)dimension * sizeof(float));
		}
	for (i = 0; i < documents; i++)
		{
		ANT_pq_codec::apply_rotation(buffer + i * dimension, dimension, rotation, tmp);
		memcpy(buffer + i * dimension, tmp, (size_t)dimension * sizeof(float));
		}
	delete [] tmp;
	}

float *codebook = new float[codebook_floats > 0 ? codebook_floats : 1];
long train_rc = ANT_pq_codec::train(present_rows, dimension, m, present_count, codebook);
delete [] present_rows;

if (train_rc != 0)
	{
	delete [] codebook;
	delete [] rotation;
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
	delete [] rotation;
	return 1;
	}

FILE *fp = fopen(temp_name, "wb");
if (fp == NULL)
	{
	delete [] codebook;
	delete [] codes;
	delete [] rotation;
	return 1;
	}

long long k = ANT_pq_codec::K;
long long opq_flag = opq ? 1 : 0;
unsigned int version = ANT_PQ_STORE_VERSION;
long failed = 0;

if (fwrite(ANT_PQ_STORE_MAGIC, 1, 8, fp) != 8
	|| fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&dimension, sizeof(dimension), 1, fp) != 1
	|| fwrite(&documents, sizeof(documents), 1, fp) != 1
	|| fwrite(&m, sizeof(m), 1, fp) != 1
	|| fwrite(&k, sizeof(k), 1, fp) != 1
	|| fwrite(&opq_flag, sizeof(opq_flag), 1, fp) != 1)
	failed = 1;

if (!failed && presence_bytes > 0 && fwrite(presence, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
	failed = 1;

if (!failed && codebook_floats > 0 && fwrite(codebook, sizeof(float), (size_t)codebook_floats, fp) != (size_t)codebook_floats)
	failed = 1;

if (!failed && codes_bytes > 0 && fwrite(codes, 1, (size_t)codes_bytes, fp) != (size_t)codes_bytes)
	failed = 1;

if (!failed && opq && fwrite(rotation, sizeof(float), (size_t)(dimension*dimension), fp) != (size_t)(dimension*dimension))
	failed = 1;

delete [] codebook;
delete [] codes;
delete [] rotation;

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

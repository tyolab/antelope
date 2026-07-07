/*
	PAYLOAD_STORE.CPP
	------------------
	Ragged opaque-blob store for the per-segment .pay sidecar: one variable
	length byte payload per document, returned verbatim alongside a search
	hit (never indexed, never filtered).

	.pay on-disk layout
	--------------------
	header: magic "ANTPAY01" (8 bytes), version (u32 == 1), document_count
	(i64), total_bytes (i64).  Then:
		offsets[document_count+1] (i64 prefix sums; offsets[0] == 0,
			monotonic non-decreasing, offsets[document_count] == total_bytes)
		byte pool: total_bytes raw bytes

	A doc with an empty payload has offsets[d+1] == offsets[d] and reads
	back as (NULL, 0) -- absent and empty are indistinguishable, which is
	fine since payload bytes are opaque.

	load() validates incrementally, the same forgiving posture as
	attribute_store / vector_store / multivector_store: the file's total
	size is captured once via fseek/ftell, and before every allocation whose
	size is driven by a value just read from the file, the claimed size is
	checked against the file's remaining bytes before the new[] happens.
	Any corrupt/truncated/mismatched file degrades to an empty store rather
	than crashing or over-allocating.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "payload_store.h"

static const char ANT_PAYLOAD_STORE_MAGIC[8] = {'A', 'N', 'T', 'P', 'A', 'Y', '0', '1'};

/*
	ANT_PAYLOAD_STORE::ANT_PAYLOAD_STORE()
	-----------------------------------------
*/
ANT_payload_store::ANT_payload_store()
{
documents = 0;
total_bytes = 0;
offsets = NULL;
pool = NULL;
}

/*
	ANT_PAYLOAD_STORE::~ANT_PAYLOAD_STORE()
	--------------------------------------------
*/
ANT_payload_store::~ANT_payload_store()
{
delete [] offsets;
delete [] pool;
}

/*
	ANT_PAYLOAD_STORE::LOAD()
	------------------------------
	Any validation failure returns a degraded empty store: the segment keeps
	working, it just has no payloads.  See the file header comment for the
	incremental validate-before-allocate discipline used here.
*/
ANT_payload_store *ANT_payload_store::load(const char *filename, long long expected_documents)
{
FILE *fp;
char stored_magic[8];
uint32_t version;
long long stored_documents, stored_total_bytes;
long long file_size, remaining;
ANT_payload_store *result = new ANT_payload_store();
long long *offsets_buf = NULL;
unsigned char *pool_buf = NULL;
long long d;

if ((fp = fopen(filename, "rb")) == NULL)
	return result;

if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

if (fread(stored_magic, 1, 8, fp) != 8 || memcmp(stored_magic, ANT_PAYLOAD_STORE_MAGIC, 8) != 0)
	{
	fclose(fp);
	return result;
	}
if (fread(&version, sizeof(version), 1, fp) != 1 || version != 1)
	{
	fclose(fp);
	return result;
	}
if (fread(&stored_documents, sizeof(stored_documents), 1, fp) != 1
	|| fread(&stored_total_bytes, sizeof(stored_total_bytes), 1, fp) != 1)
	{
	fclose(fp);
	return result;
	}

if (stored_documents != expected_documents || stored_documents < 0 || stored_documents > (1LL << 40))
	{
	fclose(fp);
	return result;
	}

remaining = file_size - ftell(fp);
if (stored_total_bytes < 0 || stored_total_bytes > remaining)
	{
	fclose(fp);
	return result;
	}

remaining = file_size - ftell(fp);
if (remaining < (stored_documents + 1) * (long long)sizeof(long long))
	{
	fclose(fp);
	return result;
	}

offsets_buf = new long long[(size_t)(stored_documents + 1)];
if (fread(offsets_buf, sizeof(long long), (size_t)(stored_documents + 1), fp) != (size_t)(stored_documents + 1))
	{
	delete [] offsets_buf;
	fclose(fp);
	return result;
	}

if (offsets_buf[0] != 0 || offsets_buf[stored_documents] != stored_total_bytes)
	{
	delete [] offsets_buf;
	fclose(fp);
	return result;
	}

for (d = 0; d < stored_documents; d++)
	if (offsets_buf[d + 1] < offsets_buf[d])
		{
		delete [] offsets_buf;
		fclose(fp);
		return result;
		}

remaining = file_size - ftell(fp);
if (stored_total_bytes > remaining)
	{
	delete [] offsets_buf;
	fclose(fp);
	return result;
	}

if (stored_total_bytes > 0)
	{
	pool_buf = new unsigned char[(size_t)stored_total_bytes];
	if (fread(pool_buf, 1, (size_t)stored_total_bytes, fp) != (size_t)stored_total_bytes)
		{
		delete [] offsets_buf;
		delete [] pool_buf;
		fclose(fp);
		return result;
		}
	}

fclose(fp);

result->documents = stored_documents;
result->total_bytes = stored_total_bytes;
result->offsets = offsets_buf;
result->pool = pool_buf;
return result;
}

/*
	ANT_PAYLOAD_STORE::GET()
	------------------------------
*/
void ANT_payload_store::get(long long docid, const unsigned char **out_ptr, long long *out_len)
{
long long start, len;

if (docid < 0 || docid >= documents || offsets == NULL)
	{
	*out_ptr = NULL;
	*out_len = 0;
	return;
	}

start = offsets[docid];
len = offsets[docid + 1] - start;
if (len <= 0)
	{
	*out_ptr = NULL;
	*out_len = 0;
	return;
	}

*out_ptr = pool + start;
*out_len = len;
}

/*
	ANT_PAYLOAD_STORE_WRITER::ANT_PAYLOAD_STORE_WRITER()
	---------------------------------------------------------
*/
ANT_payload_store_writer::ANT_payload_store_writer()
{
fp = NULL;
tempname = NULL;
documents = 0;
offsets = NULL;
offsets_capacity = 0;
pool = NULL;
pool_size = 0;
pool_capacity = 0;
}

/*
	ANT_PAYLOAD_STORE_WRITER::~ANT_PAYLOAD_STORE_WRITER()
	-----------------------------------------------------------
*/
ANT_payload_store_writer::~ANT_payload_store_writer()
{
abandon();
}

/*
	ANT_PAYLOAD_STORE_WRITER::CREATE()
	----------------------------------------
	Resets any prior state, so a writer may be reused across create() calls.
*/
long ANT_payload_store_writer::create(const char *path)
{
abandon();

if (path == NULL)
	return 1;

tempname = new char[strlen(path) + 1];
strcpy(tempname, path);

documents = 0;
offsets_capacity = 16;
offsets = new long long[(size_t)offsets_capacity];
offsets[0] = 0;

pool_capacity = 1024;
pool = new unsigned char[(size_t)pool_capacity];
pool_size = 0;

return 0;
}

/*
	ANT_PAYLOAD_STORE_WRITER::APPEND()
	----------------------------------------
*/
void ANT_payload_store_writer::append(const void *ptr, long long len)
{
if (tempname == NULL)
	return;
if (len < 0)
	len = 0;

if (documents + 2 > offsets_capacity)
	{
	long long new_cap = offsets_capacity > 0 ? offsets_capacity : 1;
	while (documents + 2 > new_cap)
		new_cap *= 2;
	long long *n = new long long[(size_t)new_cap];
	memcpy(n, offsets, (size_t)((documents + 1) * sizeof(long long)));
	delete [] offsets;
	offsets = n;
	offsets_capacity = new_cap;
	}

if (pool_size + len > pool_capacity)
	{
	long long new_cap = pool_capacity > 0 ? pool_capacity : 1;
	while (new_cap < pool_size + len)
		new_cap *= 2;
	unsigned char *n = new unsigned char[(size_t)new_cap];
	memcpy(n, pool, (size_t)pool_size);
	delete [] pool;
	pool = n;
	pool_capacity = new_cap;
	}

if (len > 0 && ptr != NULL)
	memcpy(pool + pool_size, ptr, (size_t)len);
pool_size += len;
offsets[documents + 1] = pool_size;
documents++;
}

/*
	ANT_PAYLOAD_STORE_WRITER::FINISH()
	----------------------------------------
	Write-temp then rename, per the crash-safety convention.  On any fwrite
	error the temp file is removed and 1 is returned; the writer's own
	buffers are left intact (only abandon()/the destructor free them).
*/
long ANT_payload_store_writer::finish(void)
{
char temp_name[4200];
FILE *out;
uint32_t version = 1;
long long field_documents = documents;
long long field_total_bytes = pool_size;
int failed = 0;

if (tempname == NULL)
	return 1;

if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", tempname) >= (int)sizeof(temp_name))
	return 1;
if ((out = fopen(temp_name, "wb")) == NULL)
	return 1;

if (fwrite(ANT_PAYLOAD_STORE_MAGIC, 1, 8, out) != 8
	|| fwrite(&version, sizeof(version), 1, out) != 1
	|| fwrite(&field_documents, sizeof(field_documents), 1, out) != 1
	|| fwrite(&field_total_bytes, sizeof(field_total_bytes), 1, out) != 1)
	failed = 1;

if (!failed && fwrite(offsets, sizeof(long long), (size_t)(documents + 1), out) != (size_t)(documents + 1))
	failed = 1;

if (!failed && pool_size > 0 && fwrite(pool, 1, (size_t)pool_size, out) != (size_t)pool_size)
	failed = 1;

fclose(out);
if (failed)
	{
	remove(temp_name);
	return 1;
	}
if (rename(temp_name, tempname) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}

/*
	ANT_PAYLOAD_STORE_WRITER::ABANDON()
	------------------------------------------
*/
void ANT_payload_store_writer::abandon(void)
{
if (tempname != NULL)
	{
	char temp_name[4200];
	if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", tempname) < (int)sizeof(temp_name))
		remove(temp_name);
	}

delete [] offsets;
offsets = NULL;
delete [] pool;
pool = NULL;
delete [] tempname;
tempname = NULL;

fp = NULL;
documents = 0;
offsets_capacity = 0;
pool_size = 0;
pool_capacity = 0;
}

/*
	ATTRIBUTE_STORE.CPP
	-------------------
	Schema value-type describing filterable document attribute fields, plus
	the columnar .attr sidecar loader/writer for per-document attribute
	values used by the filtered-ANN matcher.

	.attr on-disk layout
	--------------------
	header: magic "ANTATTRS" (8 bytes), version (u32 == 1), document_count
	(i64), field_count (i64).  Then, per field f in schema order:
		presence:  (document_count+7)/8 bytes bitset (bit d set => doc d HAS field f)
		column, by (schema.type(f), schema.is_multi(f)):
			INT64 single : int64[document_count]
			INT64 multi  : counts[document_count] (int32), offsets[document_count+1] (int64), pool int64[offsets[docs]]
			BOOL  single : value bitset (document_count+7)/8 bytes
			STRING single: dict_id[document_count] (int32)
			STRING multi : counts[document_count] (int32), offsets[document_count+1] (int64), id pool int32[offsets[docs]]
		and, for STRING fields ONLY, immediately after the column, the
		per-field dictionary: dict_count (i64), then dict_count entries of
		len (int32) + len bytes (no NUL); id == write order, 0-based.

	Values for a doc that is absent (presence bit 0) are still laid out but
	never read by any public accessor below (single-valued absent slots may
	hold 0; multi-valued absent docs have count 0).

	load() validates incrementally: the file's total size is captured once,
	and before every allocation whose size is driven by a value just read
	from the file (a counts/offsets/dict_count/string length), the claimed
	size is checked against the file's remaining bytes (and, for pool-style
	counts, against the whole file size, to avoid signed overflow in the
	subsequent size-in-bytes multiplication) before the new[] happens.  Any
	corrupt/truncated/oversized file degrades to an empty store rather than
	crashing or over-allocating -- the same forgiving posture as vector_store
	and multivector_store.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "attribute_store.h"

static const char ANT_ATTRIBUTE_STORE_MAGIC[8] = {'A', 'N', 'T', 'A', 'T', 'T', 'R', 'S'};

/*
	ANT_ATTRIBUTE_SCHEMA::FIELD_INDEX()
	------------------------------------
*/
long ANT_attribute_schema::field_index(const char *name) const
{
long i;

for (i = 0; i < field_count; i++)
	if (strcmp(names[i], name) == 0)
		return i;
return -1;
}

/*
	ANT_ATTRIBUTE_SCHEMA::ADD_FIELD()
	------------------------------------
*/
long ANT_attribute_schema::add_field(const char *name, int type, int multivalued)
{
if (type != TYPE_INT64 && type != TYPE_STRING && type != TYPE_BOOL)
	return 1;
if (multivalued && type == TYPE_BOOL)
	return 1;
if (field_count >= MAX_FIELDS)
	return 1;
if (name == NULL || strlen(name) >= sizeof(names[0]))
	return 1;
if (field_index(name) >= 0)
	return 1;

strcpy(names[field_count], name);
types[field_count] = type;
multi[field_count] = multivalued;
field_count++;
return 0;
}

/*
	ANT_ATTRIBUTE_SCHEMA::EQUALS()
	---------------------------------
*/
long ANT_attribute_schema::equals(const ANT_attribute_schema &o) const
{
long i;

if (field_count != o.field_count)
	return 0;
for (i = 0; i < field_count; i++)
	if (strcmp(names[i], o.names[i]) != 0 || types[i] != o.types[i] || multi[i] != o.multi[i])
		return 0;
return 1;
}

/*
	ANT_ATTRIBUTE_STORE::ANT_ATTRIBUTE_STORE()
	--------------------------------------------
*/
ANT_attribute_store::ANT_attribute_store()
{
long f;

documents = 0;
for (f = 0; f < ANT_attribute_schema::MAX_FIELDS; f++)
	{
	presence[f] = NULL;
	int_single[f] = NULL;
	int_counts[f] = NULL;
	int_offsets[f] = NULL;
	int_pool[f] = NULL;
	bool_bits[f] = NULL;
	str_single[f] = NULL;
	str_counts[f] = NULL;
	str_offsets[f] = NULL;
	str_pool[f] = NULL;
	dict_pool[f] = NULL;
	dict_offset[f] = NULL;
	dict_count[f] = 0;
	}
}

/*
	ANT_ATTRIBUTE_STORE::~ANT_ATTRIBUTE_STORE()
	----------------------------------------------
*/
ANT_attribute_store::~ANT_attribute_store()
{
long f;

for (f = 0; f < ANT_attribute_schema::MAX_FIELDS; f++)
	{
	delete [] presence[f];
	delete [] int_single[f];
	delete [] int_counts[f];
	delete [] int_offsets[f];
	delete [] int_pool[f];
	delete [] bool_bits[f];
	delete [] str_single[f];
	delete [] str_counts[f];
	delete [] str_offsets[f];
	delete [] str_pool[f];
	delete [] dict_pool[f];
	delete [] dict_offset[f];
	}
}

/*
	ANT_ATTRIBUTE_STORE::DOCUMENT_COUNT()
	-----------------------------------------
*/
long long ANT_attribute_store::document_count(void)
{
return documents;
}

/*
	ANT_ATTRIBUTE_STORE::LOAD()
	-------------------------------
	Any validation failure returns a degraded empty store: the segment keeps
	working, it just has no attributes.  See the file header comment for the
	incremental validate-before-allocate discipline used here.
*/
ANT_attribute_store *ANT_attribute_store::load(const char *filename, const ANT_attribute_schema *schema, long long expected_documents)
{
FILE *fp;
char stored_magic[8];
uint32_t version;
long long stored_documents, stored_field_count;
long long file_size, remaining;
ANT_attribute_store *result = new ANT_attribute_store();
long f;
int failed = 0;

if (schema == NULL)
	return result;

if ((fp = fopen(filename, "rb")) == NULL)
	return result;

if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

if (fread(stored_magic, 1, 8, fp) != 8 || memcmp(stored_magic, ANT_ATTRIBUTE_STORE_MAGIC, 8) != 0)
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
	|| fread(&stored_field_count, sizeof(stored_field_count), 1, fp) != 1)
	{
	fclose(fp);
	return result;
	}
if (stored_documents != expected_documents
	|| stored_documents < 0 || stored_documents > (1LL << 40)
	|| stored_field_count != schema->count())
	{
	fclose(fp);
	return result;
	}

result->documents = stored_documents;
result->schema = *schema;

long long presence_bytes = (stored_documents + 7) / 8;

for (f = 0; f < stored_field_count && !failed; f++)
	{
	int type = schema->type(f);
	int multi = schema->is_multi(f);

	remaining = file_size - ftell(fp);
	if (remaining < presence_bytes)
		{ failed = 1; break; }
	result->presence[f] = new unsigned char[(size_t)(presence_bytes > 0 ? presence_bytes : 1)];
	if (fread(result->presence[f], 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
		{ failed = 1; break; }

	if (type == ANT_attribute_schema::TYPE_INT64 && !multi)
		{
		remaining = file_size - ftell(fp);
		if (remaining < stored_documents * (long long)sizeof(long long))
			{ failed = 1; break; }
		result->int_single[f] = new long long[(size_t)(stored_documents > 0 ? stored_documents : 1)];
		if (fread(result->int_single[f], sizeof(long long), (size_t)stored_documents, fp) != (size_t)stored_documents)
			{ failed = 1; break; }
		}
	else if (type == ANT_attribute_schema::TYPE_INT64 && multi)
		{
		remaining = file_size - ftell(fp);
		if (remaining < stored_documents * (long long)sizeof(int32_t))
			{ failed = 1; break; }
		result->int_counts[f] = new int32_t[(size_t)(stored_documents > 0 ? stored_documents : 1)];
		if (fread(result->int_counts[f], sizeof(int32_t), (size_t)stored_documents, fp) != (size_t)stored_documents)
			{ failed = 1; break; }

		remaining = file_size - ftell(fp);
		if (remaining < (stored_documents + 1) * (long long)sizeof(long long))
			{ failed = 1; break; }
		result->int_offsets[f] = new long long[(size_t)(stored_documents + 1)];
		if (fread(result->int_offsets[f], sizeof(long long), (size_t)(stored_documents + 1), fp) != (size_t)(stored_documents + 1))
			{ failed = 1; break; }

		if (result->int_offsets[f][0] != 0)
			{ failed = 1; break; }
		long long d;
		for (d = 0; d < stored_documents; d++)
			if (result->int_counts[f][d] < 0
				|| result->int_offsets[f][d + 1] < result->int_offsets[f][d]
				|| result->int_offsets[f][d + 1] - result->int_offsets[f][d] != result->int_counts[f][d])
				{ failed = 1; break; }
		if (failed) break;

		long long pool_count = result->int_offsets[f][stored_documents];
		if (pool_count < 0 || pool_count > file_size)
			{ failed = 1; break; }
		remaining = file_size - ftell(fp);
		if (remaining < pool_count * (long long)sizeof(long long))
			{ failed = 1; break; }
		result->int_pool[f] = new long long[(size_t)(pool_count > 0 ? pool_count : 1)];
		if (fread(result->int_pool[f], sizeof(long long), (size_t)pool_count, fp) != (size_t)pool_count)
			{ failed = 1; break; }
		}
	else if (type == ANT_attribute_schema::TYPE_BOOL)
		{
		remaining = file_size - ftell(fp);
		if (remaining < presence_bytes)
			{ failed = 1; break; }
		result->bool_bits[f] = new unsigned char[(size_t)(presence_bytes > 0 ? presence_bytes : 1)];
		if (fread(result->bool_bits[f], 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
			{ failed = 1; break; }
		}
	else if (type == ANT_attribute_schema::TYPE_STRING && !multi)
		{
		remaining = file_size - ftell(fp);
		if (remaining < stored_documents * (long long)sizeof(int32_t))
			{ failed = 1; break; }
		result->str_single[f] = new int32_t[(size_t)(stored_documents > 0 ? stored_documents : 1)];
		if (fread(result->str_single[f], sizeof(int32_t), (size_t)stored_documents, fp) != (size_t)stored_documents)
			{ failed = 1; break; }
		}
	else if (type == ANT_attribute_schema::TYPE_STRING && multi)
		{
		remaining = file_size - ftell(fp);
		if (remaining < stored_documents * (long long)sizeof(int32_t))
			{ failed = 1; break; }
		result->str_counts[f] = new int32_t[(size_t)(stored_documents > 0 ? stored_documents : 1)];
		if (fread(result->str_counts[f], sizeof(int32_t), (size_t)stored_documents, fp) != (size_t)stored_documents)
			{ failed = 1; break; }

		remaining = file_size - ftell(fp);
		if (remaining < (stored_documents + 1) * (long long)sizeof(long long))
			{ failed = 1; break; }
		result->str_offsets[f] = new long long[(size_t)(stored_documents + 1)];
		if (fread(result->str_offsets[f], sizeof(long long), (size_t)(stored_documents + 1), fp) != (size_t)(stored_documents + 1))
			{ failed = 1; break; }

		if (result->str_offsets[f][0] != 0)
			{ failed = 1; break; }
		long long d;
		for (d = 0; d < stored_documents; d++)
			if (result->str_counts[f][d] < 0
				|| result->str_offsets[f][d + 1] < result->str_offsets[f][d]
				|| result->str_offsets[f][d + 1] - result->str_offsets[f][d] != result->str_counts[f][d])
				{ failed = 1; break; }
		if (failed) break;

		long long pool_count = result->str_offsets[f][stored_documents];
		if (pool_count < 0 || pool_count > file_size)
			{ failed = 1; break; }
		remaining = file_size - ftell(fp);
		if (remaining < pool_count * (long long)sizeof(int32_t))
			{ failed = 1; break; }
		result->str_pool[f] = new int32_t[(size_t)(pool_count > 0 ? pool_count : 1)];
		if (fread(result->str_pool[f], sizeof(int32_t), (size_t)pool_count, fp) != (size_t)pool_count)
			{ failed = 1; break; }
		}

	if (type == ANT_attribute_schema::TYPE_STRING)
		{
		long long dict_n;

		remaining = file_size - ftell(fp);
		if (remaining < (long long)sizeof(dict_n))
			{ failed = 1; break; }
		if (fread(&dict_n, sizeof(dict_n), 1, fp) != 1)
			{ failed = 1; break; }
		if (dict_n < 0 || dict_n > file_size)
			{ failed = 1; break; }
		remaining = file_size - ftell(fp);
		if (remaining < dict_n * (long long)sizeof(int32_t))		// each entry is at least a 4-byte length
			{ failed = 1; break; }

		result->dict_offset[f] = new long long[(size_t)(dict_n + 1)];
		result->dict_offset[f][0] = 0;

		long long pool_cap = 1024, pool_size = 0;
		char *pool = new char[(size_t)pool_cap];
		int dict_failed = 0;
		long long i;

		for (i = 0; i < dict_n; i++)
			{
			int32_t len;

			remaining = file_size - ftell(fp);
			if (remaining < (long long)sizeof(len))
				{ dict_failed = 1; break; }
			if (fread(&len, sizeof(len), 1, fp) != 1)
				{ dict_failed = 1; break; }
			remaining = file_size - ftell(fp);
			if (len < 0 || (long long)len > remaining)
				{ dict_failed = 1; break; }

			if (pool_size + len > pool_cap)
				{
				long long new_cap = pool_cap;
				while (new_cap < pool_size + len)
					new_cap *= 2;
				char *new_pool = new char[(size_t)new_cap];
				memcpy(new_pool, pool, (size_t)pool_size);
				delete [] pool;
				pool = new_pool;
				pool_cap = new_cap;
				}
			if (len > 0 && fread(pool + pool_size, 1, (size_t)len, fp) != (size_t)len)
				{ dict_failed = 1; break; }
			pool_size += len;
			result->dict_offset[f][i + 1] = pool_size;
			}

		if (dict_failed)
			{
			delete [] pool;
			failed = 1;
			break;
			}

		result->dict_pool[f] = pool;
		result->dict_count[f] = dict_n;

		/*
			A tampered-but-size-consistent file could still reference an
			out-of-range dictionary id from the column just read; cross-check
			now so no matcher can ever dereference past the dictionary.  Absent
			single-valued slots are excluded (they may legitimately hold
			leftover 0s per the layout comment) since they are never read by
			any accessor; multi-valued pool entries always correspond to real
			appended values, so all of them are checked.
		*/
		if (!multi && result->str_single[f] != NULL)
			{
			long long d;
			for (d = 0; d < stored_documents; d++)
				{
				int has = (result->presence[f][d / 8] & (1 << (d % 8))) != 0;
				if (has && (result->str_single[f][d] < 0 || result->str_single[f][d] >= dict_n))
					{ failed = 1; break; }
				}
			}
		else if (multi && result->str_pool[f] != NULL)
			{
			long long p, pool_n = result->str_offsets[f][stored_documents];
			for (p = 0; p < pool_n; p++)
				if (result->str_pool[f][p] < 0 || result->str_pool[f][p] >= dict_n)
					{ failed = 1; break; }
			}
		if (failed) break;
		}
	}

fclose(fp);
if (failed)
	{
	delete result;
	return new ANT_attribute_store();
	}
return result;
}

/*
	ANT_ATTRIBUTE_STORE::HAS_FIELD()
	------------------------------------
*/
long ANT_attribute_store::has_field(long field, long long docid)
{
if (field < 0 || field >= schema.count() || docid < 0 || docid >= documents || presence[field] == NULL)
	return 0;
return (presence[field][docid / 8] & (1 << (docid % 8))) != 0 ? 1 : 0;
}

/*
	ANT_ATTRIBUTE_STORE::GET_INT()
	----------------------------------
*/
long ANT_attribute_store::get_int(long field, long long docid, long long *out)
{
if (!has_field(field, docid))
	return 0;
if (int_single[field] != NULL)
	{
	*out = int_single[field][docid];
	return 1;
	}
if (int_pool[field] != NULL && int_offsets[field] != NULL)
	{
	long long start = int_offsets[field][docid];
	long long end = int_offsets[field][docid + 1];
	if (end > start)
		{
		*out = int_pool[field][start];
		return 1;
		}
	}
return 0;
}

/*
	ANT_ATTRIBUTE_STORE::GET_BOOL()
	------------------------------------
*/
long ANT_attribute_store::get_bool(long field, long long docid, int *out)
{
if (!has_field(field, docid) || bool_bits[field] == NULL)
	return 0;
*out = (bool_bits[field][docid / 8] & (1 << (docid % 8))) != 0 ? 1 : 0;
return 1;
}

/*
	ANT_ATTRIBUTE_STORE::STRING_ID()
	------------------------------------
*/
long ANT_attribute_store::string_id(long field, const char *literal)
{
if (field < 0 || field >= schema.count() || literal == NULL || dict_pool[field] == NULL)
	return -1;

long long len = (long long)strlen(literal);
long long i;
for (i = 0; i < dict_count[field]; i++)
	{
	long long entry_len = dict_offset[field][i + 1] - dict_offset[field][i];
	if (entry_len == len && memcmp(dict_pool[field] + dict_offset[field][i], literal, (size_t)len) == 0)
		return (long)i;
	}
return -1;
}

/*
	ANT_ATTRIBUTE_STORE::STRING_MATCHES()
	------------------------------------------
*/
long ANT_attribute_store::string_matches(long field, long long docid, long want_id)
{
if (want_id < 0 || !has_field(field, docid))
	return 0;
if (str_single[field] != NULL)
	return str_single[field][docid] == want_id ? 1 : 0;
if (str_counts[field] != NULL && str_offsets[field] != NULL && str_pool[field] != NULL)
	{
	long long start = str_offsets[field][docid];
	long long end = str_offsets[field][docid + 1];
	long long i;
	for (i = start; i < end; i++)
		if (str_pool[field][i] == want_id)
			return 1;
	}
return 0;
}

/*
	ANT_ATTRIBUTE_STORE::INT_EQUALS()
	--------------------------------------
*/
long ANT_attribute_store::int_equals(long field, long long docid, long long want)
{
if (!has_field(field, docid))
	return 0;
if (int_single[field] != NULL)
	return int_single[field][docid] == want ? 1 : 0;
if (int_counts[field] != NULL && int_offsets[field] != NULL && int_pool[field] != NULL)
	{
	long long start = int_offsets[field][docid];
	long long end = int_offsets[field][docid + 1];
	long long i;
	for (i = start; i < end; i++)
		if (int_pool[field][i] == want)
			return 1;
	}
return 0;
}

/*
	ANT_ATTRIBUTE_STORE::INT_MATCHES_RANGE()
	----------------------------------------------
*/
long ANT_attribute_store::int_matches_range(long field, long long docid, long long lo, long long hi, int lo_incl, int hi_incl)
{
if (!has_field(field, docid))
	return 0;

if (int_single[field] != NULL)
	{
	long long v = int_single[field][docid];
	int lo_ok = lo_incl ? v >= lo : v > lo;
	int hi_ok = hi_incl ? v <= hi : v < hi;
	return (lo_ok && hi_ok) ? 1 : 0;
	}
if (int_counts[field] != NULL && int_offsets[field] != NULL && int_pool[field] != NULL)
	{
	long long start = int_offsets[field][docid];
	long long end = int_offsets[field][docid + 1];
	long long i;
	for (i = start; i < end; i++)
		{
		long long v = int_pool[field][i];
		int lo_ok = lo_incl ? v >= lo : v > lo;
		int hi_ok = hi_incl ? v <= hi : v < hi;
		if (lo_ok && hi_ok)
			return 1;
		}
	}
return 0;
}

/*
	ANT_ATTRIBUTE_STORE::BOOL_EQUALS()
	----------------------------------------
*/
long ANT_attribute_store::bool_equals(long field, long long docid, int want)
{
int v;
if (!get_bool(field, docid, &v))
	return 0;
return v == want ? 1 : 0;
}

/*
	ANT_ATTRIBUTE_STORE::VALUE_COUNT()
	----------------------------------------
*/
long long ANT_attribute_store::value_count(long field, long long docid)
{
if (!has_field(field, docid))
	return 0;
if (int_counts[field] != NULL)
	return int_counts[field][docid];
if (str_counts[field] != NULL)
	return str_counts[field][docid];
return 1;		// single-valued present field (int single, string single, bool)
}

/*
	ANT_ATTRIBUTE_STORE::GET_INT_AT()
	----------------------------------------
*/
long ANT_attribute_store::get_int_at(long field, long long docid, long long idx, long long *out)
{
if (!has_field(field, docid) || idx < 0)
	return 0;
if (int_single[field] != NULL)
	{
	if (idx != 0)
		return 0;
	*out = int_single[field][docid];
	return 1;
	}
if (int_counts[field] != NULL && int_offsets[field] != NULL && int_pool[field] != NULL)
	{
	long long start = int_offsets[field][docid];
	long long count = int_counts[field][docid];
	if (idx >= count)
		return 0;
	*out = int_pool[field][start + idx];
	return 1;
	}
return 0;
}

/*
	ANT_ATTRIBUTE_STORE::GET_STRING_AT()
	------------------------------------------
*/
long ANT_attribute_store::get_string_at(long field, long long docid, long long idx, char *out, long long out_size)
{
long dict_id = -1;

if (!has_field(field, docid) || idx < 0 || out == NULL || out_size <= 0)
	return 0;

if (str_single[field] != NULL)
	{
	if (idx != 0)
		return 0;
	dict_id = str_single[field][docid];
	}
else if (str_counts[field] != NULL && str_offsets[field] != NULL && str_pool[field] != NULL)
	{
	long long start = str_offsets[field][docid];
	long long count = str_counts[field][docid];
	if (idx >= count)
		return 0;
	dict_id = str_pool[field][start + idx];
	}
else
	return 0;

if (dict_id < 0 || dict_id >= dict_count[field] || dict_pool[field] == NULL)
	return 0;

long long entry_start = dict_offset[field][dict_id];
long long entry_len = dict_offset[field][dict_id + 1] - entry_start;
long long copy_len = entry_len < out_size - 1 ? entry_len : out_size - 1;
if (copy_len < 0)
	copy_len = 0;
memcpy(out, dict_pool[field] + entry_start, (size_t)copy_len);
out[copy_len] = '\0';
return 1;
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::ANT_ATTRIBUTE_STORE_WRITER()
	-------------------------------------------------------------
*/
ANT_attribute_store_writer::ANT_attribute_store_writer()
{
long f;

schema = NULL;
filename[0] = '\0';
documents = 0;
capacity = 0;
for (f = 0; f < ANT_attribute_schema::MAX_FIELDS; f++)
	{
	presence[f] = NULL;
	int_single[f] = NULL;
	int_counts[f] = NULL;
	int_pool[f] = NULL;
	int_pool_size[f] = 0;
	int_pool_capacity[f] = 0;
	bool_bits[f] = NULL;
	str_single[f] = NULL;
	str_counts[f] = NULL;
	str_pool[f] = NULL;
	str_pool_size[f] = 0;
	str_pool_capacity[f] = 0;
	dict_pool[f] = NULL;
	dict_pool_size[f] = 0;
	dict_pool_capacity[f] = 0;
	dict_offset[f] = NULL;
	dict_count[f] = 0;
	dict_offset_capacity[f] = 0;
	}
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::~ANT_ATTRIBUTE_STORE_WRITER()
	----------------------------------------------------------------
*/
ANT_attribute_store_writer::~ANT_attribute_store_writer()
{
abandon();
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::CREATE()
	------------------------------------------
	Resets any prior state, so a writer may be reused across create() calls.
*/
long ANT_attribute_store_writer::create(const char *path, const ANT_attribute_schema *schema_ptr)
{
abandon();

if (schema_ptr == NULL)
	return 1;
if (snprintf(filename, sizeof(filename), "%s", path) >= (int)sizeof(filename))
	return 1;

schema = schema_ptr;
documents = 0;
capacity = 1024;

long f;
for (f = 0; f < schema->count(); f++)
	{
	presence[f] = new unsigned char[(size_t)((capacity + 7) / 8)];
	memset(presence[f], 0, (size_t)((capacity + 7) / 8));

	int type = schema->type(f);
	int multi = schema->is_multi(f);

	if (type == ANT_attribute_schema::TYPE_INT64 && !multi)
		{
		int_single[f] = new long long[(size_t)capacity];
		memset(int_single[f], 0, (size_t)(capacity * sizeof(long long)));
		}
	else if (type == ANT_attribute_schema::TYPE_INT64 && multi)
		{
		int_counts[f] = new int32_t[(size_t)capacity];
		memset(int_counts[f], 0, (size_t)(capacity * sizeof(int32_t)));
		int_pool_capacity[f] = 1024;
		int_pool[f] = new long long[(size_t)int_pool_capacity[f]];
		int_pool_size[f] = 0;
		}
	else if (type == ANT_attribute_schema::TYPE_BOOL)
		{
		bool_bits[f] = new unsigned char[(size_t)((capacity + 7) / 8)];
		memset(bool_bits[f], 0, (size_t)((capacity + 7) / 8));
		}
	else if (type == ANT_attribute_schema::TYPE_STRING && !multi)
		{
		str_single[f] = new int32_t[(size_t)capacity];
		memset(str_single[f], 0, (size_t)(capacity * sizeof(int32_t)));
		}
	else if (type == ANT_attribute_schema::TYPE_STRING && multi)
		{
		str_counts[f] = new int32_t[(size_t)capacity];
		memset(str_counts[f], 0, (size_t)(capacity * sizeof(int32_t)));
		str_pool_capacity[f] = 1024;
		str_pool[f] = new int32_t[(size_t)str_pool_capacity[f]];
		str_pool_size[f] = 0;
		}

	if (type == ANT_attribute_schema::TYPE_STRING)
		{
		dict_pool_capacity[f] = 1024;
		dict_pool[f] = new char[(size_t)dict_pool_capacity[f]];
		dict_pool_size[f] = 0;
		dict_offset_capacity[f] = 256;
		dict_offset[f] = new long long[(size_t)dict_offset_capacity[f]];
		dict_offset[f][0] = 0;
		dict_count[f] = 0;
		}
	}
return 0;
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::GROW_DOCS()
	---------------------------------------------
	Doubles the per-document capacity and reallocates every field's
	document-indexed buffer that is in use (presence, single-valued
	columns, and multi-valued counts arrays).  The value pools (int/string)
	and the string dictionaries grow independently, on demand, in
	add_int()/intern().
*/
long ANT_attribute_store_writer::grow_docs(void)
{
long long new_capacity = capacity * 2;
long f;

for (f = 0; f < schema->count(); f++)
	{
	unsigned char *new_presence = new unsigned char[(size_t)((new_capacity + 7) / 8)];
	memset(new_presence, 0, (size_t)((new_capacity + 7) / 8));
	memcpy(new_presence, presence[f], (size_t)((capacity + 7) / 8));
	delete [] presence[f];
	presence[f] = new_presence;

	if (int_single[f] != NULL)
		{
		long long *n = new long long[(size_t)new_capacity];
		memset(n, 0, (size_t)(new_capacity * sizeof(long long)));
		memcpy(n, int_single[f], (size_t)(capacity * sizeof(long long)));
		delete [] int_single[f];
		int_single[f] = n;
		}
	if (int_counts[f] != NULL)
		{
		int32_t *n = new int32_t[(size_t)new_capacity];
		memset(n, 0, (size_t)(new_capacity * sizeof(int32_t)));
		memcpy(n, int_counts[f], (size_t)(capacity * sizeof(int32_t)));
		delete [] int_counts[f];
		int_counts[f] = n;
		}
	if (bool_bits[f] != NULL)
		{
		unsigned char *n = new unsigned char[(size_t)((new_capacity + 7) / 8)];
		memset(n, 0, (size_t)((new_capacity + 7) / 8));
		memcpy(n, bool_bits[f], (size_t)((capacity + 7) / 8));
		delete [] bool_bits[f];
		bool_bits[f] = n;
		}
	if (str_single[f] != NULL)
		{
		int32_t *n = new int32_t[(size_t)new_capacity];
		memset(n, 0, (size_t)(new_capacity * sizeof(int32_t)));
		memcpy(n, str_single[f], (size_t)(capacity * sizeof(int32_t)));
		delete [] str_single[f];
		str_single[f] = n;
		}
	if (str_counts[f] != NULL)
		{
		int32_t *n = new int32_t[(size_t)new_capacity];
		memset(n, 0, (size_t)(new_capacity * sizeof(int32_t)));
		memcpy(n, str_counts[f], (size_t)(capacity * sizeof(int32_t)));
		delete [] str_counts[f];
		str_counts[f] = n;
		}
	}
capacity = new_capacity;
return 0;
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::INTERN()
	--------------------------------------------
	Returns the existing dictionary id for 'literal' in this field, or
	appends it (first-seen id == append order).
*/
long ANT_attribute_store_writer::intern(long field, const char *literal)
{
long long len = (long long)strlen(literal);
long long i;

for (i = 0; i < dict_count[field]; i++)
	{
	long long entry_len = dict_offset[field][i + 1] - dict_offset[field][i];
	if (entry_len == len && memcmp(dict_pool[field] + dict_offset[field][i], literal, (size_t)len) == 0)
		return (long)i;
	}

if (dict_pool_size[field] + len > dict_pool_capacity[field])
	{
	long long new_cap = dict_pool_capacity[field];
	while (new_cap < dict_pool_size[field] + len)
		new_cap *= 2;
	char *n = new char[(size_t)new_cap];
	memcpy(n, dict_pool[field], (size_t)dict_pool_size[field]);
	delete [] dict_pool[field];
	dict_pool[field] = n;
	dict_pool_capacity[field] = new_cap;
	}
memcpy(dict_pool[field] + dict_pool_size[field], literal, (size_t)len);
dict_pool_size[field] += len;

if (dict_count[field] + 1 >= dict_offset_capacity[field])
	{
	long long new_cap = dict_offset_capacity[field] * 2;
	long long *n = new long long[(size_t)new_cap];
	memcpy(n, dict_offset[field], (size_t)((dict_count[field] + 1) * sizeof(long long)));
	delete [] dict_offset[field];
	dict_offset[field] = n;
	dict_offset_capacity[field] = new_cap;
	}
dict_offset[field][dict_count[field] + 1] = dict_pool_size[field];
dict_count[field]++;
return (long)(dict_count[field] - 1);
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::BEGIN_DOCUMENT()
	----------------------------------------------------
*/
void ANT_attribute_store_writer::begin_document(void)
{
if (documents >= capacity)
	grow_docs();
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::SET_INT()
	--------------------------------------------
*/
void ANT_attribute_store_writer::set_int(long field, long long value)
{
if (field < 0 || field >= schema->count() || int_single[field] == NULL)
	return;
int_single[field][documents] = value;
presence[field][documents / 8] |= (unsigned char)(1 << (documents % 8));
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::ADD_INT()
	--------------------------------------------
*/
void ANT_attribute_store_writer::add_int(long field, long long value)
{
if (field < 0 || field >= schema->count() || int_counts[field] == NULL)
	return;
if (int_pool_size[field] >= int_pool_capacity[field])
	{
	long long new_cap = int_pool_capacity[field] * 2;
	long long *n = new long long[(size_t)new_cap];
	memcpy(n, int_pool[field], (size_t)(int_pool_size[field] * sizeof(long long)));
	delete [] int_pool[field];
	int_pool[field] = n;
	int_pool_capacity[field] = new_cap;
	}
int_pool[field][int_pool_size[field]++] = value;
int_counts[field][documents]++;
presence[field][documents / 8] |= (unsigned char)(1 << (documents % 8));
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::SET_STRING()
	--------------------------------------------
*/
void ANT_attribute_store_writer::set_string(long field, const char *value)
{
if (field < 0 || field >= schema->count() || str_single[field] == NULL || value == NULL)
	return;
long id = intern(field, value);
str_single[field][documents] = (int32_t)id;
presence[field][documents / 8] |= (unsigned char)(1 << (documents % 8));
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::ADD_STRING()
	--------------------------------------------
*/
void ANT_attribute_store_writer::add_string(long field, const char *value)
{
if (field < 0 || field >= schema->count() || str_counts[field] == NULL || value == NULL)
	return;
long id = intern(field, value);
if (str_pool_size[field] >= str_pool_capacity[field])
	{
	long long new_cap = str_pool_capacity[field] * 2;
	int32_t *n = new int32_t[(size_t)new_cap];
	memcpy(n, str_pool[field], (size_t)(str_pool_size[field] * sizeof(int32_t)));
	delete [] str_pool[field];
	str_pool[field] = n;
	str_pool_capacity[field] = new_cap;
	}
str_pool[field][str_pool_size[field]++] = (int32_t)id;
str_counts[field][documents]++;
presence[field][documents / 8] |= (unsigned char)(1 << (documents % 8));
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::SET_BOOL()
	----------------------------------------------
*/
void ANT_attribute_store_writer::set_bool(long field, int value)
{
if (field < 0 || field >= schema->count() || bool_bits[field] == NULL)
	return;
if (value)
	bool_bits[field][documents / 8] |= (unsigned char)(1 << (documents % 8));
else
	bool_bits[field][documents / 8] &= (unsigned char)~(1 << (documents % 8));
presence[field][documents / 8] |= (unsigned char)(1 << (documents % 8));
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::END_DOCUMENT()
	----------------------------------------------------
*/
void ANT_attribute_store_writer::end_document(void)
{
documents++;
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::FINISH()
	--------------------------------------------
	Write-temp then rename, per the crash-safety convention.  On any fwrite
	error the temp file is removed and 1 is returned; the writer's own
	buffers are left intact (only abandon()/the destructor free them).
*/
long ANT_attribute_store_writer::finish(void)
{
char temp_name[4200];
FILE *fp;
long f;
long long presence_bytes = (documents + 7) / 8;
long long field_count;
uint32_t version = 1;
long failed = 0;

if (schema == NULL)
	return 1;
field_count = schema->count();

if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;

if (fwrite(ANT_ATTRIBUTE_STORE_MAGIC, 1, 8, fp) != 8
	|| fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&documents, sizeof(documents), 1, fp) != 1
	|| fwrite(&field_count, sizeof(field_count), 1, fp) != 1)
	failed = 1;

for (f = 0; f < field_count && !failed; f++)
	{
	int type = schema->type(f);
	int multi = schema->is_multi(f);

	if (fwrite(presence[f], 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
		{ failed = 1; break; }

	if (type == ANT_attribute_schema::TYPE_INT64 && !multi)
		{
		if (fwrite(int_single[f], sizeof(long long), (size_t)documents, fp) != (size_t)documents)
			{ failed = 1; break; }
		}
	else if (type == ANT_attribute_schema::TYPE_INT64 && multi)
		{
		long long *offs = new long long[(size_t)(documents + 1)];
		long long d;
		offs[0] = 0;
		for (d = 0; d < documents; d++)
			offs[d + 1] = offs[d] + int_counts[f][d];
		int ok = fwrite(int_counts[f], sizeof(int32_t), (size_t)documents, fp) == (size_t)documents
			&& fwrite(offs, sizeof(long long), (size_t)(documents + 1), fp) == (size_t)(documents + 1)
			&& fwrite(int_pool[f], sizeof(long long), (size_t)int_pool_size[f], fp) == (size_t)int_pool_size[f];
		delete [] offs;
		if (!ok)
			{ failed = 1; break; }
		}
	else if (type == ANT_attribute_schema::TYPE_BOOL)
		{
		if (fwrite(bool_bits[f], 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes)
			{ failed = 1; break; }
		}
	else if (type == ANT_attribute_schema::TYPE_STRING && !multi)
		{
		if (fwrite(str_single[f], sizeof(int32_t), (size_t)documents, fp) != (size_t)documents)
			{ failed = 1; break; }
		}
	else if (type == ANT_attribute_schema::TYPE_STRING && multi)
		{
		long long *offs = new long long[(size_t)(documents + 1)];
		long long d;
		offs[0] = 0;
		for (d = 0; d < documents; d++)
			offs[d + 1] = offs[d] + str_counts[f][d];
		int ok = fwrite(str_counts[f], sizeof(int32_t), (size_t)documents, fp) == (size_t)documents
			&& fwrite(offs, sizeof(long long), (size_t)(documents + 1), fp) == (size_t)(documents + 1)
			&& fwrite(str_pool[f], sizeof(int32_t), (size_t)str_pool_size[f], fp) == (size_t)str_pool_size[f];
		delete [] offs;
		if (!ok)
			{ failed = 1; break; }
		}

	if (type == ANT_attribute_schema::TYPE_STRING)
		{
		long long dc = dict_count[f];
		if (fwrite(&dc, sizeof(dc), 1, fp) != 1)
			{ failed = 1; break; }
		long long i;
		for (i = 0; i < dc && !failed; i++)
			{
			int32_t len = (int32_t)(dict_offset[f][i + 1] - dict_offset[f][i]);
			if (fwrite(&len, sizeof(len), 1, fp) != 1
				|| fwrite(dict_pool[f] + dict_offset[f][i], 1, (size_t)len, fp) != (size_t)len)
				failed = 1;
			}
		}
	}

fclose(fp);
if (failed)
	{
	remove(temp_name);
	return 1;
	}
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}

/*
	ANT_ATTRIBUTE_STORE_WRITER::ABANDON()
	--------------------------------------------
*/
void ANT_attribute_store_writer::abandon(void)
{
long f;

if (schema != NULL)
	for (f = 0; f < schema->count(); f++)
		{
		delete [] presence[f]; presence[f] = NULL;
		delete [] int_single[f]; int_single[f] = NULL;
		delete [] int_counts[f]; int_counts[f] = NULL;
		delete [] int_pool[f]; int_pool[f] = NULL;
		delete [] bool_bits[f]; bool_bits[f] = NULL;
		delete [] str_single[f]; str_single[f] = NULL;
		delete [] str_counts[f]; str_counts[f] = NULL;
		delete [] str_pool[f]; str_pool[f] = NULL;
		delete [] dict_pool[f]; dict_pool[f] = NULL;
		delete [] dict_offset[f]; dict_offset[f] = NULL;
		}

schema = NULL;
documents = 0;
capacity = 0;
for (f = 0; f < ANT_attribute_schema::MAX_FIELDS; f++)
	{
	int_pool_size[f] = 0;
	int_pool_capacity[f] = 0;
	str_pool_size[f] = 0;
	str_pool_capacity[f] = 0;
	dict_pool_size[f] = 0;
	dict_pool_capacity[f] = 0;
	dict_offset_capacity[f] = 0;
	dict_count[f] = 0;
	}
}

/*
	ANT_ATTRIBUTE_SET::ANT_ATTRIBUTE_SET()
	----------------------------------------
	Zero-init every per-field array; no values held and no payload.
*/
ANT_attribute_set::ANT_attribute_set(const ANT_attribute_schema *schema)
{
long f;

this->schema = schema;
for (f = 0; f < ANT_attribute_schema::MAX_FIELDS; f++)
	{
	present_flag[f] = 0;
	int_vals[f] = NULL;
	int_count[f] = 0;
	int_cap[f] = 0;
	str_vals[f] = NULL;
	str_count[f] = 0;
	str_cap[f] = 0;
	bool_vals[f] = 0;
	}
payload = NULL;
payload_len = 0;
}

/*
	ANT_ATTRIBUTE_SET::~ANT_ATTRIBUTE_SET()
	-----------------------------------------
*/
ANT_attribute_set::~ANT_attribute_set()
{
long f, i;

for (f = 0; f < ANT_attribute_schema::MAX_FIELDS; f++)
	{
	delete [] int_vals[f];
	if (str_vals[f] != NULL)
		{
		for (i = 0; i < str_count[f]; i++)
			free(str_vals[f][i]);
		delete [] str_vals[f];
		}
	}
free(payload);
}

/*
	ANT_ATTRIBUTE_SET::SET_INT()
	------------------------------
	Single-valued: clear then append exactly this value.
*/
void ANT_attribute_set::set_int(long field, long long value)
{
if (schema == NULL || field < 0 || field >= schema->count())
	return;
present_flag[field] = 1;
int_count[field] = 0;
add_int(field, value);
}

/*
	ANT_ATTRIBUTE_SET::ADD_INT()
	------------------------------
	Multi-valued: mark present and append (grow geometrically).
*/
void ANT_attribute_set::add_int(long field, long long value)
{
if (schema == NULL || field < 0 || field >= schema->count())
	return;
present_flag[field] = 1;
if (int_count[field] >= int_cap[field])
	{
	long new_cap = int_cap[field] == 0 ? 4 : int_cap[field] * 2;
	long long *grown = new long long[new_cap];
	if (int_vals[field] != NULL)
		memcpy(grown, int_vals[field], (size_t)(int_count[field] * sizeof(long long)));
	delete [] int_vals[field];
	int_vals[field] = grown;
	int_cap[field] = new_cap;
	}
int_vals[field][int_count[field]++] = value;
}

/*
	ANT_ATTRIBUTE_SET::SET_STRING()
	---------------------------------
	Single-valued: clear then append exactly this value.
*/
void ANT_attribute_set::set_string(long field, const char *value)
{
long i;

if (schema == NULL || field < 0 || field >= schema->count())
	return;
present_flag[field] = 1;
if (str_vals[field] != NULL)
	for (i = 0; i < str_count[field]; i++)
		{
		free(str_vals[field][i]);
		str_vals[field][i] = NULL;
		}
str_count[field] = 0;
add_string(field, value);
}

/*
	ANT_ATTRIBUTE_SET::ADD_STRING()
	---------------------------------
	Multi-valued: mark present and append an owned copy (grow geometrically).
*/
void ANT_attribute_set::add_string(long field, const char *value)
{
char *copy;

if (schema == NULL || field < 0 || field >= schema->count() || value == NULL)
	return;
present_flag[field] = 1;
if (str_count[field] >= str_cap[field])
	{
	long new_cap = str_cap[field] == 0 ? 4 : str_cap[field] * 2;
	char **grown = new char *[new_cap];
	if (str_vals[field] != NULL)
		memcpy(grown, str_vals[field], (size_t)(str_count[field] * sizeof(char *)));
	delete [] str_vals[field];
	str_vals[field] = grown;
	str_cap[field] = new_cap;
	}
copy = (char *)malloc(strlen(value) + 1);
strcpy(copy, value);
str_vals[field][str_count[field]++] = copy;
}

/*
	ANT_ATTRIBUTE_SET::SET_BOOL()
	-------------------------------
*/
void ANT_attribute_set::set_bool(long field, int value)
{
if (schema == NULL || field < 0 || field >= schema->count())
	return;
present_flag[field] = 1;
bool_vals[field] = value ? 1 : 0;
}

/*
	ANT_ATTRIBUTE_SET::SET_PAYLOAD()
	----------------------------------
	Replace the opaque payload with a copy of len bytes; len 0 / ptr NULL clears.
*/
void ANT_attribute_set::set_payload(const void *ptr, long long len)
{
free(payload);
payload = NULL;
payload_len = 0;
if (ptr == NULL || len <= 0)
	return;
payload = (unsigned char *)malloc((size_t)len);
memcpy(payload, ptr, (size_t)len);
payload_len = len;
}

/*
	ANT_ATTRIBUTE_SET accessors
	-----------------------------
*/
int ANT_attribute_set::has(long field) const
{
if (field < 0 || field >= ANT_attribute_schema::MAX_FIELDS)
	return 0;
return present_flag[field] ? 1 : 0;
}

long ANT_attribute_set::present_field_count(void) const
{
long f, n = 0;

for (f = 0; f < ANT_attribute_schema::MAX_FIELDS; f++)
	if (present_flag[f])
		n++;
return n;
}

long ANT_attribute_set::ints(long field) const
{
if (field < 0 || field >= ANT_attribute_schema::MAX_FIELDS)
	return 0;
return int_count[field];
}

long long ANT_attribute_set::int_get(long field, long i) const
{
if (field < 0 || field >= ANT_attribute_schema::MAX_FIELDS || i < 0 || i >= int_count[field])
	return 0;
return int_vals[field][i];
}

long ANT_attribute_set::strings(long field) const
{
if (field < 0 || field >= ANT_attribute_schema::MAX_FIELDS)
	return 0;
return str_count[field];
}

const char *ANT_attribute_set::string_get(long field, long i) const
{
if (field < 0 || field >= ANT_attribute_schema::MAX_FIELDS || i < 0 || i >= str_count[field])
	return NULL;
return str_vals[field][i];
}

int ANT_attribute_set::boolean(long field) const
{
if (field < 0 || field >= ANT_attribute_schema::MAX_FIELDS)
	return 0;
return bool_vals[field] ? 1 : 0;
}

const unsigned char *ANT_attribute_set::payload_bytes(long long *len) const
{
if (len != NULL)
	*len = payload_len;
return payload;
}

/*
	ANT_ATTRIBUTE_SET::CLONE()
	----------------------------
	Deep heap copy sharing the same schema pointer.
*/
ANT_attribute_set *ANT_attribute_set::clone(void) const
{
ANT_attribute_set *copy = new ANT_attribute_set(schema);
long f, i;

for (f = 0; f < ANT_attribute_schema::MAX_FIELDS; f++)
	{
	copy->present_flag[f] = present_flag[f];
	copy->bool_vals[f] = bool_vals[f];
	if (int_count[f] > 0)
		{
		copy->int_vals[f] = new long long[int_count[f]];
		memcpy(copy->int_vals[f], int_vals[f], (size_t)(int_count[f] * sizeof(long long)));
		copy->int_count[f] = int_count[f];
		copy->int_cap[f] = int_count[f];
		}
	if (str_count[f] > 0)
		{
		copy->str_vals[f] = new char *[str_count[f]];
		for (i = 0; i < str_count[f]; i++)
			{
			copy->str_vals[f][i] = (char *)malloc(strlen(str_vals[f][i]) + 1);
			strcpy(copy->str_vals[f][i], str_vals[f][i]);
			}
		copy->str_count[f] = str_count[f];
		copy->str_cap[f] = str_count[f];
		}
	}
if (payload != NULL && payload_len > 0)
	{
	copy->payload = (unsigned char *)malloc((size_t)payload_len);
	memcpy(copy->payload, payload, (size_t)payload_len);
	copy->payload_len = payload_len;
	}
return copy;
}

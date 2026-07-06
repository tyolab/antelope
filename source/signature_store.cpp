/*
	SIGNATURE_STORE.CPP
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "signature_store.h"
#include "index_tombstones.h"

#define ANT_SIGNATURE_STORE_MAGIC 0x3130474953544E41ULL		/* "ANTSIG01" little-endian */
#define ANT_SIGNATURE_STORE_VERSION 1u

ANT_signature_store::ANT_signature_store()
{
bits = 0; documents = 0; presence = NULL; signatures = NULL;
}

ANT_signature_store::~ANT_signature_store()
{
delete [] presence;
delete [] signatures;
}

ANT_signature_store *ANT_signature_store::load(const char *filename, long long expected_bits, long long expected_documents)
{
FILE *fp;
unsigned long long magic;
unsigned int version;
long long stored_bits, stored_documents, sig_bytes, presence_bytes, file_size, expected_size;
ANT_signature_store *result = new ANT_signature_store();

if ((fp = fopen(filename, "rb")) == NULL)
	return result;

if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != ANT_SIGNATURE_STORE_MAGIC
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != ANT_SIGNATURE_STORE_VERSION
	|| fread(&stored_bits, sizeof(stored_bits), 1, fp) != 1
	|| fread(&stored_documents, sizeof(stored_documents), 1, fp) != 1
	|| stored_bits != expected_bits || stored_documents != expected_documents
	|| stored_bits < 8 || stored_bits > 65536 || (stored_bits % 8) != 0
	|| stored_documents < 0 || stored_documents > (1LL << 40))
	{
	fclose(fp);
	return result;
	}

sig_bytes = (stored_bits + 7) / 8;
presence_bytes = (stored_documents + 7) / 8;
expected_size = 28 + presence_bytes + stored_documents * sig_bytes;		// header is 8+4+8+8 = 28 bytes
if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) != expected_size || fseek(fp, 28, SEEK_SET) != 0)
	{
	fclose(fp);
	return result;
	}

unsigned char *presence_buffer = new unsigned char[presence_bytes > 0 ? presence_bytes : 1];
unsigned char *signature_buffer = new unsigned char[stored_documents * sig_bytes > 0 ? stored_documents * sig_bytes : 1];
if (fread(presence_buffer, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
	|| fread(signature_buffer, 1, (size_t)(stored_documents * sig_bytes), fp) != (size_t)(stored_documents * sig_bytes))
	{
	delete [] presence_buffer; delete [] signature_buffer; fclose(fp);
	return result;
	}
fclose(fp);

result->bits = stored_bits;
result->documents = stored_documents;
result->presence = presence_buffer;
result->signatures = signature_buffer;
return result;
}

void ANT_signature_store::shortlist(const unsigned char *query_signature, ANT_index_tombstones *tombstones, long long pool_size, long long *out_docids, long long *out_count)
{
long long docid, i, count = 0, worst_index, h, sb = signature_bytes();
long long *hammings;

if (presence == NULL || signatures == NULL || pool_size < 1)
	{ *out_count = 0; return; }
hammings = new long long[pool_size];

for (docid = 0; docid < documents; docid++)
	{
	if (!has(docid))
		continue;
	if (tombstones != NULL && tombstones->is_deleted(docid))
		continue;
	{
	const unsigned char *sig = get(docid);
	h = 0;
	for (i = 0; i < sb; i++)
		h += (long long)__builtin_popcount((unsigned int)(sig[i] ^ query_signature[i]));
	}
	if (count < pool_size)
		{ out_docids[count] = docid; hammings[count] = h; count++; }
	else
		{
		worst_index = 0;
		for (i = 1; i < pool_size; i++)
			if (hammings[i] > hammings[worst_index])
				worst_index = i;
		if (h < hammings[worst_index])
			{ out_docids[worst_index] = docid; hammings[worst_index] = h; }
		}
	}
*out_count = count;
delete [] hammings;
}

/* ---- writer ---- */

ANT_signature_store_writer::ANT_signature_store_writer()
{
filename[0] = '\0'; bits = 0; documents = 0; capacity = 0; presence = NULL; signatures = NULL;
}

ANT_signature_store_writer::~ANT_signature_store_writer()
{
delete [] presence;
delete [] signatures;
}

long ANT_signature_store_writer::create(const char *name, long long width)
{
if (width < 8 || width > 65536 || (width % 8) != 0)
	return 1;
delete [] presence;
delete [] signatures;
presence = NULL;
signatures = NULL;
strncpy(filename, name, sizeof(filename) - 1);
filename[sizeof(filename) - 1] = '\0';
bits = width;
documents = 0;
capacity = 1024;
presence = new unsigned char[(capacity + 7) / 8];
memset(presence, 0, (size_t)((capacity + 7) / 8));
signatures = new unsigned char[capacity * sig_bytes()];
return 0;
}

long ANT_signature_store_writer::grow(void)
{
long long new_capacity = capacity * 2, sb = sig_bytes();
unsigned char *new_presence = new unsigned char[(new_capacity + 7) / 8];
unsigned char *new_signatures = new unsigned char[new_capacity * sb];
memset(new_presence, 0, (size_t)((new_capacity + 7) / 8));
memcpy(new_presence, presence, (size_t)((capacity + 7) / 8));
memcpy(new_signatures, signatures, (size_t)(capacity * sb));
delete [] presence; delete [] signatures;
presence = new_presence; signatures = new_signatures; capacity = new_capacity;
return 0;
}

long ANT_signature_store_writer::append(const unsigned char *signature_or_null)
{
long long sb = sig_bytes();
if (signatures == NULL)
	return 1;
if (documents >= capacity)
	grow();
if (signature_or_null == NULL)
	memset(signatures + documents * sb, 0, (size_t)sb);
else
	{
	memcpy(signatures + documents * sb, signature_or_null, (size_t)sb);
	presence[documents / 8] |= (unsigned char)(1 << (documents % 8));
	}
documents++;
return 0;
}

long ANT_signature_store_writer::finish(void)
{
char temp_name[4200];
FILE *fp;
unsigned long long magic = ANT_SIGNATURE_STORE_MAGIC;
unsigned int version = ANT_SIGNATURE_STORE_VERSION;
long long sb = sig_bytes(), presence_bytes = (documents + 7) / 8;

if (signatures == NULL)
	return 1;
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&bits, sizeof(bits), 1, fp) != 1 || fwrite(&documents, sizeof(documents), 1, fp) != 1
	|| fwrite(presence, 1, (size_t)presence_bytes, fp) != (size_t)presence_bytes
	|| fwrite(signatures, 1, (size_t)(documents * sb), fp) != (size_t)(documents * sb))
	{ fclose(fp); remove(temp_name); return 1; }
fclose(fp);
if (rename(temp_name, filename) != 0)
	{ remove(temp_name); return 1; }
return 0;
}

void ANT_signature_store_writer::abandon(void)
{
delete [] presence; delete [] signatures;
presence = NULL; signatures = NULL; documents = 0; capacity = 0;
}

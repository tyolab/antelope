/*
	INDEX_TOMBSTONES.CPP
	--------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "index_tombstones.h"

/*
	ANT_INDEX_TOMBSTONES::ANT_INDEX_TOMBSTONES()
	--------------------------------------------
*/
ANT_index_tombstones::ANT_index_tombstones(long long documents)
{
bitmap_bytes = (documents / 8) + 1;
bitmap = new unsigned char[bitmap_bytes];
memset(bitmap, 0, (size_t)bitmap_bytes);
deleted_documents = 0;
}

/*
	ANT_INDEX_TOMBSTONES::~ANT_INDEX_TOMBSTONES()
	---------------------------------------------
*/
ANT_index_tombstones::~ANT_index_tombstones()
{
delete [] bitmap;
}

/*
	ANT_INDEX_TOMBSTONES::GROW_TO()
	-------------------------------
*/
void ANT_index_tombstones::grow_to(long long docid)
{
long long needed = (docid / 8) + 1;
if (needed <= bitmap_bytes)
	return;

long long new_bytes = bitmap_bytes * 2 > needed ? bitmap_bytes * 2 : needed;
unsigned char *new_bitmap = new unsigned char[new_bytes];
memcpy(new_bitmap, bitmap, (size_t)bitmap_bytes);
memset(new_bitmap + bitmap_bytes, 0, (size_t)(new_bytes - bitmap_bytes));
delete [] bitmap;
bitmap = new_bitmap;
bitmap_bytes = new_bytes;
}

/*
	ANT_INDEX_TOMBSTONES::SET_DELETED()
	-----------------------------------
*/
void ANT_index_tombstones::set_deleted(long long docid)
{
grow_to(docid);
if (!(bitmap[docid / 8] & (1 << (docid % 8))))
	{
	bitmap[docid / 8] |= (unsigned char)(1 << (docid % 8));
	deleted_documents++;
	}
}

/*
	ANT_INDEX_TOMBSTONES::IS_DELETED()
	----------------------------------
*/
long ANT_index_tombstones::is_deleted(long long docid)
{
if (docid / 8 >= bitmap_bytes)
	return 0;
return (bitmap[docid / 8] & (1 << (docid % 8))) != 0;
}

/*
	ANT_INDEX_TOMBSTONES::SAVE()
	----------------------------
	Write-temp then rename so a crash can never leave a torn bitmap.
*/
long ANT_index_tombstones::save(const char *filename)
{
char temp_name[4096];
FILE *fp;

if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
if (fwrite(&deleted_documents, sizeof(deleted_documents), 1, fp) != 1
	|| fwrite(&bitmap_bytes, sizeof(bitmap_bytes), 1, fp) != 1
	|| fwrite(bitmap, 1, (size_t)bitmap_bytes, fp) != (size_t)bitmap_bytes)
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
	ANT_INDEX_TOMBSTONES::LOAD()
	----------------------------
*/
ANT_index_tombstones *ANT_index_tombstones::load(const char *filename, long long documents)
{
FILE *fp;
ANT_index_tombstones *result = new ANT_index_tombstones(documents);

if ((fp = fopen(filename, "rb")) == NULL)
	return result;			// no .del file means no deletions

long long stored_count, stored_bytes;
if (fread(&stored_count, sizeof(stored_count), 1, fp) == 1
	&& fread(&stored_bytes, sizeof(stored_bytes), 1, fp) == 1)
	{
	if (stored_count < 0 || stored_bytes <= 0 || stored_bytes > (1LL << 40))
		{
		fclose(fp);
		return result;		// corrupt header: treat as a segment with no deletions
		}
	result->grow_to(stored_bytes * 8 - 1);
	if (fread(result->bitmap, 1, (size_t)stored_bytes, fp) == (size_t)stored_bytes)
		result->deleted_documents = stored_count;
	}
fclose(fp);
return result;
}

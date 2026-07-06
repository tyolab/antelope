/*
	WAL.CPP
	-------
	Write-ahead log implementation: length-prefixed binary records with
	torn-tail-tolerant replay and per-record durability control.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "wal.h"

/*
	ANT_WRITE_AHEAD_LOG::ANT_WRITE_AHEAD_LOG()
	------------------------------------------
*/
ANT_write_ahead_log::ANT_write_ahead_log()
{
fp = NULL;
dimension = 0;
is_healthy = 1;
use_fsync = 0;
replay_position = 0;
key_buffer = NULL;
key_buffer_size = 0;
document_buffer = NULL;
document_buffer_size = 0;
vector_buffer = NULL;
directory = NULL;
}

/*
	ANT_WRITE_AHEAD_LOG::~ANT_WRITE_AHEAD_LOG()
	-------------------------------------------
*/
ANT_write_ahead_log::~ANT_write_ahead_log()
{
if (fp != NULL)
	fclose(fp);
if (key_buffer != NULL)
	free(key_buffer);
if (document_buffer != NULL)
	free(document_buffer);
if (vector_buffer != NULL)
	free(vector_buffer);
if (directory != NULL)
	free(directory);
}

/*
	ANT_WRITE_AHEAD_LOG::OPEN()
	---------------------------
	Opens or creates the WAL at <directory>/wal.log in append+read binary
	mode.  Returns NULL only if the file cannot be opened.
*/
ANT_write_ahead_log *ANT_write_ahead_log::open(const char *directory, long long vector_dimension)
{
ANT_write_ahead_log *self = new ANT_write_ahead_log();
char path[4096];

if (snprintf(path, sizeof(path), "%s/wal.log", directory) >= (int)sizeof(path))
	{
	delete self;
	return NULL;
	}

self->fp = fopen(path, "a+b");
if (self->fp == NULL)
	{
	delete self;
	return NULL;
	}

/* Store the directory for use in truncate() */
self->directory = (char *)malloc(strlen(directory) + 1);
if (self->directory == NULL)
	{
	fclose(self->fp);
	delete self;
	return NULL;
	}
strcpy(self->directory, directory);

self->dimension = vector_dimension;
self->replay_position = 0;
self->is_healthy = 1;
self->use_fsync = 0;

return self;
}

/*
	ANT_WRITE_AHEAD_LOG::APPEND()
	-----------------------------
	Appends a single record: op ('A', 'U', or 'D'), key (1..8192 bytes),
	document (0..256MB, NULL when length is 0 or op is 'D'), and optional
	vector (dimension floats when dimension > 0).  Returns 0 on success,
	1 on failure (marks log unhealthy).
*/
long ANT_write_ahead_log::append(char op, const char *key, const char *document_or_null, const float *vector_or_null)
{
int32_t key_length;
int64_t doc_length = 0;
uint8_t has_vector = 0;
size_t written;

if (!is_healthy)
	return 1;

/* Validate operation code */
if (op != 'A' && op != 'U' && op != 'D')
	{
	is_healthy = 0;
	return 1;
	}

/* Validate and measure key */
if (key == NULL)
	{
	is_healthy = 0;
	return 1;
	}
key_length = (int32_t)strlen(key);
if (key_length < 1 || key_length > 8192)
	{
	is_healthy = 0;
	return 1;
	}

/* Measure document (0 for 'D' operations) */
if (op == 'D')
	{
	doc_length = 0;
	}
else if (document_or_null == NULL)
	{
	doc_length = 0;
	}
else
	{
	doc_length = (int64_t)strlen(document_or_null);
	if (doc_length < 0 || doc_length > (int64_t)(256 * 1024 * 1024))
		{
		is_healthy = 0;
		return 1;
		}
	}

/* Validate vector */
if (vector_or_null != NULL && dimension > 0)
	has_vector = 1;
else if (vector_or_null != NULL && dimension == 0)
	{
	is_healthy = 0;
	return 1;
	}

/* Seek to end and write record */
if (fseek(fp, 0, SEEK_END) != 0)
	{
	is_healthy = 0;
	return 1;
	}

/* Write op */
written = fwrite(&op, 1, 1, fp);
if (written != 1)
	{
	is_healthy = 0;
	return 1;
	}

/* Write key_length and key */
written = fwrite(&key_length, sizeof(key_length), 1, fp);
if (written != 1)
	{
	is_healthy = 0;
	return 1;
	}
written = fwrite(key, 1, key_length, fp);
if (written != (size_t)key_length)
	{
	is_healthy = 0;
	return 1;
	}

/* Write document_length and document */
written = fwrite(&doc_length, sizeof(doc_length), 1, fp);
if (written != 1)
	{
	is_healthy = 0;
	return 1;
	}
if (doc_length > 0)
	{
	written = fwrite(document_or_null, 1, doc_length, fp);
	if (written != (size_t)doc_length)
		{
		is_healthy = 0;
		return 1;
		}
	}

/* Write has_vector flag */
written = fwrite(&has_vector, 1, 1, fp);
if (written != 1)
	{
	is_healthy = 0;
	return 1;
	}

/* Write vector if present */
if (has_vector)
	{
	written = fwrite(vector_or_null, sizeof(float), dimension, fp);
	if (written != (size_t)dimension)
		{
		is_healthy = 0;
		return 1;
		}
	}

/* Flush and optionally fsync */
if (fflush(fp) != 0)
	{
	is_healthy = 0;
	return 1;
	}

if (use_fsync)
	{
	if (fsync(fileno(fp)) != 0)
		{
		is_healthy = 0;
		return 1;
		}
	}

return 0;
}

/*
	ANT_WRITE_AHEAD_LOG::REPLAY_NEXT()
	----------------------------------
	Reads the next record from the log starting at replay_position.
	Returns 1 if a complete record was read (into is filled); 0 if
	EOF or a torn/invalid record is encountered.  Buffers are reused
	across calls and remain valid until the next replay_next() or
	destruction.
*/
long ANT_write_ahead_log::replay_next(record *into)
{
uint8_t op;
int32_t key_length;
int64_t doc_length;
uint8_t has_vector;
size_t nread;

if (fseek(fp, replay_position, SEEK_SET) != 0)
	return 0;

/* Read op */
nread = fread(&op, 1, 1, fp);
if (nread != 1)
	return 0;

/* Validate op */
if (op != 'A' && op != 'U' && op != 'D')
	return 0;

/* Read and validate key_length */
nread = fread(&key_length, sizeof(key_length), 1, fp);
if (nread != 1)
	return 0;

if (key_length < 1 || key_length > 8192)
	return 0;

/* Allocate and read key */
if (key_buffer == NULL || key_length + 1 > key_buffer_size)
	{
	char *new_buf = (char *)realloc(key_buffer, key_length + 1);
	if (new_buf == NULL)
		return 0;
	key_buffer = new_buf;
	key_buffer_size = key_length + 1;
	}
nread = fread(key_buffer, 1, key_length, fp);
if (nread != (size_t)key_length)
	return 0;
key_buffer[key_length] = '\0';

/* Read and validate doc_length */
nread = fread(&doc_length, sizeof(doc_length), 1, fp);
if (nread != 1)
	return 0;

if (doc_length < 0 || doc_length > (int64_t)(256 * 1024 * 1024))
	return 0;

/* Allocate and read document */
if (doc_length > 0)
	{
	if (document_buffer == NULL || doc_length + 1 > document_buffer_size)
		{
		char *new_buf = (char *)realloc(document_buffer, doc_length + 1);
		if (new_buf == NULL)
			return 0;
		document_buffer = new_buf;
		document_buffer_size = doc_length + 1;
		}
	nread = fread(document_buffer, 1, doc_length, fp);
	if (nread != (size_t)doc_length)
		return 0;
	document_buffer[doc_length] = '\0';
	}

/* Read has_vector flag */
nread = fread(&has_vector, 1, 1, fp);
if (nread != 1)
	return 0;

/* Validate and read vector */
if (has_vector != 0 && has_vector != 1)
	return 0;

if (has_vector && dimension == 0)
	return 0;

if (has_vector)
	{
	if (vector_buffer == NULL)
		{
		vector_buffer = (float *)malloc(dimension * sizeof(float));
		if (vector_buffer == NULL)
			return 0;
		}
	nread = fread(vector_buffer, sizeof(float), dimension, fp);
	if (nread != (size_t)dimension)
		return 0;
	}

/* Only update replay_position after fully valid record */
replay_position = ftell(fp);

/* Fill output record */
into->op = op;
into->key = key_buffer;
into->document = (doc_length > 0) ? document_buffer : NULL;
into->vector = has_vector ? vector_buffer : NULL;

return 1;
}

/*
	ANT_WRITE_AHEAD_LOG::TRUNCATE()
	-------------------------------
	Empties the WAL by closing and reopening as an empty file.  Restores
	health.  Returns 0 on success, 1 on reopen failure (log becomes
	unhealthy but remains closeable).
*/
long ANT_write_ahead_log::truncate(void)
{
char path[4096];

if (fp != NULL)
	fclose(fp);

if (snprintf(path, sizeof(path), "%s/wal.log", directory) >= (int)sizeof(path))
	{
	fp = NULL;
	is_healthy = 0;
	return 1;
	}

/* Empty the file */
FILE *temp = fopen(path, "wb");
if (temp != NULL)
	fclose(temp);

/* Reopen for append+read */
fp = fopen(path, "a+b");
if (fp == NULL)
	{
	is_healthy = 0;
	return 1;
	}

replay_position = 0;
is_healthy = 1;
return 0;
}

/*
	ANT_WRITE_AHEAD_LOG::SIZE()
	---------------------------
	Returns the current size of the WAL file in bytes.
*/
long long ANT_write_ahead_log::size(void)
{
long long pos, end_pos;

pos = ftell(fp);
if (fseek(fp, 0, SEEK_END) != 0)
	return -1;
end_pos = ftell(fp);
fseek(fp, pos, SEEK_SET);
return end_pos;
}

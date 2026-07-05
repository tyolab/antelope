/*
	INDEX_MANIFEST.CPP
	------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "index_manifest.h"

/*
	ANT_INDEX_MANIFEST::ANT_INDEX_MANIFEST()
	----------------------------------------
*/
ANT_index_manifest::ANT_index_manifest(const char *directory)
{
this->directory = (char *)malloc(strlen(directory) + 1);
strcpy(this->directory, directory);
segments_allocated = 8;
segments = new long long[segments_allocated];
segments_used = 0;
generation = 1;
}

/*
	ANT_INDEX_MANIFEST::~ANT_INDEX_MANIFEST()
	-----------------------------------------
*/
ANT_index_manifest::~ANT_index_manifest()
{
free(directory);
delete [] segments;
}

/*
	ANT_INDEX_MANIFEST::ADD_SEGMENT()
	---------------------------------
	Grow-by-doubling append of segment generation number.
*/
void ANT_index_manifest::add_segment(long long segment_generation)
{
if (segments_used >= segments_allocated)
	{
	long long new_allocated = segments_allocated * 2;
	long long *new_segments = new long long[new_allocated];
	memcpy(new_segments, segments, (size_t)(segments_used * sizeof(long long)));
	delete [] segments;
	segments = new_segments;
	segments_allocated = new_allocated;
	}
segments[segments_used++] = segment_generation;
}

/*
	ANT_INDEX_MANIFEST::CONTAINS()
	------------------------------
	Linear scan to check if a segment generation number is in the manifest.
	Used for orphan cleanup.
*/
long ANT_index_manifest::contains(long long segment_generation)
{
for (long long i = 0; i < segments_used; i++)
	if (segments[i] == segment_generation)
		return 1;
return 0;
}

/*
	ANT_INDEX_MANIFEST::SAVE()
	--------------------------
	Write generation line then one line per segment to "<dir>/manifest.tmp",
	then rename over "<dir>/manifest" atomically.  On any failure, remove
	temp file and return 1.
*/
long ANT_index_manifest::save(void)
{
char temp_name[4096];
char manifest_name[4096];
FILE *fp;

if (snprintf(temp_name, sizeof(temp_name), "%s/manifest.tmp", directory) >= (int)sizeof(temp_name))
	return 1;

if (snprintf(manifest_name, sizeof(manifest_name), "%s/manifest", directory) >= (int)sizeof(manifest_name))
	return 1;

if ((fp = fopen(temp_name, "w")) == NULL)
	return 1;

if (fprintf(fp, "%lld\n", generation) < 0)
	{
	fclose(fp);
	remove(temp_name);
	return 1;
	}

for (long long i = 0; i < segments_used; i++)
	{
	if (fprintf(fp, "%lld\n", segments[i]) < 0)
		{
		fclose(fp);
		remove(temp_name);
		return 1;
		}
	}

fclose(fp);

if (rename(temp_name, manifest_name) != 0)
	{
	remove(temp_name);
	return 1;
	}

return 0;
}

/*
	ANT_INDEX_MANIFEST::LOAD()
	--------------------------
	Open "<dir>/manifest"; if missing return fresh manifest at generation 1.
	Read first line -> generation via atoll; if parsed generation < 1, keep 1.
	Remaining lines: atoll each; only add values > 0 as segments.
	Close, return.  On corruption (garbage text), return a fresh manifest.
*/
ANT_index_manifest *ANT_index_manifest::load(const char *directory)
{
ANT_index_manifest *result = new ANT_index_manifest(directory);
char manifest_path[4096];
FILE *fp;
char line[256];

if (snprintf(manifest_path, sizeof(manifest_path), "%s/manifest", directory) >= (int)sizeof(manifest_path))
	return result;		// path too long, return fresh manifest

if ((fp = fopen(manifest_path, "r")) == NULL)
	return result;		// no manifest file means fresh manifest

/*
	Read generation (first line)
*/
if (fgets(line, sizeof(line), fp) != NULL)
	{
	long long parsed_gen = atoll(line);
	if (parsed_gen >= 1)
		result->generation = parsed_gen;
	}

/*
	Read segments (remaining lines)
*/
while (fgets(line, sizeof(line), fp) != NULL)
	{
	long long seg_gen = atoll(line);
	if (seg_gen > 0)
		result->add_segment(seg_gen);
	}

fclose(fp);
return result;
}

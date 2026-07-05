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
this->directory = new char[strlen(directory) + 1];
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
delete [] directory;
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
	ANT_INDEX_MANIFEST::REMOVE_SEGMENT()
	------------------------------------
	Order-preserving removal.  In-memory only -- the caller decides when to
	save() (compaction batches removals + the addition into one atomic save).
*/
long ANT_index_manifest::remove_segment(long long segment_generation)
{
long long which, shuffle;

for (which = 0; which < segments_used; which++)
	if (segments[which] == segment_generation)
		{
		for (shuffle = which; shuffle < segments_used - 1; shuffle++)
			segments[shuffle] = segments[shuffle + 1];
		segments_used--;
		return 0;
		}
return 1;
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
	READ_MANIFEST_LINE()
	--------------------
	Read one logical line into buffer.  Returns 1 if a complete line was read
	(a final line at EOF without a trailing newline counts as complete),
	0 at EOF, or -1 if the line was too long for the buffer, in which case
	the rest of the logical line is discarded and the caller must ignore the
	truncated fragment (its tail must never be parsed as new entries).
*/
static long read_manifest_line(FILE *fp, char *buffer, size_t buffer_size)
{
int character;

if (fgets(buffer, (int)buffer_size, fp) == NULL)
	return 0;

if (strchr(buffer, '\n') == NULL && !feof(fp))
	{
	/*
		Overlong line: throw away the remainder so the next read starts at
		the next logical line, and tell the caller this fragment is garbage.
	*/
	while ((character = fgetc(fp)) != '\n' && character != EOF)
		;	// discard
	return -1;
	}

return 1;
}

/*
	ANT_INDEX_MANIFEST::LOAD()
	--------------------------
	Open "<dir>/manifest"; if missing return fresh manifest at generation 1.
	Read first line -> generation via atoll; if out of range (or the line is
	truncated garbage), keep 1.  Remaining lines: atoll each; only add
	in-range values as segments; skip overlong lines entirely.  All values
	are bounded to (0, 1 << 40): make_handle() packs generation << 40 so
	anything near that is meaningless, and an atoll() saturated to LLONG_MAX
	would otherwise overflow the clamp below (signed overflow UB).  Finally
	clamp the generation above the largest loaded segment so an inconsistent
	manifest can never hand out a generation that collides with an existing
	segment.
*/
ANT_index_manifest *ANT_index_manifest::load(const char *directory)
{
ANT_index_manifest *result = new ANT_index_manifest(directory);
char manifest_path[4096];
FILE *fp;
char line[256];
long status;

if (snprintf(manifest_path, sizeof(manifest_path), "%s/manifest", directory) >= (int)sizeof(manifest_path))
	return result;		// path too long, return fresh manifest

if ((fp = fopen(manifest_path, "r")) == NULL)
	return result;		// no manifest file means fresh manifest

/*
	Read generation (first line); a truncated first line is garbage, keep generation 1
*/
if (read_manifest_line(fp, line, sizeof(line)) == 1)
	{
	long long parsed_gen = atoll(line);
	if (parsed_gen >= 1 && parsed_gen < (1LL << 40))
		result->generation = parsed_gen;
	}

/*
	Read segments (remaining lines); ignore truncated fragments and out-of-range values
*/
while ((status = read_manifest_line(fp, line, sizeof(line))) != 0)
	{
	if (status != 1)
		continue;		// overlong line: fragment discarded, parse nothing from it
	long long seg_gen = atoll(line);
	if (seg_gen > 0 && seg_gen < (1LL << 40))
		result->add_segment(seg_gen);
	}

fclose(fp);

/*
	Cross-check: never hand out a generation that collides with a live
	segment (a manually-edited manifest could otherwise cause a silent
	overwrite of an existing segment file).
*/
long long max_seg = -1;
for (long long i = 0; i < result->segments_used; i++)
	if (result->segments[i] > max_seg)
		max_seg = result->segments[i];
if (result->generation <= max_seg)
	result->generation = max_seg + 1;

return result;
}

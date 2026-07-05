/*
	INDEX_MANIFEST.H
	----------------
	The manifest is the authoritative list of live disk segments in an index
	directory, plus the next segment generation number.  It is always updated
	by write-temp + rename so readers see either the old or the new state,
	never a torn file.

	On-disk format (text, "<dir>/manifest"):
		line 1:  next generation number
		line 2+: one live segment generation number per line
*/
#ifndef INDEX_MANIFEST_H_
#define INDEX_MANIFEST_H_

class ANT_index_manifest
{
private:
	char *directory;
	long long *segments;
	long long segments_used, segments_allocated;
	long long generation;			// next generation to hand out

private:
	ANT_index_manifest(const char *directory);

public:
	~ANT_index_manifest();

	static ANT_index_manifest *load(const char *directory);	// missing/corrupt file -> fresh manifest, generation 1
	long save(void);											// 0 on success

	long long take_generation(void) { return generation++; }
	long long get_generation(void) { return generation; }

	void add_segment(long long segment_generation);
	long long segment_count(void) { return segments_used; }
	long long get_segment(long long which) { return segments[which]; }
	long contains(long long segment_generation);
} ;

#endif /* INDEX_MANIFEST_H_ */

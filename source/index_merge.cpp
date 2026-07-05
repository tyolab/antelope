/*
	INDEX_MERGE.CPP
	---------------
*/
#include <stdio.h>
#include <stdlib.h>
#include "index_merge.h"
#include "index_tombstones.h"

/*
	ANT_DOCID_RENUMBERER::ANT_DOCID_RENUMBERER()
	--------------------------------------------
	Live documents are numbered densely: within a segment in ascending old
	docid order, segments in the order given (which is manifest order).
*/
ANT_docid_renumberer::ANT_docid_renumberer(ANT_index_tombstones **tombstones, long long *document_counts, long long segments)
{
long long segment, docid, next = 0;

segment_count = segments;
documents = new long long[segment_count];
new_docid = new long long *[segment_count];
for (segment = 0; segment < segment_count; segment++)
	{
	documents[segment] = document_counts[segment];
	new_docid[segment] = new long long[documents[segment]];
	for (docid = 0; docid < documents[segment]; docid++)
		if (tombstones[segment]->is_deleted(docid))
			new_docid[segment][docid] = -1;
		else
			new_docid[segment][docid] = next++;
	}
live_documents = next;
}

/*
	ANT_DOCID_RENUMBERER::~ANT_DOCID_RENUMBERER()
	---------------------------------------------
*/
ANT_docid_renumberer::~ANT_docid_renumberer()
{
long long segment;

for (segment = 0; segment < segment_count; segment++)
	delete [] new_docid[segment];
delete [] new_docid;
delete [] documents;
}

/*
	ANT_DOCID_RENUMBERER::LIVE_IN_SEGMENT()
	---------------------------------------
*/
long long ANT_docid_renumberer::live_in_segment(long long segment)
{
long long docid, live = 0;

for (docid = 0; docid < documents[segment]; docid++)
	if (new_docid[segment][docid] >= 0)
		live++;
return live;
}

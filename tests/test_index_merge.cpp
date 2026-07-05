/*
	TEST_INDEX_MERGE.CPP
	--------------------
	Unit tests for the compacting merger's components.  End-to-end merge
	tests live in test_segment_index.cpp; this file tests the pure parts.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "index_merge.h"
#include "index_tombstones.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
/*
	Two segments: seg0 has 5 docs with docids 1,3 deleted; seg1 has 3 docs, docid 0 deleted.
	Live docs in order: seg0:{0,2,4} -> new 0,1,2 ; seg1:{1,2} -> new 3,4.
*/
ANT_index_tombstones *t0 = new ANT_index_tombstones(5);
ANT_index_tombstones *t1 = new ANT_index_tombstones(3);
t0->set_deleted(1);
t0->set_deleted(3);
t1->set_deleted(0);

long long counts[2];
ANT_index_tombstones *stones[2];
counts[0] = 5;
counts[1] = 3;
stones[0] = t0;
stones[1] = t1;

ANT_docid_renumberer *map = new ANT_docid_renumberer(stones, counts, 2);

CHECK(map->total_live_documents() == 5);

CHECK(map->renumber(0, 0) == 0);
CHECK(map->renumber(0, 1) == -1);		// deleted
CHECK(map->renumber(0, 2) == 1);
CHECK(map->renumber(0, 3) == -1);		// deleted
CHECK(map->renumber(0, 4) == 2);
CHECK(map->renumber(1, 0) == -1);		// deleted
CHECK(map->renumber(1, 1) == 3);
CHECK(map->renumber(1, 2) == 4);

CHECK(map->live_in_segment(0) == 3);
CHECK(map->live_in_segment(1) == 2);

/*
	No tombstones at all: identity within segment + base offset
*/
ANT_index_tombstones *empty0 = new ANT_index_tombstones(2);
ANT_index_tombstones *empty1 = new ANT_index_tombstones(2);
ANT_index_tombstones *plain[2];
long long plain_counts[2];
plain[0] = empty0;
plain[1] = empty1;
plain_counts[0] = 2;
plain_counts[1] = 2;
ANT_docid_renumberer *identity = new ANT_docid_renumberer(plain, plain_counts, 2);
CHECK(identity->total_live_documents() == 4);
CHECK(identity->renumber(0, 1) == 1);
CHECK(identity->renumber(1, 0) == 2);
CHECK(identity->renumber(1, 1) == 3);

/*
	Single segment (compaction of one over-deleted segment)
*/
ANT_index_tombstones *solo_stones[1];
long long solo_counts[1];
solo_stones[0] = t0;
solo_counts[0] = 5;
ANT_docid_renumberer *solo = new ANT_docid_renumberer(solo_stones, solo_counts, 1);
CHECK(solo->total_live_documents() == 3);
CHECK(solo->renumber(0, 4) == 2);

delete map;
delete identity;
delete solo;
delete t0;
delete t1;
delete empty0;
delete empty1;
printf("PASSED\n");
return 0;
}

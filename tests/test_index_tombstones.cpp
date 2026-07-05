/*
	TEST_INDEX_TOMBSTONES.CPP
	-------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "index_tombstones.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_tombstones_XXXXXX";
char *dir = mkdtemp(dir_template);
char filename[1024];
CHECK(dir != NULL);
sprintf(filename, "%s/seg_000001.del", dir);

/*
	Fresh bitmap: nothing deleted
*/
ANT_index_tombstones *t = new ANT_index_tombstones(100);
CHECK(t->count() == 0);
CHECK(!t->is_deleted(0));
CHECK(!t->is_deleted(99));

/*
	set / get / count; setting twice counts once
*/
t->set_deleted(7);
t->set_deleted(99);
t->set_deleted(7);
CHECK(t->is_deleted(7));
CHECK(t->is_deleted(99));
CHECK(!t->is_deleted(8));
CHECK(t->count() == 2);

/*
	Grow: setting past the initial size must work (used by the live memory segment)
*/
t->set_deleted(250);
CHECK(t->is_deleted(250));
CHECK(!t->is_deleted(200));
CHECK(t->count() == 3);

/*
	Save / load round trip
*/
CHECK(t->save(filename) == 0);
ANT_index_tombstones *loaded = ANT_index_tombstones::load(filename, 300);
CHECK(loaded != NULL);
CHECK(loaded->is_deleted(7));
CHECK(loaded->is_deleted(99));
CHECK(loaded->is_deleted(250));
CHECK(!loaded->is_deleted(8));
CHECK(loaded->count() == 3);

/*
	Loading a missing file yields an empty bitmap (segment with no deletions)
*/
char missing[1024];
sprintf(missing, "%s/absent.del", dir);
ANT_index_tombstones *empty = ANT_index_tombstones::load(missing, 300);
CHECK(empty != NULL);
CHECK(empty->count() == 0);

/*
	Save must not leave a temp file behind (atomic write-temp + rename)
*/
char tmpname[1200];
sprintf(tmpname, "%s.tmp", filename);
CHECK(access(tmpname, F_OK) != 0);

delete t;
delete loaded;
delete empty;
printf("PASSED\n");
return 0;
}

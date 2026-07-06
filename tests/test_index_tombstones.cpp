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
	Save / load round trip (load with a small initial size so load()'s grow path is exercised)
*/
CHECK(t->save(filename) == 0);
ANT_index_tombstones *loaded = ANT_index_tombstones::load(filename, 1);
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
	Loading a corrupted file (bogus header) yields an empty bitmap, not a crash
*/
char corrupt_name[1024];
sprintf(corrupt_name, "%s/corrupt.del", dir);
long long bad[2];
bad[0] = 3;
bad[1] = -4096;
FILE *fp = fopen(corrupt_name, "wb");
CHECK(fp != NULL);
CHECK(fwrite(bad, sizeof(long long), 2, fp) == 2);
fclose(fp);
ANT_index_tombstones *corrupt = ANT_index_tombstones::load(corrupt_name, 100);
CHECK(corrupt != NULL);
CHECK(corrupt->count() == 0);

/*
	Truncation bomb: a header claiming stored_bytes = 2^39 with nothing behind
	it must NOT drive an absurd grow_to() allocation -- the file-size check
	rejects the file and load degrades to an empty bitmap.  The header alone
	passes the existing bounds check (2^39 < 2^40), so the file-size check is
	the defence.
*/
char bomb_name[1024];
sprintf(bomb_name, "%s/bomb.del", dir);
long long bomb_header[2];
bomb_header[0] = 3;			// stored_count
bomb_header[1] = 1LL << 39;		// stored_bytes: claims ~512GB of bitmap
FILE *bomb_fp = fopen(bomb_name, "wb");
CHECK(bomb_fp != NULL);
CHECK(fwrite(bomb_header, sizeof(long long), 2, bomb_fp) == 2);
fclose(bomb_fp);
ANT_index_tombstones *bomb = ANT_index_tombstones::load(bomb_name, 100);
CHECK(bomb != NULL);
CHECK(bomb->count() == 0);
CHECK(!bomb->is_deleted(7));

/*
	Save must not leave a temp file behind (atomic write-temp + rename)
*/
char tmpname[1200];
sprintf(tmpname, "%s.tmp", filename);
CHECK(access(tmpname, F_OK) != 0);

delete t;
delete loaded;
delete empty;
delete corrupt;
delete bomb;
printf("PASSED\n");
return 0;
}

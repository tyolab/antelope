/*
	TEST_INDEX_KEYMAP.CPP
	---------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "index_keymap.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *dir = mkdtemp(dir_template);
CHECK(dir != NULL);
long long generation, docid;

/*
	Empty map
*/
ANT_index_keymap *map = ANT_index_keymap::load(dir);
CHECK(map != NULL);
CHECK(!map->find("doc-1", &generation, &docid));

/*
	add / find / overwrite (newest wins) / remove
*/
map->add("doc-1", 1, 0);
map->add("doc-2", 1, 1);
CHECK(map->find("doc-1", &generation, &docid) && generation == 1 && docid == 0);
CHECK(map->find("doc-2", &generation, &docid) && generation == 1 && docid == 1);

map->add("doc-1", 2, 5);		// updated version lives in segment 2
CHECK(map->find("doc-1", &generation, &docid) && generation == 2 && docid == 5);

map->remove("doc-2");
CHECK(!map->find("doc-2", &generation, &docid));

/*
	Growth: many keys must survive table resize
*/
char key[64];
long long i;
for (i = 0; i < 5000; i++)
	{
	sprintf(key, "bulk-%lld", i);
	map->add(key, 3, i);
	}
for (i = 0; i < 5000; i++)
	{
	sprintf(key, "bulk-%lld", i);
	CHECK(map->find(key, &generation, &docid) && generation == 3 && docid == i);
	}
CHECK(map->find("doc-1", &generation, &docid) && generation == 2 && docid == 5);

/*
	Persistence: the append-only log replays to the same state
*/
delete map;
ANT_index_keymap *reloaded = ANT_index_keymap::load(dir);
CHECK(reloaded->find("doc-1", &generation, &docid) && generation == 2 && docid == 5);
CHECK(!reloaded->find("doc-2", &generation, &docid));
sprintf(key, "bulk-%lld", (long long)4999);
CHECK(reloaded->find(key, &generation, &docid) && generation == 3 && docid == 4999);

delete reloaded;

/*
	HARDENING TEST 1: Corrupt log line (A\tgarbage\n) followed by valid record
	The corrupt line should be skipped, and the valid record after it should load
*/
char corrupt_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *corrupt_dir = mkdtemp(corrupt_dir_template);
CHECK(corrupt_dir != NULL);
char corrupt_log[1200];
snprintf(corrupt_log, sizeof(corrupt_log), "%s/keymap.log", corrupt_dir);
FILE *fp = fopen(corrupt_log, "w");
CHECK(fp != NULL);
fputs("A\tgarbage\n", fp);			// corrupt: too few fields
fputs("A\t5\t10\tdoc-x\n", fp);		// valid record after corruption
fclose(fp);
ANT_index_keymap *map_corrupt = ANT_index_keymap::load(corrupt_dir);
CHECK(map_corrupt != NULL);
// The corrupt line (A\tgarbage) is malformed and should be skipped
// The valid line (A\t5\t10\tdoc-x) should load successfully
CHECK(map_corrupt->find("doc-x", &generation, &docid) && generation == 5 && docid == 10);
delete map_corrupt;

/*
	HARDENING TEST 2: Over-range generation record (generation > (1LL << 40))
	Record should be skipped entirely; next valid record should load
*/
char range_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *range_dir = mkdtemp(range_dir_template);
CHECK(range_dir != NULL);
char range_log[1200];
snprintf(range_log, sizeof(range_log), "%s/keymap.log", range_dir);
fp = fopen(range_log, "w");
CHECK(fp != NULL);
fprintf(fp, "A\t%lld\t7\tdoc-over\n", (1LL << 41));		// generation way out of range
fputs("A\t5\t20\tdoc-valid\n", fp);					// valid record after out-of-range
fclose(fp);
ANT_index_keymap *map_range = ANT_index_keymap::load(range_dir);
CHECK(map_range != NULL);
CHECK(!map_range->find("doc-over", &generation, &docid));		// out-of-range record skipped
CHECK(map_range->find("doc-valid", &generation, &docid) && generation == 5 && docid == 20);
delete map_range;

/*
	HARDENING TEST 3: Over-long log line (10000 chars, no newline before truncation)
	Line should be drained; next valid record should load
*/
char long_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *long_dir = mkdtemp(long_dir_template);
CHECK(long_dir != NULL);
char long_log[1200];
snprintf(long_log, sizeof(long_log), "%s/keymap.log", long_dir);
fp = fopen(long_log, "w");
CHECK(fp != NULL);
fputs("A\t3\t100\t", fp);
for (int j = 0; j < 10000; j++)
	fputc('x', fp);
fputs("\nA\t6\t30\tdoc-after-long\n", fp);
fclose(fp);
ANT_index_keymap *map_long = ANT_index_keymap::load(long_dir);
CHECK(map_long != NULL);
// The long line (10000 chars) exceeds the read buffer (8192 bytes)
// so it should be drained and skipped
CHECK(!map_long->find("doc-x", &generation, &docid));		// if the long line had a key, it would fail here
CHECK(map_long->find("doc-after-long", &generation, &docid) && generation == 6 && docid == 30);
delete map_long;

/*
	HARDENING TEST 4: add() with a key containing '\t' is a no-op (find fails)
*/
char tab_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *tab_dir = mkdtemp(tab_dir_template);
CHECK(tab_dir != NULL);
ANT_index_keymap *map_tab = ANT_index_keymap::load(tab_dir);
CHECK(map_tab != NULL);
map_tab->add("doc\twithtab", 1, 0);	// should be rejected: no-op
CHECK(!map_tab->find("doc\twithtab", &generation, &docid));	// not found
delete map_tab;

/*
	HARDENING TEST 5: add() with a key containing '\n' is a no-op (find fails)
*/
char newline_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *newline_dir = mkdtemp(newline_dir_template);
CHECK(newline_dir != NULL);
ANT_index_keymap *map_newline = ANT_index_keymap::load(newline_dir);
CHECK(map_newline != NULL);
map_newline->add("doc\nwithnewline", 1, 0);	// should be rejected: no-op
CHECK(!map_newline->find("doc\nwithnewline", &generation, &docid));	// not found
delete map_newline;

/*
	HARDENING TEST 6: add() with empty key is a no-op (find fails)
*/
char empty_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *empty_dir = mkdtemp(empty_dir_template);
CHECK(empty_dir != NULL);
ANT_index_keymap *map_empty = ANT_index_keymap::load(empty_dir);
CHECK(map_empty != NULL);
map_empty->add("", 1, 0);	// should be rejected: no-op
CHECK(!map_empty->find("", &generation, &docid));	// not found
delete map_empty;

/*
	HARDENING TEST 7: Malformed docid field (non-numeric) must skip the record.
	atoll would silently parse "abc" as 0, and 0 is a valid docid, so a strict
	parser must reject the record; the valid record after it must still load.
*/
char baddoc_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *baddoc_dir = mkdtemp(baddoc_dir_template);
CHECK(baddoc_dir != NULL);
char baddoc_log[1200];
snprintf(baddoc_log, sizeof(baddoc_log), "%s/keymap.log", baddoc_dir);
fp = fopen(baddoc_log, "w");
CHECK(fp != NULL);
fputs("A\t5\tabc\tdoc-x\n", fp);		// malformed docid: must be skipped, not read as docid 0
fputs("A\t5\t20\tdoc-ok\n", fp);		// valid record after it
fclose(fp);
ANT_index_keymap *map_baddoc = ANT_index_keymap::load(baddoc_dir);
CHECK(map_baddoc != NULL);
CHECK(!map_baddoc->find("doc-x", &generation, &docid));
CHECK(map_baddoc->find("doc-ok", &generation, &docid) && generation == 5 && docid == 20);
delete map_baddoc;

/*
	Same-key toggle: add, remove, re-add with new (generation, docid);
	find must return the newest values (covers slot reuse after removal)
*/
char toggle_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *toggle_dir = mkdtemp(toggle_dir_template);
CHECK(toggle_dir != NULL);
ANT_index_keymap *map_toggle = ANT_index_keymap::load(toggle_dir);
CHECK(map_toggle != NULL);
map_toggle->add("toggle-key", 1, 3);
CHECK(map_toggle->find("toggle-key", &generation, &docid) && generation == 1 && docid == 3);
map_toggle->remove("toggle-key");
CHECK(!map_toggle->find("toggle-key", &generation, &docid));
map_toggle->add("toggle-key", 4, 9);
CHECK(map_toggle->find("toggle-key", &generation, &docid) && generation == 4 && docid == 9);
delete map_toggle;

/*
	The toggle sequence must also replay correctly from the log
*/
ANT_index_keymap *map_toggle2 = ANT_index_keymap::load(toggle_dir);
CHECK(map_toggle2 != NULL);
CHECK(map_toggle2->find("toggle-key", &generation, &docid) && generation == 4 && docid == 9);
delete map_toggle2;

/*
	RETAIN_GENERATIONS: entries whose generation is not in the given list
	are dropped; entries whose generation is in the list are untouched;
	already-removed entries stay removed; the reconciliation survives reload.
*/
char retain_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *retain_dir = mkdtemp(retain_dir_template);
CHECK(retain_dir != NULL);
ANT_index_keymap *map_retain = ANT_index_keymap::load(retain_dir);
CHECK(map_retain != NULL);
map_retain->add("kept-1", 1, 0);		// generation 1: retained
map_retain->add("kept-2", 2, 1);		// generation 2: retained
map_retain->add("stale-1", 3, 2);		// generation 3: not retained -> dropped
map_retain->add("already-gone", 1, 3);
map_retain->remove("already-gone");		// already removed before reconciliation

long long keep_generations[2] = { 1, 2 };
map_retain->retain_generations(keep_generations, 2);

CHECK(map_retain->find("kept-1", &generation, &docid) && generation == 1 && docid == 0);
CHECK(map_retain->find("kept-2", &generation, &docid) && generation == 2 && docid == 1);
CHECK(!map_retain->find("stale-1", &generation, &docid));
CHECK(!map_retain->find("already-gone", &generation, &docid));
delete map_retain;

/*
	Reload: the dropped entry's removal was logged, so it stays dropped
*/
ANT_index_keymap *map_retain2 = ANT_index_keymap::load(retain_dir);
CHECK(map_retain2 != NULL);
CHECK(map_retain2->find("kept-1", &generation, &docid) && generation == 1 && docid == 0);
CHECK(map_retain2->find("kept-2", &generation, &docid) && generation == 2 && docid == 1);
CHECK(!map_retain2->find("stale-1", &generation, &docid));
delete map_retain2;

/*
	LOG_EXISTS: missing directory/file -> 0; after load()+add() -> 1
*/
char exists_dir_template[] = "/tmp/ant_keymap_XXXXXX";
char *exists_dir = mkdtemp(exists_dir_template);
CHECK(exists_dir != NULL);
CHECK(ANT_index_keymap::log_exists(exists_dir) == 0);		// dir exists, but no keymap.log yet
CHECK(ANT_index_keymap::log_exists("/tmp/ant_keymap_no_such_dir_xyz") == 0);	// dir does not exist

ANT_index_keymap *map_exists = ANT_index_keymap::load(exists_dir);
CHECK(map_exists != NULL);
/*
	load()'s no-log-file branch fopen()s the path with mode "a", which
	creates the (empty) file immediately -- not lazily on first write -- so
	the log already exists on disk at this point, before add() is ever called.
*/
CHECK(ANT_index_keymap::log_exists(exists_dir) == 1);
map_exists->add("some-key", 1, 0);
CHECK(ANT_index_keymap::log_exists(exists_dir) == 1);
delete map_exists;

/*
	compact_log(): rewrite the append-only log keeping only live entries
*/
char cl_template[] = "/tmp/ant_keymap_XXXXXX";
char *cl_dir = mkdtemp(cl_template);
CHECK(cl_dir != NULL);
ANT_index_keymap *cl = ANT_index_keymap::load(cl_dir);
long long cl_gen, cl_docid;
char cl_key[64];
for (long long i = 0; i < 100; i++)
	{
	sprintf(cl_key, "churn-%lld", i % 10);		// 10 keys, updated 10x each
	cl->add(cl_key, 1, i);
	}
delete cl;

ANT_index_keymap *cl2 = ANT_index_keymap::load(cl_dir);
CHECK(cl2->log_dead_ratio() > 0.8);				// 100 records, 10 live
char cl_log[1200];
snprintf(cl_log, sizeof(cl_log), "%s/keymap.log", cl_dir);
struct stat before_stat;
CHECK(stat(cl_log, &before_stat) == 0);
CHECK(cl2->compact_log() == 0);
struct stat after_stat;
CHECK(stat(cl_log, &after_stat) == 0);
CHECK(after_stat.st_size < before_stat.st_size / 2);
/* state preserved and still appendable */
for (long long i = 0; i < 10; i++)
	{
	sprintf(cl_key, "churn-%lld", i);
	CHECK(cl2->find(cl_key, &cl_gen, &cl_docid) && cl_docid == 90 + i);
	}
cl2->add("post-compact", 2, 7);
delete cl2;
ANT_index_keymap *cl3 = ANT_index_keymap::load(cl_dir);
CHECK(cl3->log_dead_ratio() < 0.1);
CHECK(cl3->find("post-compact", &cl_gen, &cl_docid) && cl_gen == 2 && cl_docid == 7);
sprintf(cl_key, "churn-%lld", (long long)4);
CHECK(cl3->find(cl_key, &cl_gen, &cl_docid) && cl_docid == 94);
delete cl3;

printf("PASSED\n");
return 0;
}

/*
	TEST_INDEX_MANIFEST.CPP
	-----------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "index_manifest.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_manifest_XXXXXX";
char *dir = mkdtemp(dir_template);
CHECK(dir != NULL);

/*
	Loading from an empty directory yields a new, empty manifest at generation 1
	(generation is the *next* segment number to hand out)
*/
ANT_index_manifest *m = ANT_index_manifest::load(dir);
CHECK(m != NULL);
CHECK(m->segment_count() == 0);
CHECK(m->get_generation() == 1);

/*
	Take generations, add segments, save
*/
long long g1 = m->take_generation();
long long g2 = m->take_generation();
CHECK(g1 == 1);
CHECK(g2 == 2);
m->add_segment(g1);
m->add_segment(g2);
CHECK(m->segment_count() == 2);
CHECK(m->get_segment(0) == g1);
CHECK(m->get_segment(1) == g2);
CHECK(m->save() == 0);

/*
	Reload: same contents, generation continues from where we left off
*/
ANT_index_manifest *m2 = ANT_index_manifest::load(dir);
CHECK(m2->segment_count() == 2);
CHECK(m2->get_segment(0) == g1);
CHECK(m2->get_segment(1) == g2);
CHECK(m2->take_generation() == 3);

/*
	contains(): used for orphan cleanup
*/
CHECK(m2->contains(g1));
CHECK(!m2->contains(99));

/*
	A corrupted manifest (garbage text) degrades to a fresh manifest,
	never a crash
*/
char corrupt_dir_template[] = "/tmp/ant_manifest_XXXXXX";
char *corrupt_dir = mkdtemp(corrupt_dir_template);
CHECK(corrupt_dir != NULL);
char corrupt_name[1200];
snprintf(corrupt_name, sizeof(corrupt_name), "%s/manifest", corrupt_dir);
FILE *fp = fopen(corrupt_name, "wb");
fputs("not-a-number\n-7\ngarbage\n", fp);
fclose(fp);
ANT_index_manifest *m3 = ANT_index_manifest::load(corrupt_dir);
CHECK(m3 != NULL);
CHECK(m3->segment_count() == 0);
CHECK(m3->get_generation() >= 1);

/*
	An overlong line (longer than any sane parse buffer) must be discarded
	whole: its tail must never be parsed as extra segment entries
*/
char long_dir_template[] = "/tmp/ant_manifest_XXXXXX";
char *long_dir = mkdtemp(long_dir_template);
CHECK(long_dir != NULL);
char long_name[1200];
snprintf(long_name, sizeof(long_name), "%s/manifest", long_dir);
fp = fopen(long_name, "wb");
CHECK(fp != NULL);
fputs("7\n", fp);
for (int i = 0; i < 400; i++)
	fputc('x', fp);
fputs("\n42\n", fp);
fclose(fp);
ANT_index_manifest *m4 = ANT_index_manifest::load(long_dir);
CHECK(m4 != NULL);
CHECK(m4->segment_count() == 1);
CHECK(m4->get_segment(0) == 42);

/*
	A manifest whose generation collides with a live segment must be clamped
	above the largest segment so take_generation() can never reuse a number
*/
char collide_dir_template[] = "/tmp/ant_manifest_XXXXXX";
char *collide_dir = mkdtemp(collide_dir_template);
CHECK(collide_dir != NULL);
char collide_name[1200];
snprintf(collide_name, sizeof(collide_name), "%s/manifest", collide_dir);
fp = fopen(collide_name, "wb");
CHECK(fp != NULL);
fputs("5\n5\n10\n", fp);
fclose(fp);
ANT_index_manifest *m5 = ANT_index_manifest::load(collide_dir);
CHECK(m5 != NULL);
CHECK(m5->segment_count() == 2);
CHECK(m5->take_generation() == 11);

/*
	An over-range number (atoll saturates to LLONG_MAX) must be rejected at
	parse time, never reach the generation clamp (signed overflow UB), and
	never turn the generation negative
*/
char range_dir_template[] = "/tmp/ant_manifest_XXXXXX";
char *range_dir = mkdtemp(range_dir_template);
CHECK(range_dir != NULL);
char range_name[1200];
snprintf(range_name, sizeof(range_name), "%s/manifest", range_dir);
fp = fopen(range_name, "wb");
CHECK(fp != NULL);
fputs("500\n", fp);
fputs("999999999999999999999999999999\n", fp);
fputs("7\n", fp);
fclose(fp);
ANT_index_manifest *m6 = ANT_index_manifest::load(range_dir);
CHECK(m6 != NULL);
CHECK(m6->segment_count() == 1);
CHECK(m6->get_segment(0) == 7);
CHECK(m6->get_generation() == 500);
CHECK(m6->take_generation() == 500);

/*
	Atomic save: no temp file left behind
*/
char tmpname[1200];
snprintf(tmpname, sizeof(tmpname), "%s/manifest.tmp", dir);
CHECK(access(tmpname, F_OK) != 0);

delete m;
delete m2;
delete m3;
delete m4;
delete m5;
delete m6;

/*
	remove_segment(): used by compaction to drop merged inputs
*/
char rm_template[] = "/tmp/ant_manifest_XXXXXX";
char *rm_dir = mkdtemp(rm_template);
CHECK(rm_dir != NULL);
ANT_index_manifest *rm = ANT_index_manifest::load(rm_dir);
rm->add_segment(rm->take_generation());		// 1
rm->add_segment(rm->take_generation());		// 2
rm->add_segment(rm->take_generation());		// 3
CHECK(rm->remove_segment(2) == 0);
CHECK(rm->segment_count() == 2);
CHECK(rm->get_segment(0) == 1);
CHECK(rm->get_segment(1) == 3);
CHECK(!rm->contains(2));
CHECK(rm->remove_segment(99) == 1);			// absent -> 1, no change
CHECK(rm->segment_count() == 2);
CHECK(rm->save() == 0);
ANT_index_manifest *rm2 = ANT_index_manifest::load(rm_dir);
CHECK(rm2->segment_count() == 2);
CHECK(rm2->contains(1) && rm2->contains(3) && !rm2->contains(2));
delete rm;
delete rm2;

printf("PASSED\n");
return 0;
}

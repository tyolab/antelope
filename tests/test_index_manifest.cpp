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
	Atomic save: no temp file left behind
*/
char tmpname[1200];
snprintf(tmpname, sizeof(tmpname), "%s/manifest.tmp", dir);
CHECK(access(tmpname, F_OK) != 0);

delete m;
delete m2;
delete m3;
printf("PASSED\n");
return 0;
}

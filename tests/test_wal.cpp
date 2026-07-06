/*
	TEST_WAL.CPP
	------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wal.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_wal_XXXXXX";
char *dir = mkdtemp(dir_template);
CHECK(dir != NULL);
float vec[3] = {0.5f, 0.25f, 0.0f};

/*
	Append three ops, one with a vector
*/
ANT_write_ahead_log *wal = ANT_write_ahead_log::open(dir, 3);
CHECK(wal != NULL);
CHECK(wal->healthy());
CHECK(wal->append('A', "doc-1", "<DOC>alpha</DOC>", NULL) == 0);
CHECK(wal->append('U', "doc-1", "<DOC>beta</DOC>", vec) == 0);
CHECK(wal->append('D', "doc-2", NULL, NULL) == 0);
delete wal;

/*
	Replay: exact records back, in order
*/
ANT_write_ahead_log *replay = ANT_write_ahead_log::open(dir, 3);
ANT_write_ahead_log::record record;
CHECK(replay->replay_next(&record));
CHECK(record.op == 'A' && strcmp(record.key, "doc-1") == 0 && strcmp(record.document, "<DOC>alpha</DOC>") == 0 && record.vector == NULL);
CHECK(replay->replay_next(&record));
CHECK(record.op == 'U' && record.vector != NULL && record.vector[1] == 0.25f);
CHECK(replay->replay_next(&record));
CHECK(record.op == 'D' && strcmp(record.key, "doc-2") == 0 && record.document == NULL);
CHECK(!replay->replay_next(&record));			// clean end

/*
	Truncate resets to empty
*/
CHECK(replay->truncate() == 0);
delete replay;
ANT_write_ahead_log *empty = ANT_write_ahead_log::open(dir, 3);
CHECK(!empty->replay_next(&record));
CHECK(empty->append('A', "doc-3", "<DOC>gamma</DOC>", NULL) == 0);
delete empty;

/*
	Torn tail: append garbage bytes; replay yields the good record then stops
*/
char wal_name[1200];
snprintf(wal_name, sizeof(wal_name), "%s/wal.log", dir);
FILE *fp = fopen(wal_name, "ab");
fputc('A', fp);
fputc(0x42, fp);								// half a record
fclose(fp);
ANT_write_ahead_log *torn = ANT_write_ahead_log::open(dir, 3);
CHECK(torn->replay_next(&record));
CHECK(record.op == 'A' && strcmp(record.key, "doc-3") == 0);
CHECK(!torn->replay_next(&record));				// tear ends replay, no crash
delete torn;

/*
	Bounds: a fabricated record claiming a huge key length is rejected
	(replay ends) without attempting the allocation
*/
CHECK(truncate(wal_name, 0) == 0);
fp = fopen(wal_name, "ab");
unsigned char op = 'A';
int32_t bad_len = 1 << 30;
fwrite(&op, 1, 1, fp);
fwrite(&bad_len, sizeof(bad_len), 1, fp);
fclose(fp);
ANT_write_ahead_log *bad = ANT_write_ahead_log::open(dir, 3);
CHECK(!bad->replay_next(&record));
delete bad;

printf("PASSED\n");
return 0;
}

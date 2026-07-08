/*
	TEST_PQ_BACKFILL.CPP
	--------------------
	build_pq() backfill: train+encode a .pq for a flushed segment, and load it
	on reopen.  disk_segment_has_pq() reflects both.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

#define DIM 16

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_pqbackfill_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/* deterministic distinct 16-d vector seeded by i */
static void make_vec(long long i, float *v)
{
for (int d = 0; d < DIM; d++)
	v[d] = (float)(((i * 131 + d * 17) % 97) - 48) / 40.0f;
}

/*
	TEST_BUILD_PQ_AND_RELOAD()
	--------------------------
	After flush + build_pq(), the segment has a .pq; after close+reopen the .pq
	loads back.
*/
static void test_build_pq_and_reload(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);

float v[DIM];
char key[32], body[64];
for (long long i = 0; i < 40; i++)
	{
	make_vec(i, v);
	sprintf(key, "doc-%lld", i);
	sprintf(body, "<DOC>term%lld content</DOC>", i);
	CHECK(ix->add_document(key, body, v) >= 0);
	}
CHECK(ix->flush() == 0);

CHECK(ix->disk_segment_has_pq(0) == 0);		/* ondemand: not built yet */
CHECK(ix->build_pq() == 0);
CHECK(ix->disk_segment_has_pq(0) == 1);		/* built now */
delete ix;

/* reopen: the .pq must load in append_segment() */
ATIRE_segment_index *re = new ATIRE_segment_index();
CHECK(re->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(re->open(dir) == 0);
CHECK(re->pq_configured());
CHECK(re->disk_segment_has_pq(0) == 1);		/* loaded from disk on open */
delete re;

delete [] dir;
printf("test_build_pq_and_reload OK\n");
}

/*
	TEST_BUILD_PQ_UNCONFIGURED_IS_NOOP()
	------------------------------------
*/
static void test_build_pq_unconfigured_is_noop(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
/* no set_pq_config() */
CHECK(ix->build_pq() == 1);					/* PQ unconfigured => no-op nonzero */
delete ix;
delete [] dir;
printf("test_build_pq_unconfigured_is_noop OK\n");
}

/*
	TEST_EAGER_POLICY_BUILDS_AT_FLUSH()
	-----------------------------------
	With set_pq_policy(1), flush() builds the .pq automatically.
*/
static void test_eager_policy_builds_at_flush(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(DIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_pq_policy(1) == 0);

float v[DIM];
char key[32], body[64];
for (long long i = 0; i < 40; i++)
	{
	make_vec(i, v);
	sprintf(key, "doc-%lld", i);
	sprintf(body, "<DOC>term%lld content</DOC>", i);
	CHECK(ix->add_document(key, body, v) >= 0);
	}
CHECK(ix->flush() == 0);
CHECK(ix->disk_segment_has_pq(0) == 1);		/* eager: built at flush */
delete ix;
delete [] dir;
printf("test_eager_policy_builds_at_flush OK\n");
}

int main(void)
{
test_build_pq_and_reload();
test_build_pq_unconfigured_is_noop();
test_eager_policy_builds_at_flush();
printf("test_pq_backfill PASSED\n");
return 0;
}

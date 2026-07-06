/*
	TEST_VECTOR_STORE.CPP
	---------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "vector_store.h"
#include "index_tombstones.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_vecstore_XXXXXX";
char *dir = mkdtemp(dir_template);
char filename[1024], tmpname[1200];
long long which;
CHECK(dir != NULL);
sprintf(filename, "%s/seg_000001.vec", dir);

/*
	Write 4 docs, dimension 3; doc 2 has no vector
*/
float v0[3] = {1.0f, 0.0f, 0.0f};
float v1[3] = {0.0f, 1.0f, 0.0f};
float v3[3] = {0.6f, 0.8f, 0.0f};

ANT_vector_store_writer *writer = new ANT_vector_store_writer();
CHECK(writer->create(filename, 3) == 0);
CHECK(writer->append(v0) == 0);
CHECK(writer->append(v1) == 0);
CHECK(writer->append(NULL) == 0);		// doc 2: absent
CHECK(writer->append(v3) == 0);
CHECK(writer->finish() == 0);
delete writer;
sprintf(tmpname, "%s.tmp", filename);
CHECK(access(tmpname, F_OK) != 0);		// atomic: no temp survivor

/*
	Load and verify contents + presence
*/
ANT_vector_store *store = ANT_vector_store::load(filename, 3, 4);
CHECK(store != NULL);
CHECK(store->document_count() == 4);
CHECK(store->has(0));
CHECK(store->has(1));
CHECK(!store->has(2));
CHECK(store->has(3));
CHECK(store->get(0)[0] == 1.0f && store->get(0)[1] == 0.0f);
CHECK(store->get(3)[1] == 0.8f);

/*
	Top-k scan, dot metric: query {1,0,0} -> doc0 (1.0), doc3 (0.6), doc1 (0.0);
	doc2 absent must never appear
*/
float query[3] = {1.0f, 0.0f, 0.0f};
ANT_vector_candidate best[8];
long long best_count = 0;
ANT_index_tombstones *no_deletes = new ANT_index_tombstones(4);
store->scan(query, ANT_vector_store::METRIC_DOT, no_deletes, 7, best, &best_count, 3);
CHECK(best_count == 3);
/* order within best[] is unspecified (replace-min set); verify membership + scores */
long saw0 = 0, saw1 = 0, saw3 = 0;
for (which = 0; which < best_count; which++)
	{
	if (best[which].docid == 0) { saw0 = 1; CHECK(fabs(best[which].score - 1.0) < 1e-6); }
	if (best[which].docid == 1) { saw1 = 1; CHECK(fabs(best[which].score - 0.0) < 1e-6); }
	if (best[which].docid == 3) { saw3 = 1; CHECK(fabs(best[which].score - 0.6) < 1e-6); }
	CHECK(best[which].generation == 7);
	CHECK(best[which].docid != 2);
	}
CHECK(saw0 && saw1 && saw3);

/*
	Tombstone skip: delete doc 0, k=2 -> docs 3 and 1
*/
ANT_index_tombstones *dead0 = new ANT_index_tombstones(4);
dead0->set_deleted(0);
best_count = 0;
store->scan(query, ANT_vector_store::METRIC_DOT, dead0, 7, best, &best_count, 2);
CHECK(best_count == 2);
for (which = 0; which < best_count; which++)
	CHECK(best[which].docid == 1 || best[which].docid == 3);

/*
	k larger than live vector count: returns what exists
*/
best_count = 0;
store->scan(query, ANT_vector_store::METRIC_DOT, no_deletes, 7, best, &best_count, 8);
CHECK(best_count == 3);

/*
	L2 metric: query {0,1,0} -> doc1 distance 0 (score 0), doc0/-2, doc3 -(0.36+0.04)=-0.4
*/
float q2[3] = {0.0f, 1.0f, 0.0f};
best_count = 0;
store->scan(q2, ANT_vector_store::METRIC_L2, no_deletes, 7, best, &best_count, 1);
CHECK(best_count == 1);
CHECK(best[which = 0].docid == 1);
CHECK(fabs(best[0].score - 0.0) < 1e-6);

/*
	Normalization helper (cosine support): {3,4,0} -> {0.6,0.8,0}; zero vector rejected
*/
float n[3] = {3.0f, 4.0f, 0.0f};
CHECK(ANT_vector_store::normalize(n, 3) == 0);
CHECK(fabs(n[0] - 0.6f) < 1e-6 && fabs(n[1] - 0.8f) < 1e-6);
float z[3] = {0.0f, 0.0f, 0.0f};
CHECK(ANT_vector_store::normalize(z, 3) != 0);

/*
	Corruption: bad magic -> load returns degraded empty store (never NULL-crash)
*/
char corrupt_name[1024];
sprintf(corrupt_name, "%s/corrupt.vec", dir);
FILE *fp = fopen(corrupt_name, "wb");
fputs("not a vector store at all", fp);
fclose(fp);
ANT_vector_store *corrupt = ANT_vector_store::load(corrupt_name, 3, 4);
CHECK(corrupt != NULL);
CHECK(!corrupt->has(0));
best_count = 0;
corrupt->scan(query, ANT_vector_store::METRIC_DOT, no_deletes, 7, best, &best_count, 3);
CHECK(best_count == 0);

/*
	Dimension mismatch and missing file also degrade to empty
*/
ANT_vector_store *wrong_dim = ANT_vector_store::load(filename, 5, 4);
CHECK(wrong_dim != NULL && !wrong_dim->has(0));
char missing[1024];
sprintf(missing, "%s/absent.vec", dir);
ANT_vector_store *none = ANT_vector_store::load(missing, 3, 4);
CHECK(none != NULL && !none->has(0));

delete store;
delete corrupt;
delete wrong_dim;
delete none;
delete no_deletes;
delete dead0;
printf("PASSED\n");
return 0;
}

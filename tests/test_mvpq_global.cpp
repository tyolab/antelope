/*
	TEST_MVPQ_GLOBAL.CPP -- token epic 2/4 Task 2: engine global token codebook.
	Train ONE frozen collection-wide `.mvpq` codebook (persisted to
	<dir>/multivector_pq.codebook, config v4), so token codes are comparable
	across segments; default-off writes no sidecar and stays byte-identical.
	Mirrors tests/test_pq_global.cpp's Task-2 engine harness, adapted to the
	ragged token pool (set_rerank_config / set_multivector_pq_config /
	add_document(...,rows,md) / flush / build_multivector_pq).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_store.h"
#include "../source/multivector_pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

#define RD 8			/* rerank (token) dimension */
#define MM 4			/* subvector count -> sub = 2 */

static char *make_engine_dir(const char *tmpl)
{
	char buffer[64];
	strcpy(buffer, tmpl);
	char *dir = mkdtemp(buffer);
	if (dir == NULL) exit(printf("cannot create scratch dir\n"));
	char *result = new char[strlen(dir) + 1];
	strcpy(result, dir);
	return result;
}

/* deterministic normalized RD-dim token vector for a given seed */
static void fill_tok(long long seed, float *v)
{
	double n = 0;
	for (int j = 0; j < RD; j++) { v[j] = (float)(((seed * 7 + j * 3) % 13) - 6) / 6.0f; n += v[j] * v[j]; }
	n = sqrt(n) + 1e-9;
	for (int j = 0; j < RD; j++) v[j] /= (float)n;
}

/* doc i: md tokens (2..4), each a deterministic normalized RD-dim vector */
static void add_tdocs(ATIRE_segment_index *ix, long long lo, long long hi)
{
	for (long long i = lo; i < hi; i++)
		{
		int md = 2 + (int)(i % 3);
		float rows[4 * RD];
		for (int r = 0; r < md; r++) fill_tok(i * 5 + r * 3, rows + r * RD);
		char key[32]; snprintf(key, sizeof(key), "tdoc-%lld", i);
		CHECK(ix->add_document(key, "body words here", NULL, rows, md) >= 0);
		}
}

/* a single-token probe doc (docid 0 -> token index 0 of its segment) */
static void add_probe(ATIRE_segment_index *ix, const char *key, const float *probe)
{
	CHECK(ix->add_document(key, "probe body", NULL, probe, 1) >= 0);
}

/* train-once + persistence: multivector_pq.codebook is written once global mode
   is on, and a fresh reopen picks it back up (multivector_pq_global_codebook()
   survives via config v4; token search still works). */
static void test_train_once_and_persistence(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_mvgtop_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	CHECK(ix->multivector_pq_global_codebook() == 1);

	add_tdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_has_multivector_pq(0) == 1);

	add_tdocs(ix, N, N + N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_has_multivector_pq(1) == 1);

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/multivector_pq.codebook", dir);
	FILE *fp = fopen(cb_path, "rb");
	CHECK(fp != NULL);
	fclose(fp);

	delete ix;

	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	CHECK(re->multivector_pq_global_codebook() == 1);		/* persisted config v4 survives reopen */
	CHECK(re->build_token_index() == 0);
	float q[2 * RD];
	fill_tok(3, q); fill_tok(11, q + RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);			/* .mvpq stores + shared codebook still usable */
	delete re;

	delete [] dir;
	printf("test_train_once_and_persistence OK\n");
}

/* cross-segment comparability: the SAME token vector, encoded in two different
   segments under global mode, must produce IDENTICAL code bytes (both segments
   embed a copy of the one shared codebook). */
static void test_cross_segment_comparability(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_mvgxseg_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	float probe[RD];
	fill_tok(999, probe);						/* shared single-token probe, reused verbatim in both segments */

	add_probe(ix, "probe-a", probe);			/* docid 0 == the probe (token index 0) */
	add_tdocs(ix, 1, N);						/* fill out segment A */
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);		/* trains the global codebook from segment A's tokens */

	add_probe(ix, "probe-b", probe);			/* docid 0 == the same probe (token index 0) */
	add_tdocs(ix, N + 1, N + N);				/* segment B */
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);		/* reuses the SAME codebook (no retrain) */

	CHECK(ix->disk_segment_count() == 2);
	long long gen_a = ix->disk_segment_generation(0);
	long long gen_b = ix->disk_segment_generation(1);
	long long docs_a = N, docs_b = N;
	delete ix;

	char pa[4096], pb[4096];
	snprintf(pa, sizeof(pa), "%s/seg_%06lld.mvpq", dir, gen_a);
	snprintf(pb, sizeof(pb), "%s/seg_%06lld.mvpq", dir, gen_b);
	ANT_multivector_pq_store *sa = ANT_multivector_pq_store::load(pa, RD, docs_a, ANT_pq_codec::METRIC_DOT);
	ANT_multivector_pq_store *sb = ANT_multivector_pq_store::load(pb, RD, docs_b, ANT_pq_codec::METRIC_DOT);
	CHECK(sa != NULL && sb != NULL && sa->token_count() > 0 && sb->token_count() > 0);

	/* embedded codebooks are identical (both reference the frozen global codebook) */
	size_t cb_bytes = (size_t)(MM * (long long)ANT_pq_codec::K * (RD / MM)) * sizeof(float);
	CHECK(memcmp(sa->get_codebook(), sb->get_codebook(), cb_bytes) == 0);

	/* the shared probe (token index 0 in both segments) encodes to the same code bytes */
	CHECK(sa->token_codes(0) != NULL && sb->token_codes(0) != NULL);
	CHECK(memcmp(sa->token_codes(0), sb->token_codes(0), (size_t)MM) == 0);

	delete sa; delete sb;
	delete [] dir;
	printf("test_cross_segment_comparability OK\n");
}

/* default-off: an index that never calls set_multivector_pq_global_codebook()
   builds .mvpq exactly as today -- spot-assert that no multivector_pq.codebook
   sidecar is written. */
static void test_default_off_no_codebook_file(void)
{
	const long long N = 10;
	char *dir = make_engine_dir("/tmp/ant_mvgdoff_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* NOTE: set_multivector_pq_global_codebook() deliberately NOT called -- default off */

	add_tdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
	CHECK(ix->multivector_pq_global_codebook() == 0);

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/multivector_pq.codebook", dir);
	FILE *fp = fopen(cb_path, "rb");
	CHECK(fp == NULL);						/* sidecar absent under default-off mode */

	delete ix;
	delete [] dir;
	printf("test_default_off_no_codebook_file OK\n");
}

int main(void)
{
	test_train_once_and_persistence();
	test_cross_segment_comparability();
	test_default_off_no_codebook_file();
	printf("ALL test_mvpq_global PASSED\n");
	return 0;
}

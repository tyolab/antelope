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

/*
	----------------------------------------------------------------------
	Task 3: compaction no-retrain + rebuild_mvpq_global_codebook() + OPQ
	composition + forgiving-load.
	----------------------------------------------------------------------
*/

static long read_file_bytes(const char *path, unsigned char **out, long *out_len)
{
	FILE *fp = fopen(path, "rb");
	if (fp == NULL) return 1;
	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	rewind(fp);
	unsigned char *buf = new unsigned char[len > 0 ? len : 1];
	if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len)
		{ fclose(fp); delete [] buf; return 1; }
	fclose(fp);
	*out = buf; *out_len = len;
	return 0;
}

static long long mvpq_cb_floats(void)
{
	return MM * (long long)ANT_pq_codec::K * (RD / MM);
}

/* multivector_pq.codebook (ANTMVGCB) layout:
   magic(8) version(4) dimension(8) m(8) k(8) opq(8) [rotation dim*dim floats] codebook(m*K*sub floats).
   The codebook block is ALWAYS last, so locate it from the tail. */
static void assert_store_codebook_matches_sidecar(ANT_multivector_pq_store *s, const unsigned char *file_bytes, long file_len)
{
	long long bytes = mvpq_cb_floats() * (long long)sizeof(float);
	CHECK(file_len >= bytes);
	CHECK(memcmp(s->get_codebook(), file_bytes + (file_len - bytes), (size_t)bytes) == 0);
}

/* differently-distributed token: concentrated opposite-sign spike, unlike fill_tok's
   spread -- used to prove rebuild actually retrains against the new distribution. */
static void fill_shift_tok(long long seed, float *v)
{
	double n = 0;
	for (int j = 0; j < RD; j++) { v[j] = 0.05f * (float)(((seed * 3 + j) % 4) - 2); n += v[j] * v[j]; }
	v[seed % RD] -= 5.0f; n += 25.0;
	n = sqrt(n) + 1e-9;
	for (int j = 0; j < RD; j++) v[j] /= (float)n;
}

static void add_shift_tdocs(ATIRE_segment_index *ix, long long lo, long long hi)
{
	for (long long i = lo; i < hi; i++)
		{
		int md = 2 + (int)(i % 3);
		float rows[4 * RD];
		for (int r = 0; r < md; r++) fill_shift_tok(i * 5 + r * 3, rows + r * RD);
		char key[32]; snprintf(key, sizeof(key), "sdoc-%lld", i);
		CHECK(ix->add_document(key, "shifted body text", NULL, rows, md) >= 0);
		}
}

/* no-retrain compaction: two global-mode segments (codebook trained once from
   segment A, reused verbatim for B); compacting them must NOT retrain -- the
   multivector_pq.codebook bytes stay identical and the merged .mvpq embeds the
   (unchanged) global codebook. */
static void test_compaction_reuses_global_codebook(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_mvgcmp_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	add_tdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);			/* trains the global codebook from segment A */

	add_tdocs(ix, N, N + N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);			/* reuses it (no retrain) */

	CHECK(ix->disk_segment_count() == 2);

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/multivector_pq.codebook", dir);
	unsigned char *before = NULL; long before_len = 0;
	CHECK(read_file_bytes(cb_path, &before, &before_len) == 0);

	long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
	CHECK(ix->compact(gens, 2) == 0);
	CHECK(ix->disk_segment_count() == 1);
	CHECK(ix->disk_segment_has_multivector_pq(0) == 1);

	unsigned char *after = NULL; long after_len = 0;
	CHECK(read_file_bytes(cb_path, &after, &after_len) == 0);
	CHECK(after_len == before_len && memcmp(before, after, (size_t)before_len) == 0);	/* compaction did NOT retrain */

	long long out_gen = ix->disk_segment_generation(0);
	char mvpq_path[4096];
	snprintf(mvpq_path, sizeof(mvpq_path), "%s/seg_%06lld.mvpq", dir, out_gen);
	ANT_multivector_pq_store *merged = ANT_multivector_pq_store::load(mvpq_path, RD, N + N, ANT_pq_codec::METRIC_DOT);
	CHECK(merged != NULL && merged->token_count() > 0);
	assert_store_codebook_matches_sidecar(merged, after, after_len);
	delete merged;

	CHECK(ix->build_token_index() == 0);
	float q[2 * RD]; fill_tok(3, q); fill_tok(11, q + RD);
	CHECK(ix->search_multivector(q, 2, 10) > 0);

	delete [] before; delete [] after;
	delete ix;
	delete [] dir;
	printf("test_compaction_reuses_global_codebook OK\n");
}

/* rebuild: after adding a third, differently-distributed segment (built against
   the STALE global codebook, since ensure_* is a no-op once trained),
   rebuild_mvpq_global_codebook() must retrain from ALL segments' tokens (new
   sidecar bytes differ) and re-encode every segment (all three embed the new
   codebook, byte-equal across segments); token search still resolves. */
static void test_rebuild_global_codebook(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_mvgreb_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	add_tdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);

	add_tdocs(ix, N, N + N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/multivector_pq.codebook", dir);
	unsigned char *before = NULL; long before_len = 0;
	CHECK(read_file_bytes(cb_path, &before, &before_len) == 0);

	add_shift_tdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);			/* still reuses the OLD (stale) codebook */
	CHECK(ix->disk_segment_count() == 3);

	CHECK(ix->rebuild_mvpq_global_codebook() == 0);

	unsigned char *after = NULL; long after_len = 0;
	CHECK(read_file_bytes(cb_path, &after, &after_len) == 0);
	CHECK(!(after_len == before_len && memcmp(before, after, (size_t)before_len) == 0));	/* retrained: bytes differ */

	/* every segment now embeds the NEW codebook -- byte-equal to the sidecar tail (and thus, transitively, to each other) */
	for (long long which = 0; which < ix->disk_segment_count(); which++)
		{
		CHECK(ix->disk_segment_has_multivector_pq(which) == 1);
		long long gen = ix->disk_segment_generation(which);
		char mp[4096];
		snprintf(mp, sizeof(mp), "%s/seg_%06lld.mvpq", dir, gen);
		ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(mp, RD, N, ANT_pq_codec::METRIC_DOT);
		CHECK(s != NULL && s->token_count() > 0);
		assert_store_codebook_matches_sidecar(s, after, after_len);
		delete s;
		}

	CHECK(ix->build_token_index() == 0);
	float q[2 * RD]; fill_shift_tok(3, q); fill_shift_tok(8, q + RD);
	CHECK(ix->search_multivector(q, 2, 10) > 0);

	delete [] before; delete [] after;
	delete ix;
	delete [] dir;
	printf("test_rebuild_global_codebook OK\n");
}

/* OPQ (T1) composition: global mode + set_multivector_pq_opq(1) trains ONE shared
   R + codebook once; the sidecar carries opq==1 + an R block (round-trips through
   save/load on reopen); every segment embeds the same codebook, and the shared
   probe token encodes to identical code bytes across segments (same R AND
   codebook). */
static void test_opq_global_composition(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_mvgopq_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_opq(1) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	float probe[RD]; fill_tok(777, probe);			/* shared single-token probe, reused verbatim in both segments */

	add_probe(ix, "probe-a", probe);				/* docid 0 -> token index 0 */
	add_tdocs(ix, 1, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);			/* trains the global R + codebook from segment A */

	add_probe(ix, "probe-b", probe);				/* docid 0 -> the same probe token */
	add_tdocs(ix, N + 1, N + N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);			/* reuses the SAME R + codebook (no retrain) */

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/multivector_pq.codebook", dir);
	unsigned char *bytes = NULL; long len = 0;
	CHECK(read_file_bytes(cb_path, &bytes, &len) == 0);

	/* header: magic(8) version(4) dimension(8) m(8) k(8) opq(8) */
	long long opq_flag; memcpy(&opq_flag, bytes + 8+4+8+8+8, 8);
	CHECK(opq_flag == 1);
	long long expect_len = (long long)(8+4+8+8+8+8) + RD * RD * (long long)sizeof(float)
		+ mvpq_cb_floats() * (long long)sizeof(float);
	CHECK((long long)len == expect_len);			/* R block present alongside the codebook */

	CHECK(ix->disk_segment_count() == 2);
	long long gen_a = ix->disk_segment_generation(0);
	long long gen_b = ix->disk_segment_generation(1);
	delete ix;

	/* R round-trips through load: a fresh reopen restores opq + searches */
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	CHECK(re->multivector_pq_opq() == 1);
	CHECK(re->multivector_pq_global_codebook() == 1);
	CHECK(re->build_token_index() == 0);
	float q[RD]; fill_tok(777, q);
	CHECK(re->search_multivector(q, 1, 5) > 0);
	delete re;

	char pa[4096], pb[4096];
	snprintf(pa, sizeof(pa), "%s/seg_%06lld.mvpq", dir, gen_a);
	snprintf(pb, sizeof(pb), "%s/seg_%06lld.mvpq", dir, gen_b);
	ANT_multivector_pq_store *sa = ANT_multivector_pq_store::load(pa, RD, N, ANT_pq_codec::METRIC_DOT);
	ANT_multivector_pq_store *sb = ANT_multivector_pq_store::load(pb, RD, N, ANT_pq_codec::METRIC_DOT);
	CHECK(sa != NULL && sb != NULL && sa->token_count() > 0 && sb->token_count() > 0);

	size_t cb_bytes = (size_t)mvpq_cb_floats() * sizeof(float);
	CHECK(memcmp(sa->get_codebook(), sb->get_codebook(), cb_bytes) == 0);	/* same embedded codebook */
	/* the shared probe (token index 0 in both segments) encodes identically -> same R AND codebook */
	CHECK(sa->token_codes(0) != NULL && sb->token_codes(0) != NULL);
	CHECK(memcmp(sa->token_codes(0), sb->token_codes(0), (size_t)MM) == 0);

	delete sa; delete sb;
	delete [] bytes;
	delete [] dir;
	printf("test_opq_global_composition OK\n");
}

/* forgiving-load: a truncated/corrupt multivector_pq.codebook is treated as
   untrained (NULL buffers) on reopen -- the next build retrains cleanly, no
   crash/over-read. */
static void test_forgiving_load_corrupt_codebook(void)
{
	const long long N = 10;
	char *dir = make_engine_dir("/tmp/ant_mvgforg_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	add_tdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_has_multivector_pq(0) == 1);
	delete ix;

	/* truncate the sidecar mid-header -> forgiving load must degrade to untrained */
	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/multivector_pq.codebook", dir);
	FILE *fp = fopen(cb_path, "rb+");
	CHECK(fp != NULL);
	CHECK(ftruncate(fileno(fp), 20) == 0);			/* keep magic+version, drop the rest */
	fclose(fp);

	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);						/* load_mvpq_codebook degrades to NULL, no crash */
	CHECK(re->multivector_pq_global_codebook() == 1);

	/* a fresh segment with no .mvpq forces ensure_* to retrain a valid codebook */
	add_tdocs(re, N, N + N);
	CHECK(re->flush() == 0);
	CHECK(re->build_multivector_pq() == 0);
	CHECK(re->disk_segment_has_multivector_pq(1) == 1);

	/* sidecar is valid again (full-size, not the 20-byte stub) */
	unsigned char *after = NULL; long after_len = 0;
	CHECK(read_file_bytes(cb_path, &after, &after_len) == 0);
	CHECK(after_len >= (long)(8+4+8+8+8+8) + (long)(mvpq_cb_floats() * (long long)sizeof(float)));

	CHECK(re->build_token_index() == 0);
	float q[2 * RD]; fill_tok(3, q); fill_tok(15, q + RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);

	delete [] after;
	delete re;
	delete [] dir;
	printf("test_forgiving_load_corrupt_codebook OK\n");
}

/* NONE-tier rebuild (C1 regression): under the NONE resident tier, open() wraps
   the .mvpq store in an ANT_multivector_pq_source that token_source/token_index
   borrow.  rebuild_mvpq_global_codebook() deletes+reloads every segment's .mvpq,
   so it MUST drop token_index and rebuild token_source over the new store (and
   invalidate the stale .tann); otherwise the graph-path search_multivector after
   the rebuild is a heap use-after-free.  This exercises the graph path both
   before and after the rebuild. */
static void test_none_tier_rebuild_no_uaf(void)
{
	const long long N = 14;
	char *dir = make_engine_dir("/tmp/ant_mvgnone_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	add_tdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);

	add_shift_tdocs(ix, 0, N);						/* differently-distributed second segment */
	CHECK(ix->flush() == 0);
	CHECK(ix->build_multivector_pq() == 0);

	CHECK(ix->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete ix;										/* NONE takes effect on reopen */

	ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);						/* token_source = ANT_multivector_pq_source over each .mvpq */
	CHECK(ix->multivector_pq_global_codebook() == 1);
	CHECK(ix->disk_segment_count() == 2);
	CHECK(ix->disk_segment_resident_tier_mv(0) == ATIRE_segment_index::MV_TIER_NONE);
	CHECK(ix->disk_segment_resident_tier_mv(1) == ATIRE_segment_index::MV_TIER_NONE);

	CHECK(ix->build_token_index() == 0);			/* .tann graph over the PQ (ADC) source */
	CHECK(ix->disk_segment_has_token_index(0) == 1);	/* graph path is live */

	float q[2 * RD]; fill_tok(3, q); fill_shift_tok(5, q + RD);
	long long before = ix->search_multivector(q, 2, 10);	/* graph path, BEFORE rebuild */
	CHECK(before > 0);

	CHECK(ix->rebuild_mvpq_global_codebook() == 0);	/* deletes+reloads every .mvpq store */

	/* DETERMINISTIC C1 catch: the rebuild MUST have dropped every stale
	   token_index (each borrowed a token_source over a now-freed .mvpq store).
	   Without the fix these stay non-NULL and the graph-path search below is a
	   heap use-after-free (which a plain assertion cannot observe reliably without
	   ASan); asserting the invalidation itself pins the bug deterministically. */
	CHECK(ix->disk_segment_has_token_index(0) == 0);
	CHECK(ix->disk_segment_has_token_index(1) == 0);

	/* With token_index dropped, this takes the exact-scan fallback and still
	   answers sanely (no crash, non-empty). */
	long long mid = ix->search_multivector(q, 2, 10);
	CHECK(mid > 0);

	/* rebuild token graph over the NEW stores and exercise the graph path AFTER */
	CHECK(ix->build_token_index() == 0);
	CHECK(ix->disk_segment_has_token_index(0) == 1);
	long long after = ix->search_multivector(q, 2, 10);
	CHECK(after > 0);

	delete ix;
	delete [] dir;
	printf("test_none_tier_rebuild_no_uaf OK\n");
}

int main(void)
{
	test_train_once_and_persistence();
	test_cross_segment_comparability();
	test_default_off_no_codebook_file();
	test_compaction_reuses_global_codebook();
	test_rebuild_global_codebook();
	test_opq_global_composition();
	test_forgiving_load_corrupt_codebook();
	test_none_tier_rebuild_no_uaf();
	printf("ALL test_mvpq_global PASSED\n");
	return 0;
}

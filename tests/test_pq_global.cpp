/*
	TEST_PQ_GLOBAL.CPP -- #22.2 global codebook. Task 1 locks the writer's
	external-codebook seam: finish() with a supplied codebook skips training,
	encodes against it, and embeds it (so load round-trips identically).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

// Train a standalone codebook over `n` D-dim rows (no OPQ), for use as an "external" codebook.
static float *train_codebook(const float *vecs, long long D, long long m, long long n)
{
	long long sub = D / m, floats = m * (long long)ANT_pq_codec::K * sub;
	float *cb = new float[floats];
	CHECK(ANT_pq_codec::train(vecs, D, m, ANT_pq_codec::K, n, cb) == 0);
	return cb;
}

static void test_external_codebook_encodes_and_embeds(void)
{
	const long long D = 8, m = 4, n = 50;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*7 + d*13) % 17) - 8);
	float *ext_cb = train_codebook(vecs, D, m, n);

	char path[] = "/tmp/ant_gcb_XXXXXX"; CHECK(mkstemp(path) >= 0);
	ANT_pq_store_writer w;
	CHECK(w.create(path, D, m, 256, ANT_pq_codec::METRIC_L2, 0) == 0);
	w.set_external_codebook(ext_cb, NULL);              // supply codebook, no OPQ rotation
	for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
	CHECK(w.finish() == 0);

	ANT_pq_store *s = ANT_pq_store::load(path, D, n, ANT_pq_codec::METRIC_L2);
	CHECK(s != NULL && s->document_count() == n);
	// the embedded codebook must equal the supplied one (finish did not retrain)
	CHECK(memcmp(s->get_codebook(), ext_cb, (size_t)(m*(long long)ANT_pq_codec::K*(D/m))*sizeof(float)) == 0);
	// codes must equal a direct encode against the same codebook
	for (long long doc = 0; doc < n; doc++)
		{
		unsigned char expect[4];
		ANT_pq_codec::encode(vecs + doc*D, D, m, ANT_pq_codec::K, ext_cb, expect);
		CHECK(memcmp(s->codes_for(doc), expect, (size_t)m) == 0);
		}
	delete s; remove(path); delete [] ext_cb; delete [] vecs;
	printf("test_external_codebook_encodes_and_embeds OK\n");
}

static void test_non_external_path_unchanged(void)
{
	// A writer with NO external codebook trains its own — same file two runs => byte-identical (deterministic).
	const long long D = 6, m = 3, n = 40;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*5 + d*11) % 13) - 6);
	char a[] = "/tmp/ant_gna_XXXXXX", b[] = "/tmp/ant_gnb_XXXXXX";
	CHECK(mkstemp(a) >= 0); CHECK(mkstemp(b) >= 0);
	for (int pass = 0; pass < 2; pass++)
		{
		ANT_pq_store_writer w;
		CHECK(w.create(pass ? b : a, D, m, 256, ANT_pq_codec::METRIC_L2, 0) == 0);
		for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
		CHECK(w.finish() == 0);
		}
	FILE *fa = fopen(a,"rb"), *fb = fopen(b,"rb"); CHECK(fa && fb);
	fseek(fa,0,SEEK_END); long la = ftell(fa); fseek(fb,0,SEEK_END); long lb = ftell(fb);
	CHECK(la == lb && la > 0); rewind(fa); rewind(fb);
	unsigned char *ba = new unsigned char[la], *bb = new unsigned char[lb];
	CHECK(fread(ba,1,la,fa)==(size_t)la && fread(bb,1,lb,fb)==(size_t)lb);
	CHECK(memcmp(ba, bb, la) == 0);
	fclose(fa); fclose(fb); delete[] ba; delete[] bb;
	remove(a); remove(b); delete [] vecs;
	printf("test_non_external_path_unchanged OK\n");
}

/*
	----------------------------------------------------------------------
	Task 2: engine global codebook -- train-once, pq.codebook persistence,
	cross-segment code comparability, default-off byte-identity (no
	pq.codebook sidecar).
	----------------------------------------------------------------------
*/
#define GDIM 16

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

/* doc i: dominant coordinate on unique axis (i % GDIM) -- mirrors test_pq_metrics.cpp's pattern */
static void make_gvec(long long i, float *v)
{
	for (int d = 0; d < GDIM; d++)
		v[d] = 0.02f * (float)(((i * 7 + d) % 5) - 2);
	v[i % GDIM] += 3.0f;
}

static void add_gdocs(ATIRE_segment_index *ix, long long lo, long long hi)
{
	float v[GDIM]; char key[32], body[64];
	for (long long i = lo; i < hi; i++)
		{
		make_gvec(i, v);
		sprintf(key, "gdoc-%lld", i);
		sprintf(body, "<DOC>gterm%lld z</DOC>", i);
		CHECK(ix->add_document(key, body, v) >= 0);
		}
}

/* train-once + persistence: pq.codebook is written once global mode is on,
   and a fresh reopen picks it back up (pq_global_codebook() survives, search
   still works off the persisted .pq stores). */
static void test_train_once_and_persistence(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_gtop_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);
	CHECK(ix->pq_global_codebook() == 1);

	add_gdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);
	CHECK(ix->disk_segment_has_pq(0) == 1);

	add_gdocs(ix, N, N + N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);
	CHECK(ix->disk_segment_has_pq(1) == 1);

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/pq.codebook", dir);
	FILE *fp = fopen(cb_path, "rb");
	CHECK(fp != NULL);
	fclose(fp);

	delete ix;

	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(re->open(dir) == 0);
	CHECK(re->pq_global_codebook() == 1);				/* persisted config v4 survives reopen */
	float q[GDIM]; make_gvec(5, q);
	CHECK(re->search_vector(q, 5) >= 1);				/* .pq stores + shared codebook still usable */
	delete re;

	delete [] dir;
	printf("test_train_once_and_persistence OK\n");
}

/* cross-segment comparability: the SAME vector, PQ-encoded in two different
   segments under global mode, must produce IDENTICAL code bytes (both
   segments embed a copy of the one shared codebook). */
static void test_cross_segment_comparability(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_gxseg_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);

	float probe[GDIM];
	make_gvec(3, probe);						/* shared probe vector, reused verbatim in both segments */

	CHECK(ix->add_document("probe-a", "<DOC>proba</DOC>", probe) >= 0);
	add_gdocs(ix, 1, N);						/* fill out the rest of segment A (docid 0 == the probe) */
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);					/* trains the global codebook from segment A's floats */

	CHECK(ix->add_document("probe-b", "<DOC>probb</DOC>", probe) >= 0);
	add_gdocs(ix, N + 1, N + N);				/* segment B; docid 0 == the same probe */
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);					/* reuses the SAME codebook (no retrain) */

	CHECK(ix->disk_segment_count() == 2);
	long long gen_a = ix->disk_segment_generation(0);
	long long gen_b = ix->disk_segment_generation(1);
	delete ix;

	char pa[4096], pb[4096];
	snprintf(pa, sizeof(pa), "%s/seg_%06lld.pq", dir, gen_a);
	snprintf(pb, sizeof(pb), "%s/seg_%06lld.pq", dir, gen_b);
	ANT_pq_store *sa = ANT_pq_store::load(pa, GDIM, N, ANT_pq_codec::METRIC_DOT);
	ANT_pq_store *sb = ANT_pq_store::load(pb, GDIM, N, ANT_pq_codec::METRIC_DOT);
	CHECK(sa != NULL && sb != NULL && sa->document_count() == N && sb->document_count() == N);

	/* embedded codebooks are identical (both reference the frozen global codebook) */
	size_t cb_bytes = (size_t)(4 * (long long)ANT_pq_codec::K * (GDIM / 4)) * sizeof(float);
	CHECK(memcmp(sa->get_codebook(), sb->get_codebook(), cb_bytes) == 0);

	/* the shared probe vector (docid 0 in both segments) encodes to the same code bytes */
	CHECK(memcmp(sa->codes_for(0), sb->codes_for(0), 4) == 0);

	delete sa; delete sb;
	delete [] dir;
	printf("test_cross_segment_comparability OK\n");
}

/* default-off: an index that never calls set_pq_global_codebook() builds .pq
   exactly as today (existing suites already cover the resulting bytes/search
   behaviour) -- spot-assert here that no pq.codebook sidecar is written. */
static void test_default_off_no_codebook_file(void)
{
	const long long N = 8;
	char *dir = make_engine_dir("/tmp/ant_gdoff_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	/* NOTE: set_pq_global_codebook() deliberately NOT called -- default off */

	add_gdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);
	CHECK(ix->disk_segment_has_pq(0) == 1);
	CHECK(ix->pq_global_codebook() == 0);

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/pq.codebook", dir);
	FILE *fp = fopen(cb_path, "rb");
	CHECK(fp == NULL);						/* sidecar absent under default-off mode */

	delete ix;
	delete [] dir;
	printf("test_default_off_no_codebook_file OK\n");
}

/*
	----------------------------------------------------------------------
	Task 3: compaction no-retrain + rebuild_pq_global_codebook().
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

static long long gcb_codebook_floats(long long dim, long long m)
{
	long long sub = dim / m;
	return m * (long long)ANT_pq_codec::K * sub;
}

/* pq.codebook layout: magic(8) version(4) dimension(8) m(8) k(8) opq(8) [rotation dim*dim floats] codebook(m*K*sub floats).
   The codebook block is always LAST, so we can locate it from the tail without decoding the header. */
static void assert_codebook_matches_file(ANT_pq_store *s, const unsigned char *file_bytes, long file_len, long long dim, long long m)
{
	long long floats = gcb_codebook_floats(dim, m);
	long long bytes = floats * (long long)sizeof(float);
	CHECK(file_len >= bytes);
	CHECK(memcmp(s->get_codebook(), file_bytes + (file_len - bytes), (size_t)bytes) == 0);
}

/* no-retrain compaction: two global-mode segments built (codebook trained once
   from segment A, reused verbatim for segment B); compacting them must NOT
   retrain -- pq.codebook bytes stay identical, and the merged segment's
   embedded codebook still equals the (unchanged) global codebook. */
static void test_compaction_reuses_global_codebook(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_gcompact_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);

	add_gdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);					/* trains the global codebook from segment A */

	add_gdocs(ix, N, N + N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);					/* reuses it (no retrain) */

	CHECK(ix->disk_segment_count() == 2);

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/pq.codebook", dir);
	unsigned char *before = NULL; long before_len = 0;
	CHECK(read_file_bytes(cb_path, &before, &before_len) == 0);

	long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
	CHECK(ix->compact(gens, 2) == 0);
	CHECK(ix->disk_segment_count() == 1);
	CHECK(ix->disk_segment_has_pq(0) == 1);

	unsigned char *after = NULL; long after_len = 0;
	CHECK(read_file_bytes(cb_path, &after, &after_len) == 0);
	CHECK(after_len == before_len && memcmp(before, after, (size_t)before_len) == 0);	/* compaction did NOT retrain */

	long long out_gen = ix->disk_segment_generation(0);
	char pq_path[4096];
	snprintf(pq_path, sizeof(pq_path), "%s/seg_%06lld.pq", dir, out_gen);
	ANT_pq_store *merged = ANT_pq_store::load(pq_path, GDIM, N + N, ANT_pq_codec::METRIC_DOT);
	CHECK(merged != NULL && merged->document_count() == N + N);
	assert_codebook_matches_file(merged, after, after_len, GDIM, 4);
	delete merged;

	float q[GDIM]; make_gvec(7, q);
	CHECK(ix->search_vector(q, 5) >= 1);

	delete [] before; delete [] after;
	delete ix;
	delete [] dir;
	printf("test_compaction_reuses_global_codebook OK\n");
}

/* doc i: differently-distributed from make_gvec (opposite-sign, larger, shifted
   dominant coordinate) -- used to prove rebuild_pq_global_codebook() actually
   retrains against the new distribution rather than being a no-op. */
static void make_shift_vec(long long i, float *v)
{
	for (int d = 0; d < GDIM; d++)
		v[d] = 0.02f * (float)(((i * 11 + d * 3) % 5) - 2);
	v[i % GDIM] -= 6.0f;
}

static void add_shift_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{
	float v[GDIM]; char key[32], body[64];
	for (long long i = lo; i < hi; i++)
		{
		make_shift_vec(i, v);
		sprintf(key, "shift-%lld", i);
		sprintf(body, "<DOC>shiftterm%lld z</DOC>", i);
		CHECK(ix->add_document(key, body, v) >= 0);
		}
}

/* rebuild: after adding a third, differently-distributed segment (still built
   against the STALE global codebook, since ensure_* is a no-op once trained),
   rebuild_pq_global_codebook() must retrain from ALL segments' floats (new
   pq.codebook bytes differ) and re-encode every segment (all three now embed
   the new codebook); search still resolves correctly post-rebuild. */
static void test_rebuild_global_codebook(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_grebuild_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);

	add_gdocs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);

	add_gdocs(ix, N, N + N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/pq.codebook", dir);
	unsigned char *before = NULL; long before_len = 0;
	CHECK(read_file_bytes(cb_path, &before, &before_len) == 0);

	add_shift_docs(ix, 0, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);					/* still reuses the OLD (stale) codebook */
	CHECK(ix->disk_segment_count() == 3);

	CHECK(ix->rebuild_pq_global_codebook() == 0);

	unsigned char *after = NULL; long after_len = 0;
	CHECK(read_file_bytes(cb_path, &after, &after_len) == 0);
	CHECK(!(after_len == before_len && memcmp(before, after, (size_t)before_len) == 0));	/* retrained: bytes differ */

	for (long long which = 0; which < ix->disk_segment_count(); which++)
		{
		CHECK(ix->disk_segment_has_pq(which) == 1);
		long long gen = ix->disk_segment_generation(which);
		char pq_path[4096];
		snprintf(pq_path, sizeof(pq_path), "%s/seg_%06lld.pq", dir, gen);
		ANT_pq_store *s = ANT_pq_store::load(pq_path, GDIM, N, ANT_pq_codec::METRIC_DOT);
		CHECK(s != NULL && s->document_count() == N);
		assert_codebook_matches_file(s, after, after_len, GDIM, 4);
		delete s;
		}

	/* sanity: search still resolves a shifted-distribution doc as its own top-1 */
	float q[GDIM]; make_shift_vec(3, q);
	CHECK(ix->search_vector(q, 5) >= 1);
	CHECK(strcmp(ix->get_hit(0)->filename, "shift-3") == 0);

	delete [] before; delete [] after;
	delete ix;
	delete [] dir;
	printf("test_rebuild_global_codebook OK\n");
}

/* OPQ composition: global mode + OPQ together train/persist ONE shared R +
   codebook (pq.codebook grows an R block, opq flag == 1); every segment
   embeds the same R+codebook, so the SAME probe vector encodes identically
   in both segments (rotation composed with the codebook, not just the codebook). */
static void test_opq_global_composition(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_gopq_XXXXXX");

	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_opq(1) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);

	float probe[GDIM];
	make_gvec(3, probe);						/* shared probe vector, reused verbatim in both segments */

	CHECK(ix->add_document("probe-a", "<DOC>proba</DOC>", probe) >= 0);
	add_gdocs(ix, 1, N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);					/* trains the global codebook + rotation from segment A */

	CHECK(ix->add_document("probe-b", "<DOC>probb</DOC>", probe) >= 0);
	add_gdocs(ix, N + 1, N + N);
	CHECK(ix->flush() == 0);
	CHECK(ix->build_pq() == 0);					/* reuses the SAME R + codebook (no retrain) */

	char cb_path[4096];
	snprintf(cb_path, sizeof(cb_path), "%s/pq.codebook", dir);
	unsigned char *bytes = NULL; long len = 0;
	CHECK(read_file_bytes(cb_path, &bytes, &len) == 0);

	/* header layout: magic(8) version(4) dimension(8) m(8) k(8) opq(8) */
	long long opq_flag; memcpy(&opq_flag, bytes + 8+4+8+8+8, 8);
	CHECK(opq_flag == 1);
	long long expect_len = (long long)(8+4+8+8+8+8) + GDIM * GDIM * (long long)sizeof(float)
		+ gcb_codebook_floats(GDIM, 4) * (long long)sizeof(float);
	CHECK((long long)len == expect_len);				/* R block present alongside the codebook */

	CHECK(ix->disk_segment_count() == 2);
	long long gen_a = ix->disk_segment_generation(0);
	long long gen_b = ix->disk_segment_generation(1);
	delete ix;

	char pa[4096], pb[4096];
	snprintf(pa, sizeof(pa), "%s/seg_%06lld.pq", dir, gen_a);
	snprintf(pb, sizeof(pb), "%s/seg_%06lld.pq", dir, gen_b);
	ANT_pq_store *sa = ANT_pq_store::load(pa, GDIM, N, ANT_pq_codec::METRIC_DOT);
	ANT_pq_store *sb = ANT_pq_store::load(pb, GDIM, N, ANT_pq_codec::METRIC_DOT);
	CHECK(sa != NULL && sb != NULL && sa->document_count() == N && sb->document_count() == N);

	assert_codebook_matches_file(sa, bytes, len, GDIM, 4);
	assert_codebook_matches_file(sb, bytes, len, GDIM, 4);
	/* the shared probe vector (docid 0 in both segments) encodes to the same code bytes */
	CHECK(memcmp(sa->codes_for(0), sb->codes_for(0), 4) == 0);

	delete sa; delete sb;
	delete [] bytes;
	delete [] dir;
	printf("test_opq_global_composition OK\n");
}

int main(void)
{
	test_external_codebook_encodes_and_embeds();
	test_non_external_path_unchanged();
	test_train_once_and_persistence();
	test_cross_segment_comparability();
	test_default_off_no_codebook_file();
	test_compaction_reuses_global_codebook();
	test_rebuild_global_codebook();
	test_opq_global_composition();
	printf("ALL TESTS PASSED\n");
	return 0;
}

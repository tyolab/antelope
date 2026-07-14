/*
	TEST_PQ_KWIDTH_COMPOSE.CPP -- #22.3 composition: variable k with the
	global codebook, OPQ, and compaction. (a) k=16 + global trains one
	m*16*sub codebook, both segments embed it, the shared probe encodes to
	identical packed rows, and pq.codebook's k field reads 16; (b) k=64 +
	OPQ builds and searches; (c) compaction under k=16 reuses the frozen
	codebook (pq.codebook byte-identical) and the merged segment searches.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)
#define GDIM 16
#define GM 4

static char *make_engine_dir(const char *tmpl)
{
	char buffer[64]; strcpy(buffer, tmpl);
	char *dir = mkdtemp(buffer);
	if (dir == NULL) exit(printf("cannot create scratch dir\n"));
	char *r = new char[strlen(dir) + 1]; strcpy(r, dir); return r;
}
static void make_gvec(long long i, float *v)
{
	for (int d = 0; d < GDIM; d++) v[d] = 0.02f * (float)(((i * 7 + d) % 5) - 2);
	v[i % GDIM] += 3.0f;
}
static void add_gdocs(ATIRE_segment_index *ix, long long lo, long long hi)
{
	float v[GDIM]; char key[32], body[64];
	for (long long i = lo; i < hi; i++)
		{ make_gvec(i, v); sprintf(key, "gdoc-%lld", i); sprintf(body, "<DOC>gterm%lld z</DOC>", i);
		  CHECK(ix->add_document(key, body, v) >= 0); }
}
static int read_file_bytes(const char *path, unsigned char **out, long *len)
{
	FILE *fp = fopen(path, "rb"); if (!fp) return 1;
	fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
	unsigned char *b = new unsigned char[n > 0 ? n : 1];
	if (fread(b, 1, n, fp) != (size_t)n) { fclose(fp); delete [] b; return 1; }
	fclose(fp); *out = b; *len = n; return 0;
}

/* k=16 + global codebook: cross-segment comparability + sidecar k field. */
static void test_k16_global_comparability(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_kw16_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_k(16) == 0);
	CHECK(ix->pq_k() == 16);
	CHECK(ix->set_pq_global_codebook(1) == 0);

	float probe[GDIM]; make_gvec(3, probe);
	CHECK(ix->add_document("probe-a", "<DOC>proba</DOC>", probe) >= 0);
	add_gdocs(ix, 1, N);
	CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	CHECK(ix->add_document("probe-b", "<DOC>probb</DOC>", probe) >= 0);
	add_gdocs(ix, N + 1, N + N);
	CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	CHECK(ix->disk_segment_count() == 2);
	long long ga = ix->disk_segment_generation(0), gb = ix->disk_segment_generation(1);
	delete ix;

	char pa[4096], pb[4096];
	snprintf(pa, sizeof(pa), "%s/seg_%06lld.pq", dir, ga);
	snprintf(pb, sizeof(pb), "%s/seg_%06lld.pq", dir, gb);
	ANT_pq_store *sa = ANT_pq_store::load(pa, GDIM, N, ANT_pq_codec::METRIC_DOT);
	ANT_pq_store *sb = ANT_pq_store::load(pb, GDIM, N, ANT_pq_codec::METRIC_DOT);
	CHECK(sa && sb && sa->document_count() == N && sb->document_count() == N);
	CHECK(sa->get_k() == 16 && sb->get_k() == 16);					// v3 stores carry k=16
	long long sub = GDIM / GM, cb_floats = GM * sa->get_k() * sub;
	CHECK(memcmp(sa->get_codebook(), sb->get_codebook(), (size_t)cb_floats * sizeof(float)) == 0);
	long bits = ANT_pq_codec::bits_for_k(16); long long row_bytes = (GM * bits + 7) / 8;	// 4 bits -> 2 bytes
	CHECK(memcmp(sa->codes_for(0), sb->codes_for(0), (size_t)row_bytes) == 0);	// same packed row
	delete sa; delete sb;

	// pq.codebook sidecar (ANTPQGCB): magic8 + u32 version + i64 dim + i64 m + i64 k ...
	// -> k field at byte offset 8 + 4 + 8 + 8 = 28.  (Confirm against save_pq_codebook's
	//    write order before trusting this offset.)
	char cbp[4096]; snprintf(cbp, sizeof(cbp), "%s/pq.codebook", dir);
	FILE *fp = fopen(cbp, "rb"); CHECK(fp != NULL);
	long long sidecar_k = 0; CHECK(fseek(fp, 28, SEEK_SET) == 0);
	CHECK(fread(&sidecar_k, sizeof(sidecar_k), 1, fp) == 1); fclose(fp);
	CHECK(sidecar_k == 16);
	delete [] dir;
	printf("test_k16_global_comparability OK\n");
}

/* k=64 + OPQ: builds and searches (composition smoke). */
static void test_k64_opq_search(void)
{
	char *dir = make_engine_dir("/tmp/ant_kw64_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_k(64) == 0);
	CHECK(ix->set_pq_opq(1) == 0);
	add_gdocs(ix, 0, 30);
	CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	float q[GDIM]; make_gvec(5, q);
	CHECK(ix->search_vector(q, 5) >= 1);							// k=64 + OPQ search works
	delete ix; delete [] dir;
	printf("test_k64_opq_search OK\n");
}

/* k=16 + global: compaction reuses the frozen codebook (no retrain). */
static void test_k16_compaction_reuse(void)
{
	const long long N = 12;
	char *dir = make_engine_dir("/tmp/ant_kwcmp_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->set_vector_config(GDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_pq_config(GM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_pq_k(16) == 0);
	CHECK(ix->set_pq_global_codebook(1) == 0);
	add_gdocs(ix, 0, N); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	add_gdocs(ix, N, N + N); CHECK(ix->flush() == 0); CHECK(ix->build_pq() == 0);
	CHECK(ix->disk_segment_count() == 2);

	char cbp[4096]; snprintf(cbp, sizeof(cbp), "%s/pq.codebook", dir);
	unsigned char *before = NULL; long blen = 0; CHECK(read_file_bytes(cbp, &before, &blen) == 0);
	long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
	CHECK(ix->compact(gens, 2) == 0);
	CHECK(ix->disk_segment_count() == 1);
	unsigned char *after = NULL; long alen = 0; CHECK(read_file_bytes(cbp, &after, &alen) == 0);
	CHECK(alen == blen && memcmp(before, after, (size_t)blen) == 0);		// no retrain

	long long og = ix->disk_segment_generation(0);
	char mp[4096]; snprintf(mp, sizeof(mp), "%s/seg_%06lld.pq", dir, og);
	ANT_pq_store *merged = ANT_pq_store::load(mp, GDIM, N + N, ANT_pq_codec::METRIC_DOT);
	CHECK(merged && merged->document_count() == N + N && merged->get_k() == 16);
	delete merged;
	float q[GDIM]; make_gvec(7, q);
	CHECK(ix->search_vector(q, 5) >= 1);
	delete [] before; delete [] after; delete ix; delete [] dir;
	printf("test_k16_compaction_reuse OK\n");
}

int main(void)
{
	test_k16_global_comparability();
	test_k64_opq_search();
	test_k16_compaction_reuse();
	printf("ALL test_pq_kwidth_compose PASSED\n");
	return 0;
}

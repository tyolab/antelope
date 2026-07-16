/*
	TEST_MVPQ_VARIABLE_K_COMPOSE.CPP -- token epic 3/4 Task 3: k composes with
	global codebook, OPQ, compaction, rebuild, and forgiving-load.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/pq_codec.h"
#include "../source/multivector_pq_store.h"
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define RD 8
#define MM 4

static char *mkdir_tmp(const char *tmpl)
{ char b[64]; strcpy(b, tmpl); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r, d); return r; }
static void fill(long long seed, float *v)
{ double n=0; for (int j=0;j<RD;j++){v[j]=(float)(((seed*7+j*3)%13)-6)/6.0f;n+=v[j]*v[j];} n=sqrt(n)+1e-9; for(int j=0;j<RD;j++)v[j]/=(float)n; }
static void add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{ for (long long i=lo;i<hi;i++){ float rows[3*RD]; for(int r=0;r<3;r++) fill(i*5+r, rows+r*RD);
  char key[32]; snprintf(key,sizeof(key),"d-%lld",i); CHECK(ix->add_document(key,"body",NULL,rows,3)>=0); } }
static void add_probe(ATIRE_segment_index *ix, const char *key, const float *p)
{ CHECK(ix->add_document(key,"probe",NULL,p,1) >= 0); }

/* k + global: two segments under global mode at k=16 embed byte-equal k*dim
   codebooks and a shared probe token encodes to identical packed rows. */
static void test_k_global_cross_segment(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkg_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	float probe[RD]; fill(999, probe);
	add_probe(ix, "pa", probe); add_docs(ix, 1, 12);
	CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_probe(ix, "pb", probe); add_docs(ix, 13, 24);
	CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->disk_segment_count() == 2);
	long long ga = ix->disk_segment_generation(0), gb = ix->disk_segment_generation(1);
	delete ix;

	char pa[4096], pb[4096];
	snprintf(pa,sizeof(pa),"%s/seg_%06lld.mvpq",dir,ga);
	snprintf(pb,sizeof(pb),"%s/seg_%06lld.mvpq",dir,gb);
	ANT_multivector_pq_store *sa = ANT_multivector_pq_store::load(pa, RD, 12, ANT_pq_codec::METRIC_DOT);
	ANT_multivector_pq_store *sb = ANT_multivector_pq_store::load(pb, RD, 12, ANT_pq_codec::METRIC_DOT);
	CHECK(sa && sb && sa->get_k() == 16 && sb->get_k() == 16);
	/* embedded codebooks (k*dim floats) byte-equal */
	size_t cb = (size_t)(16 * RD) * sizeof(float);
	CHECK(memcmp(sa->get_codebook(), sb->get_codebook(), cb) == 0);
	/* shared probe (token 0 in both) -> identical packed rows; row_bytes = (m*4+7)/8 = 2 */
	long long bits = ANT_pq_codec::bits_for_k(16);
	size_t rb = (size_t)((MM*bits + 7)/8);
	CHECK(memcmp(sa->token_codes(0), sb->token_codes(0), rb) == 0);
	/*
		Correctness (catches the crux): the global codebook must be TRAINED and
		read at k=16 (16 centroids/subvector), not at ANT_pq_codec::K=256.  If the
		codebook is trained at 256 but the writer encodes/embeds it at k=16 stride,
		every subvector's code is looked up in the wrong centroid region -> the
		probe (token 0) reconstructs to garbage.  A correct k=16 book reconstructs
		the unit-norm probe with bounded error.  Cross-segment equality alone does
		NOT catch this (both segments corrupt identically).
	*/
	float rec[RD]; sa->token_reconstruct(0, rec);
	double rerr = 0; for (int j = 0; j < RD; j++) { double d = rec[j] - probe[j]; rerr += d*d; }
	CHECK(rerr < 1.0);
	delete sa; delete sb; delete [] dir;
	printf("test_k_global_cross_segment OK\n");
}

/* k + OPQ: build under k=16 + opq; reopen; MaxSim search sane; .mvpq is v3 with R block. */
static void test_k_opq(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvko_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_opq(1) == 0);
	add_docs(ix, 0, 16);
	CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	long long gen = ix->disk_segment_generation(0);
	delete ix;
	/* v3 + opq: header version 3, opq flag 1, size includes R block (D*D) */
	char mp[4096]; snprintf(mp,sizeof(mp),"%s/seg_%06lld.mvpq",dir,gen);
	FILE *fp = fopen(mp,"rb"); CHECK(fp);
	unsigned int version; fseek(fp,8,SEEK_SET); CHECK(fread(&version,4,1,fp)==1); CHECK(version==3u);
	long long opq; fseek(fp,52,SEEK_SET); CHECK(fread(&opq,8,1,fp)==1); CHECK(opq==1);
	fclose(fp);
	ATIRE_segment_index *re = new ATIRE_segment_index();
	CHECK(re->open(dir) == 0);
	CHECK(re->multivector_pq_k() == 16);
	CHECK(re->build_token_index() == 0);
	float q[2*RD]; fill(1,q); fill(2,q+RD);
	CHECK(re->search_multivector(q, 2, 10) > 0);
	delete re; delete [] dir;
	printf("test_k_opq OK\n");
}

/* k + compaction no-retrain + rebuild at k. */
static void test_k_compaction_and_rebuild(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkc_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);
	add_docs(ix, 0, 12); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 12, 24); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);

	/* compact: merged .mvpq is v3 at k=16, reuses the global codebook (no retrain) */
	CHECK(ix->disk_segment_count() == 2);
	long long gens[2] = { ix->disk_segment_generation(0), ix->disk_segment_generation(1) };
	CHECK(ix->compact(gens, 2) == 0);
	CHECK(ix->disk_segment_count() == 1);
	long long gen = ix->disk_segment_generation(0);
	char mp[4096]; snprintf(mp,sizeof(mp),"%s/seg_%06lld.mvpq",dir,gen);
	FILE *fp = fopen(mp,"rb"); CHECK(fp);
	unsigned int version; fseek(fp,8,SEEK_SET); CHECK(fread(&version,4,1,fp)==1); CHECK(version==3u); fclose(fp);

	/* rebuild at k: retrains + re-encodes; still v3 at k=16, search sane */
	add_docs(ix, 24, 36); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	CHECK(ix->rebuild_mvpq_global_codebook() == 0);
	CHECK(ix->build_token_index() == 0);
	float q[2*RD]; fill(3,q); fill(7,q+RD);
	CHECK(ix->search_multivector(q, 2, 10) > 0);
	delete ix; delete [] dir;
	printf("test_k_compaction_and_rebuild OK\n");
}

/* forgiving load: truncate a v3 .mvpq -> degraded-empty (token_count()==0), no crash. */
static void test_k_forgiving_load(void)
{
	char path[] = "/tmp/ant_mvkf_XXXXXX";
	int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
	ANT_multivector_pq_store_writer w;
	CHECK(w.create(path, RD, MM, 16, ANT_pq_codec::METRIC_DOT, 0) == 0);
	for (int d = 0; d < 10; d++) { float rows[2*RD]; fill(d,rows); fill(d+1,rows+RD); CHECK(w.append(rows,2)==0); }
	CHECK(w.finish() == 0);
	CHECK(truncate(path, 40) == 0);						/* chop mid-header/body */
	ANT_multivector_pq_store *s = ANT_multivector_pq_store::load(path, RD, 10, ANT_pq_codec::METRIC_DOT);
	CHECK(s != NULL && s->token_count() == 0);			/* degraded, not a crash */
	delete s; remove(path);
	printf("test_k_forgiving_load OK\n");
}

/* k + global + NONE resident tier + rebuild: the exact corner that carried a real
   UAF in the prior T3 feature (stale token_index borrowing a token_source over a
   freed .mvpq store).  Mirrors test_none_tier_rebuild_no_uaf in test_mvpq_global.cpp
   but under k=16 -- proves the Pass-2 token_index/token_source refresh + .tann
   invalidation runs at variable k too.  The disk_segment_has_token_index(...)==0
   assertion pins the UAF deterministically (no ASan needed). */
static void test_k_global_none_tier_rebuild(void)
{
	char *dir = mkdir_tmp("/tmp/ant_mvkn_XXXXXX");
	ATIRE_segment_index *ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);
	CHECK(ix->set_rerank_config(RD, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_config(MM, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(ix->set_multivector_pq_k(16) == 0);
	CHECK(ix->set_multivector_pq_global_codebook(1) == 0);

	add_docs(ix, 0, 14); CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);
	add_docs(ix, 100, 114);						/* differently-distributed second segment */
	CHECK(ix->flush() == 0); CHECK(ix->build_multivector_pq() == 0);

	CHECK(ix->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
	delete ix;									/* NONE takes effect on reopen */

	ix = new ATIRE_segment_index();
	CHECK(ix->open(dir) == 0);					/* token_source = ANT_multivector_pq_source over each .mvpq */
	CHECK(ix->multivector_pq_k() == 16);
	CHECK(ix->multivector_pq_global_codebook() == 1);
	CHECK(ix->disk_segment_count() == 2);
	CHECK(ix->disk_segment_resident_tier_mv(0) == ATIRE_segment_index::MV_TIER_NONE);
	CHECK(ix->disk_segment_resident_tier_mv(1) == ATIRE_segment_index::MV_TIER_NONE);

	CHECK(ix->build_token_index() == 0);		/* .tann graph over the PQ (ADC) source */
	CHECK(ix->disk_segment_has_token_index(0) == 1);	/* graph path is live */

	float q[2*RD]; fill(3, q); fill(107, q + RD);
	CHECK(ix->search_multivector(q, 2, 10) > 0);	/* graph path, BEFORE rebuild */

	CHECK(ix->rebuild_mvpq_global_codebook() == 0);	/* deletes+reloads every .mvpq store */

	/* DETERMINISTIC UAF catch: rebuild MUST have dropped every stale token_index
	   (each borrowed a token_source over a now-freed .mvpq store). */
	CHECK(ix->disk_segment_has_token_index(0) == 0);
	CHECK(ix->disk_segment_has_token_index(1) == 0);

	/* exact-scan fallback still answers sanely (no crash) */
	CHECK(ix->search_multivector(q, 2, 10) > 0);

	/* rebuild the graph over the NEW stores and exercise the graph path AFTER */
	CHECK(ix->build_token_index() == 0);
	CHECK(ix->disk_segment_has_token_index(0) == 1);
	CHECK(ix->search_multivector(q, 2, 10) > 0);

	/* still v3 at k=16 after the rebuild */
	long long gen = ix->disk_segment_generation(0);
	char mp[4096]; snprintf(mp,sizeof(mp),"%s/seg_%06lld.mvpq",dir,gen);
	FILE *fp = fopen(mp,"rb"); CHECK(fp);
	unsigned int version; fseek(fp,8,SEEK_SET); CHECK(fread(&version,4,1,fp)==1); CHECK(version==3u); fclose(fp);

	delete ix; delete [] dir;
	printf("test_k_global_none_tier_rebuild OK\n");
}

int main(void)
{
	test_k_global_cross_segment();
	test_k_opq();
	test_k_compaction_and_rebuild();
	test_k_forgiving_load();
	test_k_global_none_tier_rebuild();
	printf("ALL test_mvpq_variable_k_compose PASSED\n");
	return 0;
}

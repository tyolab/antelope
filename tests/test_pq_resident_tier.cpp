#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include "../atire/atire_segment_index.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)

static const char *DIR = "/tmp/test_pq_resident_tier_idx";

static int file_exists(const char *dir, long long generation, const char *ext)
{
	char path[2048]; struct stat st;
	/* segment filenames are seg_<generation, zero-padded to 6 digits>.<ext> (see ATIRE_segment_index::segment_filename) */
	snprintf(path, sizeof(path), "%s/seg_%06lld.%s", dir, (long long)generation, ext);
	return stat(path, &st) == 0;
}

/* Index ~24 dense docs (dim 16), flush, build_pq; return the flushed generation via an out-param. */
static ATIRE_segment_index *build_indexed(long posture, long tier, long long *gen_out)
{
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_pq_config(4, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_pq_resident_tier(tier) == 0);
	float v[16];
	for (int d = 0; d < 24; d++)
		{
		char name[64]; snprintf(name, sizeof(name), "doc%d", d);
		for (int j = 0; j < 16; j++) v[j] = (float)((d*7 + j*3) % 11) / 10.0f;
		CHECK(idx->add_document(name, "body words here", v) >= 0);
		}
	CHECK(idx->flush() == 0);
	*gen_out = idx->disk_segment_generation(0);   /* atire/atire_segment_index.h -> segments[0].generation */
	CHECK(idx->build_pq() == 0);
	return idx;
}

static void test_pqr_built_under_int8(void)
{
	long long gen;
	ATIRE_segment_index *idx = build_indexed(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_INT8, &gen);
	CHECK(idx->disk_segment_has_pq(0) == 1);
	CHECK(file_exists(DIR, gen, "pqr"));      /* int8 rerank sidecar written */
	CHECK(file_exists(DIR, gen, "vec"));      /* float .vec still on disk (never removed) */
	delete idx;
}

static void test_pqr_absent_under_float(void)
{
	long long gen;
	ATIRE_segment_index *idx = build_indexed(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_FLOAT, &gen);
	CHECK(idx->disk_segment_has_pq(0) == 1);
	CHECK(!file_exists(DIR, gen, "pqr"));      /* FLOAT tier writes no .pqr */
	delete idx;
}

static ATIRE_segment_index *fresh(long posture)
{
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);   // BEFORE open()
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_pq_config(4, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);   // POST open()
	return idx;
}

static void test_default_is_float(void)
{
	ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_REPLACE);
	CHECK(idx->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_FLOAT);
	delete idx;
}

static void test_set_and_immutable(void)
{
	ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_REPLACE);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);   // idempotent same
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_NONE) != 0);   // different -> reject
	CHECK(idx->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_INT8);
	delete idx;
}

static void test_none_rejects_rerank(void)
{
	ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_RERANK);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_NONE) != 0);   // NONE + rerank rejected
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);
	delete idx;
}

static void test_invalid_and_unconfigured(void)
{
	ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_REPLACE);
	CHECK(idx->set_pq_resident_tier(7) != 0);              // invalid tier
	delete idx;
	// PQ unconfigured: open + vectors but no set_pq_config
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx2 = new ATIRE_segment_index();
	CHECK(idx2->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);   // BEFORE open()
	CHECK(idx2->open(DIR) == 0);
	CHECK(idx2->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) != 0);   // PQ not configured
	delete idx2;
}

static void test_persist_and_backcompat(void)
{
	{
		ATIRE_segment_index *idx = fresh(ATIRE_segment_index::PQ_POSTURE_REPLACE);
		CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);
		delete idx;
	}
	// reopen -> tier restored from pq.config v2
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->pq_configured());
	CHECK(idx->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_INT8);
	delete idx;

	// Phase-1 pq.config (v1, no tier field) loads as FLOAT: rewrite the file as a v1 record.
	char path[2048]; snprintf(path, sizeof(path), "%s/pq.config", DIR);
	FILE *fp = fopen(path, "wb");
	unsigned long long magic; memcpy(&magic, "ANTPQCF1", 8);
	unsigned int version = 1u; long long m = 4, posture = 0, rq = 0;
	fwrite(&magic, sizeof(magic), 1, fp); fwrite(&version, sizeof(version), 1, fp);
	fwrite(&m, sizeof(m), 1, fp); fwrite(&posture, sizeof(posture), 1, fp); fwrite(&rq, sizeof(rq), 1, fp);
	fclose(fp);
	ATIRE_segment_index *idx2 = new ATIRE_segment_index();
	CHECK(idx2->open(DIR) == 0);
	CHECK(idx2->pq_configured());
	CHECK(idx2->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_FLOAT);   // absent field -> FLOAT
	delete idx2;
}

/* Returns recall@10 of idx->search_vector(q,10) against the planted-nearest set `planted` (size np). */
static double recall_at_10(ATIRE_segment_index *idx, const float *q, const long *planted, int np)
{
	long long n = idx->search_vector(q, 10);   // search_vector returns the result count
	int hit = 0;
	for (int i = 0; i < np; i++)
		{
		char want[64]; snprintf(want, sizeof(want), "doc%ld", planted[i]);
		for (long long h = 0; h < n && h < 10; h++)
			if (strcmp(idx->get_hit(h)->filename, want) == 0) { hit++; break; }
		}
	return (double)hit / np;
}

static void test_rerank_through_int8_tier(void)
{
	long long gen_int8, gen_float;
	long planted[1] = {10};
	float q[16];
	for (int j = 0; j < 16; j++) q[j] = (float)((10*7 + j*3) % 11) / 10.0f;

	/* INT8-tier rerank index (build_indexed wipes DIR, so do the whole int8 lifecycle -- including
	   the .pqr-absent reopen below -- before switching DIR over to the FLOAT reference build). */
	ATIRE_segment_index *idx8 = build_indexed(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_INT8, &gen_int8);
	delete idx8;
	idx8 = new ATIRE_segment_index();
	CHECK(idx8->open(DIR) == 0);
	CHECK(idx8->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_INT8);
	double recall_int8 = recall_at_10(idx8, q, planted, 1);
	delete idx8;

	/* .pqr-absent path: delete the int8 rerank sidecar, reopen, rerank still returns a sane top-k */
	char pqr_path[2048]; snprintf(pqr_path, sizeof(pqr_path), "%s/seg_%06lld.pqr", DIR, (long long)gen_int8);
	CHECK(remove(pqr_path) == 0);

	ATIRE_segment_index *idx8b = new ATIRE_segment_index();
	CHECK(idx8b->open(DIR) == 0);
	CHECK(idx8b->pq_resident_tier() == ATIRE_segment_index::PQ_TIER_INT8);   // configured tier unchanged; loaded store just absent
	long long n = idx8b->search_vector(q, 10);
	CHECK(n >= 1);
	CHECK(idx8b->get_hit(0)->filename[0] == 'd');   // top hit is a real doc, no crash
	delete idx8b;

	/* FLOAT-tier rerank reference index, same synthetic docs (rebuilds DIR from scratch) */
	ATIRE_segment_index *idxf = build_indexed(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_FLOAT, &gen_float);
	double recall_float = recall_at_10(idxf, q, planted, 1);
	delete idxf;

	printf("recall_at_10: int8=%.3f float=%.3f\n", recall_int8, recall_float);
	CHECK(recall_int8 <= recall_float + 1e-9);   // float is the precision ceiling
	CHECK(recall_int8 >= 0.5);                    // sane floor for this tiny synthetic set
}

static void test_resident_tier_after_reopen(long tier, long posture)
{
	long long gen;
	{ ATIRE_segment_index *idx = build_indexed(posture, tier, &gen); delete idx; }   // close
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->pq_resident_tier() == tier);
	CHECK(idx->disk_segment_has_pq(0) == 1);
	CHECK(idx->disk_segment_resident_tier(0) == tier);   // FLOAT->float store, INT8->int8 .pqr, NONE->NULL
	delete idx;
}

static void test_compaction_rebuilds_pqr(void)
{
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DIR, DIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(idx->open(DIR) == 0);
	CHECK(idx->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
	CHECK(idx->set_pq_resident_tier(ATIRE_segment_index::PQ_TIER_INT8) == 0);
	float v[16];
	for (int d = 0; d < 12; d++)
		{ char name[64]; snprintf(name,sizeof(name),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%11)/10.0f; CHECK(idx->add_document(name,"body words here",v)>=0); }
	CHECK(idx->flush() == 0);
	for (int d = 12; d < 24; d++)
		{ char name[64]; snprintf(name,sizeof(name),"doc%d",d); for(int j=0;j<16;j++) v[j]=(float)((d*7+j*3)%11)/10.0f; CHECK(idx->add_document(name,"body words here",v)>=0); }
	CHECK(idx->flush() == 0);
	CHECK(idx->build_pq() == 0);

	float q[16]; for(int j=0;j<16;j++) q[j]=(float)((10*7+j*3)%11)/10.0f;   // query near doc10
	CHECK(idx->search_vector(q, 10) >= 1);

	long long gens[2] = { idx->disk_segment_generation(0), idx->disk_segment_generation(1) };
	CHECK(idx->compact(gens, 2) == 0);

	CHECK(idx->disk_segment_has_pq(0) == 1);
	CHECK(idx->disk_segment_resident_tier(0) == ATIRE_segment_index::PQ_TIER_INT8);   // merged .pqr rebuilt + resident
	long long n = idx->search_vector(q, 10);
	CHECK(n >= 1);
	int found10 = 0;
	for (long long h = 0; h < n && h < 10; h++)
		if (strcmp(idx->get_hit(h)->filename, "doc10") == 0) found10 = 1;
	CHECK(found10);   // planted doc still found after merge (renumbered codes + int8 tier correct)
	delete idx;
}

/*
	Recall sanity + FLOAT-default byte-identical lock (Task 6).
	Second scratch dir (dim 32, DEFAULT m via set_pq_config(0,...)) so this doesn't collide
	with the dim-16 fixtures above, which reuse DIR across many tests.
*/
#define RTDIM 32
#define RTNDOCS 200
static const char *RTDIR = "/tmp/test_pq_resident_tier_recall_idx";

static unsigned long long rt_rng;
static double rt_nextf(void) { rt_rng = rt_rng * 6364136223846793005ULL + 1442695040888963407ULL; return (double)((rt_rng >> 33) & 0x7fffffff) / (double)0x7fffffff; }

static void rt_unit_vec(long long seed, float *v)
{
	rt_rng = (unsigned long long)(seed + 1) * 2654435761ULL;
	double norm = 0.0;
	for (int d = 0; d < RTDIM; d++) { v[d] = (float)(rt_nextf() * 2.0 - 1.0); norm += (double)v[d]*v[d]; }
	norm = sqrt(norm) + 1e-9;
	for (int d = 0; d < RTDIM; d++) v[d] = (float)(v[d]/norm);
}

/* fixed query direction */
static void rt_query_vec(float *v)
{
	for (int d = 0; d < RTDIM; d++) v[d] = 0.0f;
	v[0] = v[1] = v[2] = 0.577f;
}

/* planted doc: q + tiny noise, re-normalized -- large margin over random docs so a correct
   implementation reliably gets recall 1.0 float / >=0.9 int8 */
static void rt_planted_vec(long long k, float *v)
{
	rt_query_vec(v);
	rt_rng = (unsigned long long)(90000 + k) * 2654435761ULL;
	double norm = 0.0;
	for (int d = 0; d < RTDIM; d++) { v[d] += (float)((rt_nextf()*2.0-1.0) * 0.001); norm += (double)v[d]*v[d]; }
	norm = sqrt(norm) + 1e-9;
	for (int d = 0; d < RTDIM; d++) v[d] = (float)(v[d]/norm);
}

static void rt_fill(ATIRE_segment_index *idx)
{
	float v[RTDIM]; char key[32], body[64];
	for (long long i = 0; i < RTNDOCS; i++)
		{ rt_unit_vec(i, v); snprintf(key, sizeof(key), "doc-%lld", i); snprintf(body, sizeof(body), "<DOC>term%lld body</DOC>", i); CHECK(idx->add_document(key, body, v) >= 0); }
	for (long long k = 0; k < 3; k++)
		{ rt_planted_vec(k, v); snprintf(key, sizeof(key), "planted-%lld", k); snprintf(body, sizeof(body), "<DOC>plantterm%lld here</DOC>", k); CHECK(idx->add_document(key, body, v) >= 0); }
	CHECK(idx->flush() == 0);
}

static ATIRE_segment_index *rt_build(long posture, long tier, int explicit_tier)
{
	char cmd[2048]; snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", RTDIR, RTDIR); system(cmd);
	ATIRE_segment_index *idx = new ATIRE_segment_index();
	CHECK(idx->set_vector_config(RTDIM, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
	CHECK(idx->open(RTDIR) == 0);
	CHECK(idx->set_pq_config(0, posture, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);   /* DEFAULT m */
	if (explicit_tier) CHECK(idx->set_pq_resident_tier(tier) == 0);
	rt_fill(idx);
	CHECK(idx->build_pq() == 0);
	return idx;
}

static double rt_recall_planted(ATIRE_segment_index *idx, const float *q)
{
	long long n = idx->search_vector(q, 10);
	int hit = 0;
	for (int k = 0; k < 3; k++)
		{
		char want[32]; snprintf(want, sizeof(want), "planted-%d", k);
		for (long long h = 0; h < n && h < 10; h++)
			if (strcmp(idx->get_hit(h)->filename, want) == 0) { hit++; break; }
		}
	return (double)hit / 3.0;
}

static void test_recall_sanity_and_ceiling(void)
{
	float q[RTDIM]; rt_query_vec(q);

	ATIRE_segment_index *idxf = rt_build(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_FLOAT, 1);
	double recall_float = rt_recall_planted(idxf, q);
	delete idxf;

	ATIRE_segment_index *idx8 = rt_build(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_INT8, 1);
	double recall_int8 = rt_recall_planted(idx8, q);
	delete idx8;

	ATIRE_segment_index *idxn = rt_build(ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::PQ_TIER_NONE, 1);
	double recall_replace = rt_recall_planted(idxn, q);
	delete idxn;

	printf("recall_at_10 (planted=3, dim=32, m=default): float_rerank=%.3f int8_rerank=%.3f replace_adc=%.3f\n",
		recall_float, recall_int8, recall_replace);

	CHECK(recall_int8 >= recall_replace - 1e-9);   /* int8 rescore at least as good as raw ADC */
	CHECK(recall_int8 <= recall_float + 1e-9);      /* float is the precision ceiling */
	CHECK(recall_int8 >= 0.9);
	CHECK(recall_float == 1.0);                     /* all 3 planted docs recalled under FLOAT-tier rerank */
}

struct rt_hit_rec { char filename[64]; long long docid; double score; };

static void rt_capture_top10(ATIRE_segment_index *idx, const float *q, rt_hit_rec *out, long long *n)
{
	long long c = idx->search_vector(q, 10);
	*n = c;
	for (long long i = 0; i < c; i++)
		{
		ATIRE_segment_index::hit *h = idx->get_hit(i);
		strncpy(out[i].filename, h->filename, sizeof(out[i].filename) - 1);
		out[i].filename[sizeof(out[i].filename) - 1] = 0;
		out[i].docid = h->docid;
		out[i].score = h->score;
		}
}

/* Index A: explicit set_pq_resident_tier(PQ_TIER_FLOAT). Index B: never calls set_pq_resident_tier
   (relies on the FLOAT default). Both PQ_POSTURE_RERANK over identical data + query. Results must
   be bit-identical -- FLOAT is the unchanged pre-feature (Phase-1) path. */
static void test_float_default_byte_identical(void)
{
	float q[RTDIM]; rt_query_vec(q);

	rt_hit_rec resA[10], resB[10]; long long nA = 0, nB = 0;

	ATIRE_segment_index *idxA = rt_build(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_FLOAT, 1);
	rt_capture_top10(idxA, q, resA, &nA);
	delete idxA;

	ATIRE_segment_index *idxB = rt_build(ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::PQ_TIER_FLOAT, 0);   /* never calls set_pq_resident_tier */
	rt_capture_top10(idxB, q, resB, &nB);
	delete idxB;

	CHECK(nA == nB);
	for (long long i = 0; i < nA; i++)
		{
		CHECK(strcmp(resA[i].filename, resB[i].filename) == 0);
		CHECK(resA[i].docid == resB[i].docid);
		CHECK(resA[i].score == resB[i].score);   /* exact == : FLOAT is bit-identical to the pre-feature path */
		}
	printf("byte-identical lock: %lld/%lld hits match exactly (FLOAT explicit vs default)\n", nA, nA);
}

int main(void)
{
	test_default_is_float();
	test_set_and_immutable();
	test_none_rejects_rerank();
	test_invalid_and_unconfigured();
	test_persist_and_backcompat();
	test_pqr_built_under_int8();
	test_pqr_absent_under_float();
	test_resident_tier_after_reopen(ATIRE_segment_index::PQ_TIER_FLOAT, ATIRE_segment_index::PQ_POSTURE_REPLACE);
	test_resident_tier_after_reopen(ATIRE_segment_index::PQ_TIER_INT8,  ATIRE_segment_index::PQ_POSTURE_RERANK);
	test_resident_tier_after_reopen(ATIRE_segment_index::PQ_TIER_NONE,  ATIRE_segment_index::PQ_POSTURE_REPLACE);
	test_rerank_through_int8_tier();
	test_compaction_rebuilds_pqr();
	test_recall_sanity_and_ceiling();
	test_float_default_byte_identical();
	printf("test_pq_resident_tier PASSED\n");
	return 0;
}

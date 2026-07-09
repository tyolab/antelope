#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../atire/atire_segment_index.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1);} } while(0)

static const char *DIR = "/tmp/test_pq_resident_tier_idx";

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

int main(void)
{
	test_default_is_float();
	test_set_and_immutable();
	test_none_rejects_rerank();
	test_invalid_and_unconfigured();
	test_persist_and_backcompat();
	printf("test_pq_resident_tier PASSED\n");
	return 0;
}

/*
	TEST_PQ_CONFIG.CPP
	------------------
	Task 4 of the PQ dense plan: set_pq_config()/pq.config persistence and
	mutual exclusion with V4 int8 quantization (set_quantization()).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static char *make_index_dir(void)
{
char buffer[64];
strcpy(buffer, "/tmp/ant_pqconfig_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL)
	exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1];
strcpy(result, dir);
return result;
}

/*
	TEST_BASIC_SET_AND_IDEMPOTENT_AND_IMMUTABLE()
	----------------------------------------------
	set_pq_config() succeeds, is idempotent for the same (m, posture,
	rerank_quant), and rejects a different m once set.
*/
static void test_basic_set_and_idempotent_and_immutable(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);

CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);	/* idempotent, same config */
CHECK(ix->set_pq_config(8, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);	/* different m: rejected (immutable) */
CHECK(ix->pq_configured());
CHECK(ix->pq_m() == 4);

delete ix;
delete [] dir;
printf("test_basic_set_and_idempotent_and_immutable OK\n");
}

/*
	TEST_MUTUAL_EXCLUSION_QUANTIZATION_FIRST()
	--------------------------------------------
	Once V4 int8 quantization is enabled, set_pq_config() must be rejected.
*/
static void test_mutual_exclusion_quantization_first(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);

CHECK(ix->set_quantization(ATIRE_segment_index::QUANTIZE_REPLACE) == 0);
CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
CHECK(!ix->pq_configured());

delete ix;
delete [] dir;
printf("test_mutual_exclusion_quantization_first OK\n");
}

/*
	TEST_MUTUAL_EXCLUSION_PQ_FIRST()
	----------------------------------
	Once PQ is enabled, set_quantization() must be rejected.
*/
static void test_mutual_exclusion_pq_first(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);

CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_quantization(ATIRE_segment_index::QUANTIZE_REPLACE) != 0);
CHECK(ix->quantization_mode() == ATIRE_segment_index::QUANTIZE_OFF);

delete ix;
delete [] dir;
printf("test_mutual_exclusion_pq_first OK\n");
}

/*
	TEST_M_MUST_DIVIDE_DIMENSION()
	--------------------------------
*/
static void test_m_must_divide_dimension(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);

CHECK(ix->set_pq_config(3, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);	/* 3 does not divide 16 */
CHECK(!ix->pq_configured());

delete ix;
delete [] dir;
printf("test_m_must_divide_dimension OK\n");
}

/*
	TEST_DEFAULT_M_RULE()
	-----------------------
	m == 0 => default_pq_m(dimension): largest divisor of dimension that is
	<= 16.  For dimension 16, that is 16 itself.
*/
static void test_default_m_rule(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);

CHECK(ix->set_pq_config(0, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->pq_m() == 16);

delete ix;
delete [] dir;
printf("test_default_m_rule OK\n");
}

/*
	TEST_REQUIRES_VECTORS_CONFIGURED()
	-------------------------------------
	set_pq_config() must fail on an index opened without vectors enabled.
*/
static void test_requires_vectors_configured(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);		/* no set_vector_config() */

CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
CHECK(!ix->pq_configured());

delete ix;
delete [] dir;
printf("test_requires_vectors_configured OK\n");
}

/*
	TEST_PQ_CONFIG_PERSISTS()
	----------------------------
	set_pq_config() persists across close/reopen.
*/
static void test_pq_config_persists(void)
{
char *dir = make_index_dir();
ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(a->open(dir) == 0);
CHECK(a->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
delete a;

ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(b->open(dir) == 0);
CHECK(b->pq_configured());
CHECK(b->pq_m() == 4);
delete b;

delete [] dir;
printf("test_pq_config_persists OK\n");
}

/*
	TEST_POSTURE_QUANT_PERSIST()
	-------------------------------
	set_pq_config()'s posture and rerank_quant (not just m) persist across
	close/reopen via load_pq_config().
*/
static void test_posture_quant_persist(void)
{
char dir[64]; strcpy(dir, "/tmp/ant_pqcfg2_XXXXXX"); { char *d = mkdtemp(dir); CHECK(d != NULL); }
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);
CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
delete ix;						/* close; pq.config persisted */

ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(16, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(ix->open(dir) == 0);				/* load_pq_config restores posture+quant */
CHECK(ix->pq_configured());
CHECK(ix->pq_m() == 4);
CHECK(ix->pq_posture() == ATIRE_segment_index::PQ_POSTURE_RERANK);
CHECK(ix->pq_rerank_quant() == ATIRE_segment_index::RERANK_QUANT_INT8);
delete ix;
printf("test_posture_quant_persist OK\n");
}

int main(void)
{
test_basic_set_and_idempotent_and_immutable();
test_mutual_exclusion_quantization_first();
test_mutual_exclusion_pq_first();
test_m_must_divide_dimension();
test_default_m_rule();
test_requires_vectors_configured();
test_pq_config_persists();
test_posture_quant_persist();
printf("test_pq_config PASSED\n");
return 0;
}

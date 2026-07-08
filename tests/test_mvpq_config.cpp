/*
	TEST_MVPQ_CONFIG.CPP -- set_multivector_pq_config()/persistence/mutual exclusion
	with the .mvec int8 mode (set_rerank_config's RERANK_QUANT_INT8).

	Note: set_rerank_config() requires the index to already be open() (it has no
	before-open "pending" mechanism, unlike set_vector_config()), so every call
	below is sequenced open() then set_rerank_config().
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char *dir_(void) { char b[64]; strcpy(b, "/tmp/ant_mvpqcfg_XXXXXX"); char *d = mkdtemp(b); if (!d) exit(1); char *r = new char[strlen(d)+1]; strcpy(r,d); return r; }

int main(void)
{
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(8, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
CHECK(ix->multivector_pq_configured()); CHECK(ix->multivector_pq_m() == 4);
delete ix; delete [] d;
}
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(3, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
CHECK(!ix->multivector_pq_configured());
delete ix; delete [] d;
}
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
delete ix; delete [] d;
}
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
CHECK(ix->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) != 0);
CHECK(!ix->multivector_pq_configured());
delete ix; delete [] d;
}
{
char *d = dir_(); ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(d) == 0);
CHECK(ix->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(0, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->multivector_pq_m() == 16);
delete ix; delete [] d;
}
{
char *d = dir_(); ATIRE_segment_index *a = new ATIRE_segment_index();
CHECK(a->open(d) == 0);
CHECK(a->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(a->set_multivector_pq_config(4, ATIRE_segment_index::PQ_POSTURE_RERANK, ATIRE_segment_index::RERANK_QUANT_INT8) == 0);
delete a;
ATIRE_segment_index *b = new ATIRE_segment_index();
CHECK(b->open(d) == 0);
CHECK(b->set_rerank_config(16, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(b->multivector_pq_configured()); CHECK(b->multivector_pq_m() == 4);
delete b; delete [] d;
}
printf("test_mvpq_config PASSED\n");
return 0;
}

/*
	TEST_MVPQ_OPQ_COMPOSE.CPP -- token .mvpq OPQ rotation (token epic, Task 3):
	composition + recall.  Proves:

	(a) test_opq_recall_ge_non_opq -- on genuinely anisotropic multi-vector data
	    (correlated variance packed into the first half of the dims so the
	    axis-aligned m=2 subvector split wastes the second subvector), OPQ MaxSim
	    recall@k -- vs exact float MaxSim over the resident .mvec -- is >= the
	    non-OPQ recall.  Both PQ indices score via the ADC MaxSim path (REPLACE
	    posture, pqs->maxsim), so the only difference is the learned rotation;
	    the assertion is on the DIRECTION (recall_opq >= recall_noopq), not a
	    fixed number, mirroring the #22.1 dense OPQ fixture discipline.

	(b) test_opq_composes_with_token_graph -- OPQ + the #24 PQ-backed token graph
	    (set_multivector_pq_config REPLACE + build_token_index): scoring flows
	    through token_score_prepared on the ROTATED prepared table; a planted
	    exact-match doc ranks #1 and nothing crashes.

	(c) test_opq_none_tier_reconstruct -- OPQ + MV_TIER_NONE: the float pool is
	    dropped and search reconstructs tokens from PQ codes, un-rotating via R^T
	    (token_reconstruct); the planted doc is still found and search is sane.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../atire/atire_segment_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

#define DIM   16			/* rerank dimension */
#define PQ_M  2				/* 2 subvectors of 8 dims each -- lossy enough for OPQ headroom */

/* ---- deterministic, per-(index,salt) independent hashed uniforms (mirrors #22.1) ---- */
static unsigned long long h64(unsigned long long x)
{
x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
x ^= x >> 27; x *= 0x94D049BB133111EBULL;
x ^= x >> 31;
return x;
}
static double randu(long long i, long long salt)	/* ~uniform [-1,1), independent per (i,salt) */
{
unsigned long long h = h64((unsigned long long)(i * 1000003LL + salt) * 0x9E3779B97F4A7C15ULL);
return ((double)(h % 2000003ULL) / 2000003.0) * 2.0 - 1.0;
}

/*
	High-intrinsic-dimension anisotropic token generator.  Dims 0..7 each carry an
	INDEPENDENT high-variance component (a mild decreasing scale adds anisotropy);
	dims 8..15 carry only tiny noise.  With m=2 the axis-aligned split crams all 8
	independent high-variance dims into subvector 0 -- far more intrinsic dimensions
	than 256 centroids can resolve jointly (~1.9 levels/dim) -- while subvector 1
	(dims 8..15) wastes its 256-centroid budget on near-zero noise.  OPQ's orthogonal
	rotation spreads the 8 active dims across BOTH subvectors (~4 each -> ~4 levels/dim),
	so its PQ reconstruction (and thus ADC-MaxSim recall) beats the un-rotated split.
	add_document normalizes each row, so the anisotropy lives in the DIRECTION
	distribution over the first-8-dims subsphere.
*/
static void make_token(long long tokseed, float *v)
{
/* mild anisotropy: independent scales gently decreasing across the 8 active dims,
   all far above the dim 8..15 noise floor so intrinsic dimensionality stays high. */
static const double scale[8] = { 1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3 };
for (int d = 0; d < 8; d++)
	v[d] = (float)(randu(tokseed, 1 + d) * scale[d]);	/* independent, high-variance */
for (int d = 8; d < DIM; d++)
	v[d] = (float)(randu(tokseed, 200 + d) * 0.01);	/* tiny noise in the second subvector */
}

/* fill an index with NDOCS docs of 1..3 anisotropic tokens each; token seeds are
   deterministic and identical across indices so all three see the SAME data. */
static void fill(ATIRE_segment_index *ix, long long ndocs)
{
float doc[3 * DIM];
char key[32], body[64];
long long tokseed = 0;
for (long long i = 0; i < ndocs; i++)
	{
	long long md = (i % 3) + 1;			/* 1..3 tokens */
	for (long long t = 0; t < md; t++)
		make_token(tokseed++, doc + t * DIM);
	sprintf(key, "doc-%lld", i);
	sprintf(body, "<DOC>term%lld z</DOC>", i);
	CHECK(ix->add_document(key, body, NULL, doc, md) >= 0);
	}
CHECK(ix->flush() == 0);
}

static char *make_dir(const char *prefix)
{
char buffer[64]; strcpy(buffer, prefix); strcat(buffer, "_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *r = new char[strlen(dir) + 1]; strcpy(r, dir); return r;
}

/* recall@k of a candidate index vs the exact float-MaxSim ground truth, averaged
   over nq held-out anisotropic queries (1..3 tokens, seeds disjoint from the corpus). */
static double recall_at_k(ATIRE_segment_index *cand, ATIRE_segment_index *ex, long long nq, long long k)
{
long long hit = 0, tot = 0;
float q[3 * DIM];
for (long long qi = 0; qi < nq; qi++)
	{
	long long mq = (qi % 3) + 1;
	for (long long t = 0; t < mq; t++)
		make_token(500000 + qi * 3 + t, q + t * DIM);	/* held out */

	long long en = ex->search_multivector(q, mq, k);
	long long e[16]; long long enn = en < k ? en : k;
	for (long long i = 0; i < enn; i++) e[i] = atoll(ex->get_hit(i)->filename + 4);

	long long cn = cand->search_multivector(q, mq, k);
	long long c[16]; long long cnn = cn < k ? cn : k;
	for (long long i = 0; i < cnn; i++) c[i] = atoll(cand->get_hit(i)->filename + 4);

	for (long long a = 0; a < enn; a++)
		{
		tot++;
		for (long long bb = 0; bb < cnn; bb++)
			if (e[a] == c[bb]) { hit++; break; }
		}
	}
return tot ? (double)hit / (double)tot : 1.0;
}

/*
	(a) OPQ MaxSim recall >= non-OPQ recall on anisotropic multi-vector data.
	Three indices over IDENTICAL data: exact float (ground truth), PQ-no-OPQ,
	PQ-OPQ, both PQ indices in REPLACE posture so search scores via ADC MaxSim
	(pqs->maxsim over the PQ codes).  No token index is built, so the ADC scan is
	exhaustive -- isolating pure PQ quantization error, which is exactly what the
	rotation improves.
*/
static void test_opq_recall_ge_non_opq(void)
{
const long long NDOCS = 400, NQ = 50, K = 10;

/* exact float ground truth: rerank only, no token-PQ */
char *de = make_dir("/tmp/ant_mvopq_gt");
ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->open(de) == 0);
CHECK(ex->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
fill(ex, NDOCS);

/* PQ, no OPQ (REPLACE posture) */
char *dn = make_dir("/tmp/ant_mvopq_no");
ATIRE_segment_index *pq_no = new ATIRE_segment_index();
CHECK(pq_no->open(dn) == 0);
CHECK(pq_no->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(pq_no->set_multivector_pq_config(PQ_M, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(pq_no->multivector_pq_opq() == 0);		/* default off */
fill(pq_no, NDOCS);
CHECK(pq_no->build_multivector_pq() == 0);

/* PQ, OPQ enabled (POST set_multivector_pq_config) */
char *dq = make_dir("/tmp/ant_mvopq_on");
ATIRE_segment_index *pq_opq = new ATIRE_segment_index();
CHECK(pq_opq->open(dq) == 0);
CHECK(pq_opq->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(pq_opq->set_multivector_pq_config(PQ_M, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(pq_opq->set_multivector_pq_opq(1) == 0);
CHECK(pq_opq->multivector_pq_opq() == 1);
fill(pq_opq, NDOCS);
CHECK(pq_opq->build_multivector_pq() == 0);

double recall_noopq = recall_at_k(pq_no,  ex, NQ, K);
double recall_opq   = recall_at_k(pq_opq, ex, NQ, K);
printf("  recall_noopq=%.4f  recall_opq=%.4f  (m=%d dim=%d, %lld docs, %lld queries)\n",
	recall_noopq, recall_opq, PQ_M, DIM, NDOCS, NQ);
CHECK(recall_opq >= recall_noopq);			/* DIRECTION, not a fixed number */

delete ex; delete pq_no; delete pq_opq;
delete [] de; delete [] dn; delete [] dq;
printf("test_opq_recall_ge_non_opq OK\n");
}

/*
	(b) OPQ composes with the #24 PQ-backed token graph (REPLACE posture +
	build_token_index): scoring flows through token_score_prepared on the ROTATED
	prepared ADC table.  A planted doc whose two tokens equal the two query tokens
	scores MaxSim == 2 (the maximum), so it must rank #1; nothing crashes.
*/
static void test_opq_composes_with_token_graph(void)
{
char *dir = make_dir("/tmp/ant_mvopq_graph");
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(PQ_M, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_opq(1) == 0);

/* the planted query: two distinctive anisotropic tokens */
float q[2 * DIM];
make_token(900001, q + 0 * DIM);
make_token(900002, q + 1 * DIM);

/* background corpus */
float doc[3 * DIM];
long long tokseed = 0;
for (long long i = 0; i < 300; i++)
	{
	long long md = (i % 3) + 1;
	for (long long t = 0; t < md; t++) make_token(tokseed++, doc + t * DIM);
	char key[32], body[64]; sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld z</DOC>", i);
	CHECK(ix->add_document(key, body, NULL, doc, md) >= 0);
	}
/* planted exact-match doc: its two tokens ARE the two query tokens */
CHECK(ix->add_document("planted", "<DOC>planted z</DOC>", NULL, q, 2) >= 0);
CHECK(ix->flush() == 0);
CHECK(ix->build_multivector_pq() == 0);
CHECK(ix->build_token_index() == 0);			/* PQ-backed token graph over the codes */
CHECK(ix->disk_segment_has_token_index(0) == 1);

long long n = ix->search_multivector(q, 2, 10);
CHECK(n > 0);
printf("  token-graph top-1 = %s score=%.4f (n=%lld)\n", ix->get_hit(0)->filename, ix->get_hit(0)->score, n);
CHECK(strcmp(ix->get_hit(0)->filename, "planted") == 0);	/* planted MaxSim==2 outranks all */

delete ix; delete [] dir;
printf("test_opq_composes_with_token_graph OK\n");
}

/*
	(c) OPQ + MV_TIER_NONE: the resident float pool is dropped at reopen and the
	segment's only source is the PQ store, so search reconstructs tokens from PQ
	codes -- un-rotating via R^T (token_reconstruct) -- and scores ADC-MaxSim.
	The planted exact-match doc must still be found; search must not crash.
*/
static void test_opq_none_tier_reconstruct(void)
{
char *dir = make_dir("/tmp/ant_mvopq_none");
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);
CHECK(ix->set_rerank_config(DIM, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_config(PQ_M, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->set_multivector_pq_opq(1) == 0);

float q[2 * DIM];
make_token(800001, q + 0 * DIM);
make_token(800002, q + 1 * DIM);

float doc[3 * DIM];
long long tokseed = 0;
for (long long i = 0; i < 300; i++)
	{
	long long md = (i % 3) + 1;
	for (long long t = 0; t < md; t++) make_token(tokseed++, doc + t * DIM);
	char key[32], body[64]; sprintf(key, "doc-%lld", i); sprintf(body, "<DOC>term%lld z</DOC>", i);
	CHECK(ix->add_document(key, body, NULL, doc, md) >= 0);
	}
CHECK(ix->add_document("planted", "<DOC>planted z</DOC>", NULL, q, 2) >= 0);
CHECK(ix->flush() == 0);
CHECK(ix->build_multivector_pq() == 0);
CHECK(ix->set_multivector_resident_tier(ATIRE_segment_index::MV_TIER_NONE) == 0);
delete ix;						/* NONE takes effect on reopen */

ix = new ATIRE_segment_index();
CHECK(ix->open(dir) == 0);				/* config restores OPQ + NONE */
CHECK(ix->multivector_pq_opq() == 1);			/* v3 config persisted OPQ */
CHECK(ix->disk_segment_resident_tier_mv(0) == ATIRE_segment_index::MV_TIER_NONE);	/* no float resident */
CHECK(ix->build_token_index() == 0);			/* .tann over the PQ source */
CHECK(ix->disk_segment_has_token_index(0) == 1);

long long n = ix->search_multivector(q, 2, 10);
CHECK(n > 0);						/* NONE-tier reconstruct-from-PQ answers */
/* planted (MaxSim==2 in exact space) must survive the lossy reconstruct into the top-k */
long long rank = -1;
for (long long i = 0; i < n; i++)
	if (strcmp(ix->get_hit(i)->filename, "planted") == 0) { rank = i; break; }
printf("  none-tier planted rank = %lld (top-1 = %s, n=%lld)\n", rank, ix->get_hit(0)->filename, n);
CHECK(rank >= 0);

delete ix; delete [] dir;
printf("test_opq_none_tier_reconstruct OK\n");
}

int main(void)
{
test_opq_recall_ge_non_opq();
test_opq_composes_with_token_graph();
test_opq_none_tier_reconstruct();
printf("ALL test_mvpq_opq_compose PASSED\n");
return 0;
}

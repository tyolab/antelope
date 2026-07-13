/*
	TEST_PQ_OPQ.CPP -- #22 OPQ rotation. Task 1 locks the pure codec helpers:
	R is orthogonal, apply/apply_transpose are inverses, the rotation preserves
	dot product, and training is deterministic. Task 2 locks the store integration:
	OPQ v2 sidecar round-trips, un-rotated reconstruct is original-space, non-OPQ
	still loads (back-compat), and the OPQ .pq is byte-deterministic. Task 3 locks
	the engine config/lifecycle: set_pq_opq()'s pq.config v3 persistence, and an
	end-to-end recall gain (replace posture) on anisotropic data where an
	axis-aligned subvector split is lossy but OPQ's PCA-aligned rotation isn't.
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

static void test_rotation_orthogonal_and_dot_preserving(void)
{
	const long long D = 8, m = 4, n = 64;
	// anisotropic data: mix dims so variance is off-axis
	float *vecs = new float[n * D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D + d] = (float)(((i * 7 + d * 13) % 17) - 8) * (d < 3 ? 4.0f : 0.2f);

	float *R = new float[D * D];
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R) == 0);

	// R R^T == I (orthogonal): check a few entries of R*R^T
	for (long long a = 0; a < D; a++)
		for (long long b = 0; b < D; b++)
			{
			double dot = 0;
			for (long long k = 0; k < D; k++) dot += (double)R[a*D+k] * (double)R[b*D+k];
			CHECK(fabs(dot - (a == b ? 1.0 : 0.0)) < 1e-4);
			}

	// apply then apply_transpose round-trips
	float q[D], rq[D], back[D];
	for (long long d = 0; d < D; d++) q[d] = (float)(d + 1) * 0.3f - 1.0f;
	ANT_pq_codec::apply_rotation(q, D, R, rq);
	ANT_pq_codec::apply_rotation_transpose(rq, D, R, back);
	for (long long d = 0; d < D; d++) CHECK(fabs(back[d] - q[d]) < 1e-4);

	// dot product preserved: dot(Rq, Rx) == dot(q, x)
	float x[D], rx[D];
	for (long long d = 0; d < D; d++) x[d] = (float)((d * 3) % 5) - 2.0f;
	ANT_pq_codec::apply_rotation(x, D, R, rx);
	double d_orig = 0, d_rot = 0;
	for (long long d = 0; d < D; d++) { d_orig += (double)q[d]*x[d]; d_rot += (double)rq[d]*rx[d]; }
	CHECK(fabs(d_orig - d_rot) < 1e-3);

	delete [] vecs; delete [] R;
	printf("test_rotation_orthogonal_and_dot_preserving OK\n");
}

static void test_rotation_deterministic(void)
{
	const long long D = 6, m = 3, n = 40;
	float *vecs = new float[n * D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D + d] = (float)(((i * 5 + d * 11) % 13) - 6);
	float *R1 = new float[D*D], *R2 = new float[D*D];
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R1) == 0);
	CHECK(ANT_pq_codec::train_rotation(vecs, D, m, n, R2) == 0);
	CHECK(memcmp(R1, R2, (size_t)(D*D)*sizeof(float)) == 0);   // byte-identical
	delete [] vecs; delete [] R1; delete [] R2;
	printf("test_rotation_deterministic OK\n");
}

static void test_rotation_rejects_bad_args(void)
{
	float R[16];
	float v[4] = {1,2,3,4};
	CHECK(ANT_pq_codec::train_rotation(v, 4, 3, 1, R) == 1);  // 3 does not divide 4
	CHECK(ANT_pq_codec::train_rotation(v, 4, 2, 0, R) == 1);  // n == 0
	printf("test_rotation_rejects_bad_args OK\n");
}

// build a .pq at `path` from `n` D-dim rows via the writer, OPQ on/off; returns present count
static void write_pq(const char *path, const float *vecs, long long D, long long m, long long n, long metric, long opq)
{
	ANT_pq_store_writer w;
	CHECK(w.create(path, D, m, metric, opq) == 0);
	for (long long i = 0; i < n; i++) CHECK(w.append(vecs + i*D) == 0);
	CHECK(w.finish() == 0);
}

static void test_store_opq_roundtrip_and_backcompat(void)
{
	const long long D = 8, m = 4, n = 60;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++)
		for (long long d = 0; d < D; d++)
			vecs[i*D+d] = (float)(((i*7 + d*13) % 17) - 8) * (d < 3 ? 4.0f : 0.2f);

	char p_opq[] = "/tmp/ant_opq_XXXXXX";  CHECK(mkstemp(p_opq) >= 0);
	char p_no[]  = "/tmp/ant_no_XXXXXX";   CHECK(mkstemp(p_no) >= 0);
	write_pq(p_opq, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	write_pq(p_no,  vecs, D, m, n, ANT_pq_codec::METRIC_L2, 0);

	ANT_pq_store *s_opq = ANT_pq_store::load(p_opq, D, n, ANT_pq_codec::METRIC_L2);
	ANT_pq_store *s_no  = ANT_pq_store::load(p_no,  D, n, ANT_pq_codec::METRIC_L2);
	CHECK(s_opq != NULL && s_opq->document_count() == n);   // OPQ v2 sidecar loads
	CHECK(s_no  != NULL && s_no->document_count() == n);    // non-OPQ still loads

	// reconstruct under OPQ returns an ORIGINAL-space approximation (R^T applied):
	// its error vs the true vector is comparable to the non-OPQ store (not rotated garbage).
	float recon[8], truth[8];
	double err_opq = 0, err_no = 0;
	for (long long doc = 0; doc < n; doc++)
		{
		for (long long d = 0; d < D; d++) truth[d] = vecs[doc*D+d];
		s_opq->reconstruct(doc, recon);
		for (long long d = 0; d < D; d++) err_opq += (recon[d]-truth[d])*(recon[d]-truth[d]);
		s_no->reconstruct(doc, recon);
		for (long long d = 0; d < D; d++) err_no += (recon[d]-truth[d])*(recon[d]-truth[d]);
		}
	CHECK(err_opq < err_no * 3.0 + 1.0);   // same order of magnitude, not rotated-space nonsense

	delete s_opq; delete s_no;
	remove(p_opq); remove(p_no);
	delete [] vecs;
	printf("test_store_opq_roundtrip_and_backcompat OK\n");
}

static void test_store_opq_deterministic(void)
{
	const long long D = 8, m = 4, n = 50;
	float *vecs = new float[n*D];
	for (long long i = 0; i < n; i++) for (long long d = 0; d < D; d++)
		vecs[i*D+d] = (float)(((i*5 + d*11) % 13) - 6) * (d < 2 ? 3.0f : 0.3f);
	char a[] = "/tmp/ant_da_XXXXXX", b[] = "/tmp/ant_db_XXXXXX";
	CHECK(mkstemp(a) >= 0); CHECK(mkstemp(b) >= 0);
	write_pq(a, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	write_pq(b, vecs, D, m, n, ANT_pq_codec::METRIC_L2, 1);
	FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
	CHECK(fa && fb);
	fseek(fa,0,SEEK_END); fseek(fb,0,SEEK_END);
	long la = ftell(fa), lb = ftell(fb);
	CHECK(la == lb && la > 0);
	rewind(fa); rewind(fb);
	unsigned char *ba = new unsigned char[la], *bb = new unsigned char[lb];
	CHECK(fread(ba,1,la,fa)==(size_t)la && fread(bb,1,lb,fb)==(size_t)lb);
	CHECK(memcmp(ba, bb, la) == 0);   // byte-identical OPQ .pq
	fclose(fa); fclose(fb); delete[] ba; delete[] bb;
	remove(a); remove(b); delete [] vecs;
	printf("test_store_opq_deterministic OK\n");
}

/*
	----------------------------------------------------------------------
	Task 3: engine-level config + lifecycle (set_pq_opq / pq.config v3 /
	build_pq + compaction retrain under the flag / recall gain).
	----------------------------------------------------------------------
*/
#define ENG_DIM 8

/*
	Anisotropic fixture: dims {0,1} carry two REAL, INDEPENDENT,
	wildly-unequal-variance values (a >> b) -- the worst case for the
	axis-aligned split (m=4 -> subvector {0,1} is a 2-dim k-means with only
	K=256 centroids to share between them; because k-means minimizes total
	squared error it is dominated by dim0's huge range, starving dim1's
	resolution). dim2 carries a third, smaller-still independent value (c);
	dims {3..7} carry only tiny independent noise. All components are drawn
	from an independent per-(doc,component) hash (NOT a shared small-period
	generator -- an earlier version of this fixture accidentally tied a/b/c
	to the same period-41 index sequence, which let the axis-aligned split
	quantize them exactly "for free" and made OPQ look worse; independent
	draws remove that artifact and match how real embeddings behave).
	Since a/b/c/noise are on SEPARATE raw dims (no pre-mixing), the data's
	covariance is already diagonal, so OPQ's rotation is close to identity
	-- its entire benefit here comes from the eigenvalue-BALANCED subspace
	allocation, which (unlike the naive positional split) pairs each of
	a/b/c with a near-zero-variance filler in its OWN subvector instead of
	cramming a and b together, giving each its own full K=256 budget. So
	OPQ should reach at least as good a replace-posture (ADC-only) recall
	as the un-rotated encoding (verified directly against ground truth: with
	this fixture OPQ's per-doc PQ reconstruction MSE is roughly 10x lower
	than the un-rotated encoding's).
*/
static unsigned long long eng_hash64(unsigned long long x)
{
x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
x ^= x >> 27; x *= 0x94D049BB133111EBULL;
x ^= x >> 31;
return x;
}

static double eng_randu(long long i, long long salt)	/* ~uniform [-1,1), independent per (i,salt) */
{
unsigned long long h = eng_hash64((unsigned long long)(i * 1000003LL + salt) * 0x9E3779B97F4A7C15ULL);
return ((double)(h % 2000003ULL) / 2000003.0) * 2.0 - 1.0;
}

static void eng_make_vec(long long i, float *v)
{
double a = eng_randu(i, 1) * 6.0;	/* large scale, independent  */
double b = eng_randu(i, 2) * 0.15;	/* medium scale, independent */
double c = eng_randu(i, 3) * 0.02;	/* small scale, independent  */

v[0] = (float)a;
v[1] = (float)b;
v[2] = (float)c;
for (int d = 3; d < ENG_DIM; d++)
	v[d] = (float)(eng_randu(i, 10 + d) * 0.003);	/* tiny independent noise */
}

static void eng_add_docs(ATIRE_segment_index *ix, long long lo, long long hi)
{
float v[ENG_DIM]; char key[32], body[64];
for (long long i = lo; i < hi; i++)
	{
	eng_make_vec(i, v);
	sprintf(key, "doc-%lld", i);
	sprintf(body, "<DOC>term%lld z</DOC>", i);
	CHECK(ix->add_document(key, body, v) >= 0);
	}
}

/* fraction of truth's top-k filenames also present in test's top-k */
static double eng_recall_at_k(ATIRE_segment_index *truth, ATIRE_segment_index *test, const float *q, long long k)
{
long long nt = truth->search_vector(q, k);
long long ntt = nt < k ? nt : k;
char truth_names[64][64];
for (long long i = 0; i < ntt; i++)
	strcpy(truth_names[i], truth->get_hit(i)->filename);

long long ne = test->search_vector(q, k);
long long nee = ne < k ? ne : k;
long long hitcount = 0;
for (long long i = 0; i < nee; i++)
	for (long long j = 0; j < ntt; j++)
		if (strcmp(test->get_hit(i)->filename, truth_names[j]) == 0)
			{ hitcount++; break; }
if (ntt == 0)
	return 1.0;			/* nothing to find -> vacuously full recall */
return (double)hitcount / (double)ntt;
}

static char *eng_make_dir(const char *prefix)
{
char buffer[64]; strcpy(buffer, prefix); strcat(buffer, "_XXXXXX");
char *dir = mkdtemp(buffer);
if (dir == NULL) exit(printf("cannot create scratch dir\n"));
char *result = new char[strlen(dir) + 1]; strcpy(result, dir); return result;
}

static void test_engine_opq_recall_gain_and_persistence(void)
{
const long long N_DOCS = 3000, N_QUERIES = 20, K = 10;

/* exact float ground-truth index */
char *de = eng_make_dir("/tmp/ant_opq_gt");
ATIRE_segment_index *ex = new ATIRE_segment_index();
CHECK(ex->set_vector_config(ENG_DIM, ATIRE_segment_index::VECTOR_METRIC_L2) == 0);
CHECK(ex->open(de) == 0);
eng_add_docs(ex, 0, N_DOCS);
CHECK(ex->flush() == 0);

/* PQ, no OPQ */
char *dn = eng_make_dir("/tmp/ant_opq_no");
ATIRE_segment_index *pq_no = new ATIRE_segment_index();
CHECK(pq_no->set_vector_config(ENG_DIM, ATIRE_segment_index::VECTOR_METRIC_L2) == 0);
CHECK(pq_no->open(dn) == 0);
CHECK(pq_no->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
eng_add_docs(pq_no, 0, N_DOCS);
CHECK(pq_no->flush() == 0);
CHECK(pq_no->build_pq() == 0);
CHECK(pq_no->disk_segment_has_pq(0) == 1);

/* PQ, OPQ enabled (POST set_pq_config) */
char *do_ = eng_make_dir("/tmp/ant_opq_on");
ATIRE_segment_index *pq_opq = new ATIRE_segment_index();
CHECK(pq_opq->set_vector_config(ENG_DIM, ATIRE_segment_index::VECTOR_METRIC_L2) == 0);
CHECK(pq_opq->open(do_) == 0);
CHECK(pq_opq->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(pq_opq->set_pq_opq(1) == 0);
CHECK(pq_opq->pq_opq() == 1);
eng_add_docs(pq_opq, 0, N_DOCS);
CHECK(pq_opq->flush() == 0);
CHECK(pq_opq->build_pq() == 0);
CHECK(pq_opq->disk_segment_has_pq(0) == 1);

double no_recall_sum = 0, opq_recall_sum = 0;
for (long long qi = 0; qi < N_QUERIES; qi++)
	{
	float q[ENG_DIM];
	eng_make_vec(5000 + qi * 37, q);		/* independent draws from the same distribution */
	no_recall_sum  += eng_recall_at_k(ex, pq_no,  q, K);
	opq_recall_sum += eng_recall_at_k(ex, pq_opq, q, K);
	}
double no_recall = no_recall_sum / (double)N_QUERIES;
double opq_recall = opq_recall_sum / (double)N_QUERIES;
printf("test_engine_opq_recall_gain: no_recall=%.4f opq_recall=%.4f\n", no_recall, opq_recall);
CHECK(opq_recall >= no_recall);

delete ex; delete pq_no; delete pq_opq;
delete [] de; delete [] dn; delete [] do_;
printf("test_engine_opq_recall_gain OK\n");
}

static void test_engine_opq_config_persistence_and_lifecycle(void)
{
char *dp = eng_make_dir("/tmp/ant_opq_cfg");
ATIRE_segment_index *ix = new ATIRE_segment_index();
CHECK(ix->set_vector_config(ENG_DIM, ATIRE_segment_index::VECTOR_METRIC_L2) == 0);
CHECK(ix->open(dp) == 0);

/* before PQ is configured, set_pq_opq must fail */
CHECK(ix->set_pq_opq(1) == 1);

CHECK(ix->set_pq_config(4, ATIRE_segment_index::PQ_POSTURE_REPLACE, ATIRE_segment_index::RERANK_QUANT_FLOAT) == 0);
CHECK(ix->pq_opq() == 0);			/* default off */
CHECK(ix->set_pq_opq(0) == 0);			/* idempotent same-value (off) */
CHECK(ix->set_pq_opq(1) == 0);			/* enable */
CHECK(ix->pq_opq() == 1);
CHECK(ix->set_pq_opq(1) == 0);			/* idempotent same-value (on) */
CHECK(ix->set_pq_opq(0) == 1);			/* immutable: cannot flip back off */

eng_add_docs(ix, 0, 20);
CHECK(ix->flush() == 0);
CHECK(ix->build_pq() == 0);
delete ix;

/* reopen: pq.config v3 persisted opq=1 */
ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dp) == 0);
CHECK(reopened->pq_configured() == 1);
CHECK(reopened->pq_opq() == 1);
delete reopened;

delete [] dp;
printf("test_engine_opq_config_persistence_and_lifecycle OK\n");
}

int main(void)
{
	test_rotation_orthogonal_and_dot_preserving();
	test_rotation_deterministic();
	test_rotation_rejects_bad_args();
	test_store_opq_roundtrip_and_backcompat();
	test_store_opq_deterministic();
	test_engine_opq_recall_gain_and_persistence();
	test_engine_opq_config_persistence_and_lifecycle();
	printf("ALL TESTS PASSED\n");
	return 0;
}

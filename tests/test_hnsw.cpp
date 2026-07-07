/*
	TEST_HNSW.CPP -- unit tests for ANT_hnsw (build + search + recall).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../source/hnsw.h"
#include "../source/vector_store.h"
#include "../source/index_tombstones.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/*
	Build an in-memory ANT_vector_store by writing a .vec with the writer and loading it.
	dim floats per doc, all present.
*/
static ANT_vector_store *make_store(const char *path, long long dim, long long n, float *data)
{
ANT_vector_store_writer w;
long long i;
CHECK(w.create(path, dim) == 0);
for (i = 0; i < n; i++)
	CHECK(w.append(data + i * dim) == 0);
CHECK(w.finish() == 0);
return ANT_vector_store::load(path, dim, n);
}

static void brute_force_topk(const float *query, const float *data, long long dim, long long n, long metric, long long k, long long *out)
{
long long i, j;
double *score = new double[n];
for (i = 0; i < n; i++)
	score[i] = ANT_vector_store::kernel(query, data + i * dim, dim, metric);
for (j = 0; j < k; j++)
	{
	long long best = -1;
	for (i = 0; i < n; i++)
		if (score[i] > -1e300 && (best < 0 || score[i] > score[best]))
			best = i;
	out[j] = best;
	score[best] = -1e300;
	}
delete [] score;
}

static void test_recall_and_determinism(void)
{
long long dim = 24, n = 500, k = 10, i, d;
char path[64]; strcpy(path, "/tmp/ant_hnsw_XXXXXX"); int fd = mkstemp(path); if (fd >= 0) close(fd);
float *data = new float[n * dim];
srand(11);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 200 - 100);
ANT_vector_store *store = make_store(path, dim, n, data);

ANT_hnsw a, b;
CHECK(a.build(store, /*M=*/16, /*ef_construction=*/200, ANT_vector_store::METRIC_L2) == 0);
CHECK(b.build(store, 16, 200, ANT_vector_store::METRIC_L2) == 0);
CHECK(a.node_count() == n);

ANT_index_tombstones stones(n);
float query[24]; for (d = 0; d < dim; d++) query[d] = (float)(rand() % 200 - 100);

long long ha[10], hb[10]; double sa[10], sb[10];
long long ca = a.search(query, ANT_vector_store::METRIC_L2, /*ef_search=*/64, k, store, &stones, ha, sa);
long long cb = b.search(query, ANT_vector_store::METRIC_L2, 64, k, store, &stones, hb, sb);
CHECK(ca == k && cb == k);
/* determinism: two builds from the same data give identical results */
for (i = 0; i < k; i++) { CHECK(ha[i] == hb[i]); }

long long exact[10]; brute_force_topk(query, data, dim, n, ANT_vector_store::METRIC_L2, k, exact);
long long overlap = 0, j;
for (i = 0; i < k; i++) for (j = 0; j < k; j++) if (ha[i] == exact[j]) { overlap++; break; }
CHECK((double)overlap / (double)k >= 0.8);		/* single query, generous floor; e2e test averages */
/* scores must be descending kernel (higher = nearer) */
for (i = 1; i < k; i++) CHECK(sa[i] <= sa[i-1] + 1e-9);
delete store; delete [] data; unlink(path);
printf("test_recall_and_determinism OK (overlap=%lld/%lld)\n", overlap, k);
}

static void test_ef_monotonic(void)
{
long long dim = 16, n = 300, k = 10, i, d;
char path[64]; strcpy(path, "/tmp/ant_hnsw2_XXXXXX"); int fd = mkstemp(path); if (fd >= 0) close(fd);
float *data = new float[n * dim];
srand(3);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 100 - 50);
ANT_vector_store *store = make_store(path, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 16, 200, ANT_vector_store::METRIC_COSINE) == 0);
ANT_index_tombstones stones(n);
float q[16]; for (d = 0; d < dim; d++) q[d] = (float)(rand() % 100 - 50);
long long exact[10]; brute_force_topk(q, data, dim, n, ANT_vector_store::METRIC_COSINE, k, exact);
double recall_lo = 0, recall_hi = 0; long long h[10]; double s[10]; long long i2, j2;
g.search(q, ANT_vector_store::METRIC_COSINE, 16, k, store, &stones, h, s);
for (i2=0;i2<k;i2++) for(j2=0;j2<k;j2++) if(h[i2]==exact[j2]){recall_lo++;break;}
g.search(q, ANT_vector_store::METRIC_COSINE, 128, k, store, &stones, h, s);
for (i2=0;i2<k;i2++) for(j2=0;j2<k;j2++) if(h[i2]==exact[j2]){recall_hi++;break;}
CHECK(recall_hi >= recall_lo);		/* more ef never hurts recall */
delete store; delete [] data; unlink(path);
printf("test_ef_monotonic OK (ef16=%.0f ef128=%.0f)\n", recall_lo, recall_hi);
}

static void test_tombstone_filter(void)
{
long long dim = 8, n = 50, i, d;
char path[64]; strcpy(path, "/tmp/ant_hnsw3_XXXXXX"); int fd = mkstemp(path); if (fd >= 0) close(fd);
float *data = new float[n * dim];
for (i = 0; i < n; i++) for (d = 0; d < dim; d++) data[i*dim+d] = (float)((i + d) % 7);
ANT_vector_store *store = make_store(path, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 8, 64, ANT_vector_store::METRIC_L2) == 0);
ANT_index_tombstones stones(n);
stones.set_deleted(3); stones.set_deleted(7);
float q[8]; for (d = 0; d < dim; d++) q[d] = (float)(d % 7);
long long h[20]; double s[20];
long long c = g.search(q, ANT_vector_store::METRIC_L2, 32, 20, store, &stones, h, s);
for (i = 0; i < c; i++) { CHECK(h[i] != 3 && h[i] != 7); }		/* deleted docids never returned */
delete store; delete [] data; unlink(path);
printf("test_tombstone_filter OK\n");
}

static void test_degenerate_M(void)
{
long long dim = 8, n = 30, i, d;
char path[64]; strcpy(path, "/tmp/ant_hnsw_M_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
float *data = new float[n * dim];
for (i = 0; i < n * dim; i++) data[i] = (float)(i % 5);
ANT_vector_store *store = make_store(path, dim, n, data);
ANT_hnsw g;
CHECK(g.build(store, /*M=*/1, 64, ANT_vector_store::METRIC_L2) == 0);	/* must NOT crash; M<2 -> default */
CHECK(g.get_M() == 16);							/* fell back to the default */
ANT_index_tombstones stones(n);
float q[8]; for (d = 0; d < dim; d++) q[d] = (float)(d % 5);
long long h[5]; double s[5];
long long c = g.search(q, ANT_vector_store::METRIC_L2, 16, 5, store, &stones, h, s);
CHECK(c == 5);
delete store; delete [] data; unlink(path);
printf("test_degenerate_M OK\n");
}

static void test_save_load_roundtrip(void)
{
long long dim = 16, n = 200, k = 8, i, d;
char vpath[64]; strcpy(vpath, "/tmp/ant_hnsw_v_XXXXXX"); { int fd=mkstemp(vpath); if(fd>=0) close(fd); }
char gpath[64]; strcpy(gpath, "/tmp/ant_hnsw_g_XXXXXX"); { int fd=mkstemp(gpath); if(fd>=0) close(fd); unlink(gpath); }
float *data = new float[n * dim]; srand(5);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 100 - 50);
ANT_vector_store *store = make_store(vpath, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 16, 200, ANT_vector_store::METRIC_L2) == 0);
CHECK(g.save(gpath) == 0);
ANT_hnsw *loaded = ANT_hnsw::load(gpath, 16, 200, n);
CHECK(loaded->node_count() == n);
CHECK(!loaded->empty());
ANT_index_tombstones stones(n);
float q[16]; for (d = 0; d < dim; d++) q[d] = (float)(rand() % 100 - 50);
long long h1[8], h2[8]; double s1[8], s2[8];
long long c1 = g.search(q, ANT_vector_store::METRIC_L2, 64, k, store, &stones, h1, s1);
long long c2 = loaded->search(q, ANT_vector_store::METRIC_L2, 64, k, store, &stones, h2, s2);
CHECK(c1 == c2);
for (i = 0; i < c1; i++) { CHECK(h1[i] == h2[i]); }		/* loaded graph searches identically */
delete loaded; delete store; delete [] data; unlink(vpath); unlink(gpath);
printf("test_save_load_roundtrip OK\n");
}

static void test_load_degrade(void)
{
/* missing file -> empty graph */
ANT_hnsw *missing = ANT_hnsw::load("/tmp/does_not_exist_hnsw", 16, 200, 10);
CHECK(missing->node_count() == 0 && missing->empty());
CHECK(missing->search(NULL, ANT_vector_store::METRIC_L2, 8, 4, NULL, NULL, NULL, NULL) == 0);
delete missing;
/* config mismatch -> empty graph */
long long dim = 8, n = 20, i;
char vpath[64]; strcpy(vpath, "/tmp/ant_hnsw_v2_XXXXXX"); { int fd=mkstemp(vpath); if(fd>=0) close(fd); }
char gpath[64]; strcpy(gpath, "/tmp/ant_hnsw_g2_XXXXXX"); { int fd=mkstemp(gpath); if(fd>=0) close(fd); unlink(gpath); }
float *data = new float[n * dim]; for (i = 0; i < n*dim; i++) data[i] = (float)(i % 5);
ANT_vector_store *store = make_store(vpath, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 8, 64, ANT_vector_store::METRIC_L2) == 0);
CHECK(g.save(gpath) == 0);
ANT_hnsw *wrong_M = ANT_hnsw::load(gpath, 16, 64, n);		/* M mismatch */
CHECK(wrong_M->node_count() == 0 && wrong_M->empty());
delete wrong_M;
ANT_hnsw *wrong_n = ANT_hnsw::load(gpath, 8, 64, n + 1);		/* doc count mismatch */
CHECK(wrong_n->node_count() == 0 && wrong_n->empty());
delete wrong_n;
delete store; delete [] data; unlink(vpath); unlink(gpath);
printf("test_load_degrade OK\n");
}

static void test_load_content_corruption(void)
{
long long dim = 8, n = 60, i;
char vpath[64]; strcpy(vpath, "/tmp/ant_hnsw_cv_v_XXXXXX"); { int fd=mkstemp(vpath); if(fd>=0) close(fd); }
char gpath[64]; strcpy(gpath, "/tmp/ant_hnsw_cv_g_XXXXXX"); { int fd=mkstemp(gpath); if(fd>=0) close(fd); unlink(gpath); }
float *data = new float[n * dim]; srand(21);
for (i = 0; i < n*dim; i++) data[i] = (float)(rand() % 50 - 25);
ANT_vector_store *store = make_store(vpath, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 16, 200, ANT_vector_store::METRIC_L2) == 0);
CHECK(g.save(gpath) == 0);
/* the neighbours region starts at header(52) + 4*n (levels) + 8*(n+1) (offsets).
   The first int there is node 0's layer-0 [count]; the next ints are its docids.
   Overwrite the first neighbour docid with an out-of-range value, same file size. */
long long neigh_off = 52 + 4*n + 8*(n+1);
FILE *fp = fopen(gpath, "r+b"); CHECK(fp != NULL);
CHECK(fseek(fp, neigh_off + 4, SEEK_SET) == 0);		/* skip the first [count], hit the first docid */
int bad = (int)(n + 1000);							/* out of [0, n) */
CHECK(fwrite(&bad, sizeof(bad), 1, fp) == 1);
fclose(fp);
ANT_hnsw *loaded = ANT_hnsw::load(gpath, 16, 200, n);
CHECK(loaded->node_count() == 0 && loaded->empty());	/* content-corrupt sidecar degrades, no crash */
delete loaded; delete store; delete [] data; unlink(vpath); unlink(gpath);
printf("test_load_content_corruption OK\n");
}

static void test_tombstone_no_underfill(void)
{
long long dim = 24, n = 500, k = 10, dead = 60, i, d;
char path[64]; strcpy(path, "/tmp/ant_hnsw_tu_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
float *data = new float[n * dim]; srand(13);
for (i = 0; i < n*dim; i++) data[i] = (float)(rand() % 200 - 100);
ANT_vector_store *store = make_store(path, dim, n, data);
ANT_hnsw g; CHECK(g.build(store, 16, 200, ANT_vector_store::METRIC_L2) == 0);
float query[24]; for (d = 0; d < dim; d++) query[d] = (float)(rand() % 200 - 100);
/* exact ranking, then tombstone the exact top-`dead` nearest so the naive ef=64
   frontier is heavily depleted of live docs near the query (dead > ef-k, so the
   pre-fix drain-time filter under-fills: it returns far fewer than k live docs) */
long long exact[500]; brute_force_topk(query, data, dim, n, ANT_vector_store::METRIC_L2, n, exact);
ANT_index_tombstones stones(n);
for (i = 0; i < dead; i++) stones.set_deleted(exact[i]);
long long h[10]; double s[10];
long long c = g.search(query, ANT_vector_store::METRIC_L2, 64, k, store, &stones, h, s);
CHECK(c == k);							/* must fill k LIVE results despite `dead` deleted near the query */
for (i = 0; i < c; i++) CHECK(!stones.is_deleted(h[i]));	/* none deleted */
/* recall vs exact-over-live (the true next `dead`.. band) */
long long overlap = 0, j;
for (i = 0; i < c; i++) for (j = dead; j < dead + k; j++) if (h[i] == exact[j]) { overlap++; break; }
CHECK((double)overlap / (double)k >= 0.8);
delete store; delete [] data; unlink(path);
printf("test_tombstone_no_underfill OK (filled=%lld/%lld)\n", c, k);
}

static void test_load_docs_over_int32_cap(void)
{
/* Craft a .hnsw with a VALID header (magic + version) but documents ==
   ANT_HNSW_MAX_DOCUMENTS + 1 (0x80000000LL, first value that overflows int32
   docid storage).  load() must reject this on the Phase-1 range check --
   before any levels[]/offsets[] allocation -- and hand back a degraded,
   empty graph rather than trying to allocate/crash. */
char gpath[64]; strcpy(gpath, "/tmp/ant_hnsw_cap_XXXXXX"); { int fd=mkstemp(gpath); if(fd>=0) close(fd); }
unsigned long long magic; memcpy(&magic, "ANTHNSW1", 8);		/* same bytes as save()'s ant_hnsw_magic() */
unsigned int version = 1u;								/* ANT_HNSW_VERSION, private to hnsw.cpp */
long long M = 16, efc = 200, docs = 0x80000000LL, ep = -1, maxl = -1;
FILE *fp = fopen(gpath, "wb"); CHECK(fp != NULL);
CHECK(fwrite(&magic,sizeof(magic),1,fp)==1);
CHECK(fwrite(&version,sizeof(version),1,fp)==1);
CHECK(fwrite(&M,sizeof(M),1,fp)==1);
CHECK(fwrite(&efc,sizeof(efc),1,fp)==1);
CHECK(fwrite(&docs,sizeof(docs),1,fp)==1);
CHECK(fwrite(&ep,sizeof(ep),1,fp)==1);
CHECK(fwrite(&maxl,sizeof(maxl),1,fp)==1);
fclose(fp);
/* expected_* match the crafted header exactly, so it's the docs > cap check
   that rejects the file, not an expectation mismatch. */
ANT_hnsw *loaded = ANT_hnsw::load(gpath, M, efc, docs);
CHECK(loaded->node_count() == 0 && loaded->empty());
delete loaded; unlink(gpath);
printf("test_load_docs_over_int32_cap OK\n");
}

static void test_distance_cache_equivalence(void)
{
/* At dim >= ANT_HNSW_DISTANCE_CACHE_MIN_DIM build() memoises distances.  The
   memo only avoids recomputation -- it never changes a value -- so building
   the same data with the cache on vs. off must yield an identical graph.
   Verify by comparing search results (hits AND exact scores) across many
   queries; any structural difference would surface as a divergent result. */
long long dim = 256, n = 800, k = 10, i, d, qi;
CHECK(dim >= ANT_HNSW_DISTANCE_CACHE_MIN_DIM);			/* ensure the cache path is exercised */
char path[64]; strcpy(path, "/tmp/ant_hnsw_eq_XXXXXX"); int fd = mkstemp(path); if (fd >= 0) close(fd);
float *data = new float[n * dim];
srand(7);
for (i = 0; i < n * dim; i++) data[i] = (float)(rand() % 200 - 100);
ANT_vector_store *store = make_store(path, dim, n, data);

ANT_hnsw cached, plain;
CHECK(cached.build(store, 16, 200, ANT_vector_store::METRIC_COSINE, /*use_distance_cache=*/true) == 0);
CHECK(plain.build(store, 16, 200, ANT_vector_store::METRIC_COSINE, /*use_distance_cache=*/false) == 0);
CHECK(cached.node_count() == plain.node_count());

ANT_index_tombstones stones(n);
float *query = new float[dim];
for (qi = 0; qi < 50; qi++)
	{
	for (d = 0; d < dim; d++) query[d] = (float)(rand() % 200 - 100);
	long long hc[10], hp[10]; double sc[10], sp[10];
	long long cc = cached.search(query, ANT_vector_store::METRIC_COSINE, 64, k, store, &stones, hc, sc);
	long long cp = plain.search(query, ANT_vector_store::METRIC_COSINE, 64, k, store, &stones, hp, sp);
	CHECK(cc == cp);
	for (i = 0; i < cc; i++) { CHECK(hc[i] == hp[i]); CHECK(sc[i] == sp[i]); }		/* byte-identical */
	}
delete [] query; delete store; delete [] data; unlink(path);
printf("test_distance_cache_equivalence OK (cache-on == cache-off, dim=%lld)\n", dim);
}

int main(void)
{
test_recall_and_determinism();
test_distance_cache_equivalence();
test_ef_monotonic();
test_tombstone_filter();
test_tombstone_no_underfill();
test_degenerate_M();
test_save_load_roundtrip();
test_load_degrade();
test_load_content_corruption();
test_load_docs_over_int32_cap();
printf("PASSED\n");
return 0;
}

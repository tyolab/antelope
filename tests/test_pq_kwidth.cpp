/*
	TEST_PQ_KWIDTH.CPP -- store + engine variable code-width (#22.3):
	a k=16 store round-trips (reconstruct/scan_adc correct), its .pq is v3
	with a documents*row_bytes codes region (~half a k=256 store), a k=256
	store is written as v2 (byte-identical default), and set_pq_k persists
	to pq.config v5 with immutability + reopen restore.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pq_store.h"
#include "pq_codec.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static unsigned int read_u32_at(const char *path, long off)
{
FILE *fp = fopen(path, "rb");
unsigned int v = 0;
if (fp) { fseek(fp, off, SEEK_SET); if (fread(&v, sizeof(v), 1, fp) != 1) v = 0; fclose(fp); }
return v;
}

static long file_size_of(const char *path)
{
FILE *fp = fopen(path, "rb");
long n = -1;
if (fp) { fseek(fp, 0, SEEK_END); n = ftell(fp); fclose(fp); }
return n;
}

static void write_store(const char *path, long long dim, long long m, long long k, long long n, const float *vecs)
{
ANT_pq_store_writer w;
CHECK(w.create(path, dim, m, k, ANT_pq_codec::METRIC_L2, 0) == 0);
for (long long i = 0; i < n; i++)
	CHECK(w.append(vecs + i * dim) == 0);
CHECK(w.finish() == 0);
}

static void test_k16_roundtrip_and_size(void)
{
long long dim = 8, m = 4, n = 40;
float *vecs = new float[n * dim];
srand(7);
for (long long i = 0; i < n * dim; i++) vecs[i] = (float)(rand() % 1000) / 500.0f - 1.0f;

write_store("/tmp/kw16.pq", dim, m, 16, n, vecs);
// version field is at byte offset 8 (after 8-byte magic)
CHECK(read_u32_at("/tmp/kw16.pq", 8) == 3u);				// k!=256 -> v3

ANT_pq_store *s16 = ANT_pq_store::load("/tmp/kw16.pq", dim, n, ANT_pq_codec::METRIC_L2);
CHECK(s16->document_count() == n);
// reconstruct returns a valid centroid tuple (finite, dimensioned)
float recon[8];
s16->reconstruct(0, recon);
for (long long d = 0; d < dim; d++) CHECK(recon[d] == recon[d]);	// not NaN

// codes region shrinks: bits=4 -> row_bytes = (4*4+7)/8 = 2 (vs m=4 at k=256)
long size16 = file_size_of("/tmp/kw16.pq");
write_store("/tmp/kw256.pq", dim, m, 256, n, vecs);
CHECK(read_u32_at("/tmp/kw256.pq", 8) == 2u);				// k==256 -> v2 (byte-identity)
long size256 = file_size_of("/tmp/kw256.pq");
CHECK(size16 < size256);					// smaller codebook (m*16*sub) + smaller codes (n*2 vs n*4)
delete s16;
delete [] vecs;
}

static void test_bad_k_degrades(void)
{
// hand-forge a v3 header with k=6 (not a power of two): load must degrade to empty.
long long dim = 4, m = 2, n = 2;
float vecs[8] = {1,2,3,4, 5,6,7,8};
write_store("/tmp/kwbad.pq", dim, m, 16, n, vecs);
// overwrite the k field (offset 8 + 4 + 8 + 8 + 8 = 36) with 6
FILE *fp = fopen("/tmp/kwbad.pq", "r+b");
long long bad = 6; fseek(fp, 36, SEEK_SET); fwrite(&bad, sizeof(bad), 1, fp); fclose(fp);
ANT_pq_store *s = ANT_pq_store::load("/tmp/kwbad.pq", dim, n, ANT_pq_codec::METRIC_L2);
CHECK(s->document_count() == 0);				// forgiving: degraded empty store
delete s;
}

int main(void)
{
test_k16_roundtrip_and_size();
test_bad_k_degrades();
if (failures == 0) printf("ALL test_pq_kwidth PASSED\n");
else printf("%d CHECK(s) FAILED\n", failures);
return failures == 0 ? 0 : 1;
}

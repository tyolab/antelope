/*
	TEST_SIGNATURE_STORE.CPP -- unit tests for the per-segment .vsig sidecar.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/signature_store.h"
#include "../source/index_tombstones.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char *scratch(void)
{
static char path[64];
strcpy(path, "/tmp/ant_vsig_XXXXXX");
int fd = mkstemp(path); if (fd >= 0) close(fd); unlink(path);
return path;
}

static void test_roundtrip(void)
{
char name[64]; strcpy(name, scratch());
long long bits = 16;
unsigned char s0[2] = {0xAB, 0xCD}, s2[2] = {0x12, 0x34};
ANT_signature_store_writer w;
CHECK(w.create(name, bits) == 0);
CHECK(w.append(s0) == 0);			// docid 0 present
CHECK(w.append(NULL) == 0);			// docid 1 absent
CHECK(w.append(s2) == 0);			// docid 2 present
CHECK(w.finish() == 0);
ANT_signature_store *store = ANT_signature_store::load(name, bits, 3);
CHECK(store->document_count() == 3);
CHECK(store->has(0) && !store->has(1) && store->has(2));
CHECK(memcmp(store->get(0), s0, 2) == 0);
CHECK(memcmp(store->get(2), s2, 2) == 0);
delete store;
unlink(name);
printf("test_roundtrip OK\n");
}

static void test_degrade(void)
{
char name[64]; strcpy(name, scratch());
unsigned char s0[2] = {0x00, 0xFF};
ANT_signature_store_writer w;
CHECK(w.create(name, 16) == 0);
CHECK(w.append(s0) == 0);
CHECK(w.finish() == 0);
ANT_signature_store *wrong_bits = ANT_signature_store::load(name, 32, 1);	// bits mismatch -> empty
CHECK(wrong_bits->document_count() == 0);
delete wrong_bits;
ANT_signature_store *missing = ANT_signature_store::load("/tmp/does_not_exist_vsig", 16, 1);
CHECK(missing->document_count() == 0);
delete missing;
unlink(name);
printf("test_degrade OK\n");
}

static void test_shortlist(void)
{
char name[64]; strcpy(name, scratch());
long long bits = 8;
unsigned char a = 0x00, b = 0x0F, c = 0xFF;			// vs query 0x00: hamming 0, 4, 8
ANT_signature_store_writer w;
CHECK(w.create(name, bits) == 0);
CHECK(w.append(&a) == 0);
CHECK(w.append(&b) == 0);
CHECK(w.append(&c) == 0);
CHECK(w.finish() == 0);
ANT_signature_store *store = ANT_signature_store::load(name, bits, 3);
ANT_index_tombstones stones(3);
unsigned char query = 0x00;
long long docids[2], count = 0;
store->shortlist(&query, &stones, /*pool_size=*/2, docids, &count);
CHECK(count == 2);
long long has0 = (docids[0] == 0 || docids[1] == 0);
long long has1 = (docids[0] == 1 || docids[1] == 1);
CHECK(has0 && has1);				// nearest two are docids 0 and 1; docid 2 (hamming 8) excluded
delete store;
unlink(name);
printf("test_shortlist OK\n");
}

int main(void)
{
test_roundtrip();
test_degrade();
test_shortlist();
printf("PASSED\n");
return 0;
}

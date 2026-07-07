#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "payload_store.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)

int main(void)
{
char path[64]; strcpy(path,"/tmp/ant_pay_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_payload_store_writer w;
CHECK(w.create(path) == 0);
w.append("{\"a\":1}", 7);				/* doc0 */
w.append(NULL, 0);						/* doc1 empty */
unsigned char blob[4] = {0x00, 0xFF, 0x10, 0x00};
w.append(blob, 4);						/* doc2 binary incl NULs */
w.append("hello world", 11);			/* doc3 */
CHECK(w.finish() == 0);

ANT_payload_store *p = ANT_payload_store::load(path, 4);
CHECK(p != NULL && p->document_count() == 4);
const unsigned char *ptr; long long len;
p->get(0, &ptr, &len); CHECK(len == 7 && memcmp(ptr, "{\"a\":1}", 7) == 0);
p->get(1, &ptr, &len); CHECK(len == 0 && ptr == NULL);				/* empty -> (NULL,0) */
p->get(2, &ptr, &len); CHECK(len == 4 && memcmp(ptr, blob, 4) == 0);	/* binary + NULs preserved */
p->get(3, &ptr, &len); CHECK(len == 11 && memcmp(ptr, "hello world", 11) == 0);
p->get(4, &ptr, &len); CHECK(len == 0 && ptr == NULL);				/* OOB -> (NULL,0) */
p->get(-1, &ptr, &len); CHECK(len == 0 && ptr == NULL);				/* OOB -> (NULL,0) */
delete p; unlink(path);

/* wrong expected_documents -> empty */
ANT_payload_store *mism = ANT_payload_store::load(path, 99);			/* file already unlinked; also a mismatch */
CHECK(mism != NULL && mism->document_count() == 0);
delete mism;

/* corrupt header -> empty, no crash */
FILE *f=fopen(path,"wb"); fputs("XX", f); fclose(f);
ANT_payload_store *bad = ANT_payload_store::load(path, 4);
CHECK(bad != NULL && bad->document_count() == 0);
delete bad; unlink(path);

/* truncated pool (offsets claim more bytes than the file holds) -> empty */
strcpy(path,"/tmp/ant_pay2_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_payload_store_writer w2; CHECK(w2.create(path) == 0);
w2.append("abcdefgh", 8); CHECK(w2.finish() == 0);
/* chop the file to just past the header+offsets, dropping pool bytes */
{ FILE *tr=fopen(path,"rb"); fseek(tr,0,SEEK_END); long sz=ftell(tr); fclose(tr);
  CHECK(truncate(path, sz-4) == 0); }
ANT_payload_store *trunc = ANT_payload_store::load(path, 1);
CHECK(trunc != NULL && trunc->document_count() == 0);
delete trunc; unlink(path);

printf("PASSED\n");
return 0;
}

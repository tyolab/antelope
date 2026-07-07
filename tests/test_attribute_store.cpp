#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "attribute_store.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)

int main(void)
{
ANT_attribute_schema s;
s.add_field("created", ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("tenant",  ANT_attribute_schema::TYPE_STRING, 0);
s.add_field("lang",    ANT_attribute_schema::TYPE_STRING, 1);
s.add_field("archived",ANT_attribute_schema::TYPE_BOOL, 0);

char path[64]; strcpy(path,"/tmp/ant_attr_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_attribute_store_writer w;
CHECK(w.create(path, &s) == 0);
w.begin_document();
w.set_int(0, 1700000000LL); w.set_string(1, "acme");
w.add_string(2, "en"); w.add_string(2, "fr"); w.set_bool(3, 1);
w.end_document();
w.begin_document(); w.set_string(1, "beta"); w.set_bool(3, 0); w.end_document();		/* created + lang MISSING */
w.begin_document(); w.set_int(0, 1700000500LL); w.add_string(2, "de"); w.end_document();
w.begin_document(); w.end_document();												/* empty */
CHECK(w.finish() == 0);

ANT_attribute_store *a = ANT_attribute_store::load(path, &s, 4);
CHECK(a != NULL && a->document_count() == 4);
long long v;
CHECK(a->get_int(0, 0, &v) && v == 1700000000LL);
CHECK(a->has_field(1, 0));
long acme = a->string_id(1, "acme"); CHECK(acme >= 0);
CHECK(a->string_matches(1, 0, acme));
CHECK(a->string_matches(2, 0, a->string_id(2, "fr")));		/* multi contains fr */
CHECK(!a->string_matches(2, 0, a->string_id(2, "de")));
CHECK(a->string_matches(2, 2, a->string_id(2, "de")));
CHECK(a->string_id(1, "nonexistent") < 0);					/* dict miss */
CHECK(!a->has_field(0, 1));									/* doc1 missing created */
CHECK(a->int_equals(0, 0, 1700000000LL));
CHECK(!a->int_equals(0, 1, 1700000000LL));					/* missing -> false */
CHECK(a->int_matches_range(0, 2, 1700000200LL, 1700001000LL, 1, 1));	/* doc2 in range */
CHECK(!a->int_matches_range(0, 1, 0, 9999999999LL, 1, 1));	/* missing -> false */
int b;
CHECK(a->get_bool(3, 1, &b) && b == 0);
CHECK(a->bool_equals(3, 1, 0) && !a->bool_equals(3, 1, 1));
CHECK(!a->has_field(3, 3));									/* doc3 empty */
/* enumeration */
CHECK(a->value_count(2, 0) == 2 && a->value_count(2, 3) == 0);
char buf[32]; CHECK(a->get_string_at(2, 0, 0, buf, sizeof(buf)) && (!strcmp(buf,"en")||!strcmp(buf,"fr")));
delete a; unlink(path);

/* corrupt -> empty */
FILE *f=fopen(path,"wb"); fputs("nope",f); fclose(f);
ANT_attribute_store *bad = ANT_attribute_store::load(path, &s, 4);
CHECK(bad != NULL && bad->document_count() == 0);
delete bad; unlink(path);
printf("PASSED\n");
return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "filter.h"
#include "attribute_store.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
#define DOCS 6

static int getbit(const unsigned char *b, long long d){ return (b[d>>3]>>(d&7))&1; }

/* assert evaluate(f) over the store == the expect[] mask exactly, AND padding bits (6,7) are 0 */
static void expect_set(ANT_attribute_store *a, ANT_filter *f, const int *expect, const char *label)
{
	unsigned char bits[(DOCS+7)/8];
	memset(bits, 0xAA, sizeof(bits));					/* poison to catch un-cleared padding */
	CHECK(f->evaluate(a, DOCS, bits) == 0);
	for (long long d = 0; d < DOCS; d++)
		if (getbit(bits, d) != expect[d]) { printf("FAIL %s: doc %lld got %d want %d\n", label, d, getbit(bits,d), expect[d]); exit(1); }
	CHECK(getbit(bits, 6) == 0 && getbit(bits, 7) == 0);	/* padding masked */
}

int main(void)
{
ANT_attribute_schema s;
s.add_field("created", ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("price",   ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("tenant",  ANT_attribute_schema::TYPE_STRING, 0);
s.add_field("lang",    ANT_attribute_schema::TYPE_STRING, 1);
s.add_field("archived",ANT_attribute_schema::TYPE_BOOL, 0);

char path[64]; strcpy(path,"/tmp/ant_fe_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_attribute_store_writer w; CHECK(w.create(path, &s) == 0);
/* doc0 */ w.begin_document(); w.set_int(0,100); w.set_int(1,50);  w.set_string(2,"acme");  w.add_string(3,"en"); w.add_string(3,"fr"); w.set_bool(4,0); w.end_document();
/* doc1 */ w.begin_document(); w.set_int(0,200); w.set_int(1,150); w.set_string(2,"acme");  w.add_string(3,"en"); w.set_bool(4,1); w.end_document();
/* doc2 */ w.begin_document(); w.set_int(0,300); w.set_int(1,250); w.set_string(2,"beta");  w.add_string(3,"de"); w.set_bool(4,0); w.end_document();
/* doc3: created MISSING */ w.begin_document(); w.set_int(1,250); w.set_string(2,"beta"); w.add_string(3,"fr"); w.set_bool(4,1); w.end_document();
/* doc4: price+lang MISSING */ w.begin_document(); w.set_int(0,400); w.set_string(2,"gamma"); w.set_bool(4,0); w.end_document();
/* doc5: empty */ w.begin_document(); w.end_document();
CHECK(w.finish() == 0);
ANT_attribute_store *a = ANT_attribute_store::load(path, &s, DOCS);
CHECK(a && a->document_count() == DOCS);

const char *fr[1]={"fr"}; const char *en_de[2]={"en","de"};
long long inids[2]={100,400};
{ int e[DOCS]={0,0,1,0,0,0}; ANT_filter *f=ANT_filter::eq_int("created",300);           CHECK(f->build(&s)==0); expect_set(a,f,e,"eq_int");            delete f; }
{ int e[DOCS]={1,1,0,0,0,0}; ANT_filter *f=ANT_filter::eq_string("tenant","acme");       CHECK(f->build(&s)==0); expect_set(a,f,e,"eq_string");         delete f; }
{ int e[DOCS]={0,0,0,0,0,0}; ANT_filter *f=ANT_filter::eq_string("tenant","nope");        CHECK(f->build(&s)==0); expect_set(a,f,e,"eq_string_dictmiss"); delete f; }
{ int e[DOCS]={1,0,0,1,0,0}; ANT_filter *f=ANT_filter::in_string("lang",fr,1);            CHECK(f->build(&s)==0); expect_set(a,f,e,"in_string_fr");      delete f; }
{ int e[DOCS]={1,1,1,0,0,0}; ANT_filter *f=ANT_filter::in_string("lang",en_de,2);         CHECK(f->build(&s)==0); expect_set(a,f,e,"in_string_en_de");   delete f; }
{ int e[DOCS]={0,1,1,1,0,0}; ANT_filter *f=ANT_filter::range_int("price",100,1,250,1,1,1);CHECK(f->build(&s)==0); expect_set(a,f,e,"range_price");        delete f; }
{ int e[DOCS]={0,0,1,0,1,0}; ANT_filter *f=ANT_filter::range_int("created",300,1,0,0,1,1);CHECK(f->build(&s)==0); expect_set(a,f,e,"range_created_openhi"); delete f; }
{ int e[DOCS]={0,1,0,1,0,0}; ANT_filter *f=ANT_filter::eq_bool("archived",1);             CHECK(f->build(&s)==0); expect_set(a,f,e,"eq_bool");           delete f; }
/* in_int("created",{100,400}): doc0=100 matches, doc4=400 matches; doc1=200/doc2=300 don't; doc3/doc5 lack created -> false */
{ int e[DOCS]={1,0,0,0,1,0}; ANT_filter *f=ANT_filter::in_int("created",inids,2);         CHECK(f->build(&s)==0); expect_set(a,f,e,"in_int");            delete f; }

/* NOT / pinned missing-field semantics */
{ int e[DOCS]={0,0,1,1,1,1}; ANT_filter *f=ANT_filter::not_(ANT_filter::eq_string("tenant","acme")); CHECK(f->build(&s)==0); expect_set(a,f,e,"not_eq_string"); delete f; } /* complement of {0,1}; doc5 lacks tenant -> included */
{ int e[DOCS]={0,1,1,1,1,1}; ANT_filter *f=ANT_filter::not_(ANT_filter::eq_int("created",100));      CHECK(f->build(&s)==0); expect_set(a,f,e,"not_eq_int_missing"); delete f; } /* doc3,doc5 lack created -> EQ false -> NOT true */

/* AND / OR / empty */
{ int e[DOCS]={0,0,1,0,0,0}; ANT_filter *f=ANT_filter::and_(2, ANT_filter::eq_string("tenant","beta"), ANT_filter::eq_bool("archived",0)); CHECK(f->build(&s)==0); expect_set(a,f,e,"and"); delete f; }
{ int e[DOCS]={1,1,0,1,0,0}; ANT_filter *f=ANT_filter::or_(2, ANT_filter::eq_int("created",100), ANT_filter::eq_bool("archived",1)); CHECK(f->build(&s)==0); expect_set(a,f,e,"or"); delete f; }
{ int e[DOCS]={1,1,1,1,1,1}; ANT_filter *f=ANT_filter::and_(0); CHECK(f->build(&s)==0); expect_set(a,f,e,"and_empty_identity"); delete f; }
{ int e[DOCS]={0,0,0,0,0,0}; ANT_filter *f=ANT_filter::or_(0);  CHECK(f->build(&s)==0); expect_set(a,f,e,"or_empty"); delete f; }
{ int e[DOCS]={0,0,1,1,0,0}; ANT_filter *f=ANT_filter::and_(2, ANT_filter::range_int("price",100,1,0,0,1,1), ANT_filter::not_(ANT_filter::eq_string("tenant","acme"))); CHECK(f->build(&s)==0); expect_set(a,f,e,"and_range_not"); delete f; } /* price>=100 {1,2,3}; NOT acme {2,3,4,5}; AND {2,3} */

/* degraded: NULL store -> all zero even for NOT */
{ unsigned char bits[1]; memset(bits,0xAA,1); ANT_filter *f=ANT_filter::not_(ANT_filter::eq_int("created",100)); CHECK(f->build(&s)==0);
  CHECK(f->evaluate(NULL, DOCS, bits) == 0); CHECK(bits[0]==0); delete f; }

delete a; unlink(path);
printf("PASSED\n");
return 0;
}

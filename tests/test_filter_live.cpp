#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "filter.h"
#include "attribute_store.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
#define DOCS 6

/* assert evaluate(store) == evaluate_live(sets) byte-for-byte */
static void equiv(ANT_attribute_store *a, ANT_attribute_set *const *sets, ANT_filter *f, const char *label)
{
	unsigned char be[(DOCS+7)/8], bl[(DOCS+7)/8];
	memset(be,0xAA,sizeof(be)); memset(bl,0x55,sizeof(bl));
	CHECK(f->evaluate(a, DOCS, be) == 0);
	CHECK(f->evaluate_live(sets, DOCS, DOCS, bl) == 0);
	if (memcmp(be, bl, sizeof(be)) != 0) { printf("FAIL %s: store=%02x live=%02x\n", label, be[0], bl[0]); exit(1); }
}

int main(void)
{
ANT_attribute_schema s;
s.add_field("created", ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("price",   ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("tenant",  ANT_attribute_schema::TYPE_STRING, 0);
s.add_field("lang",    ANT_attribute_schema::TYPE_STRING, 1);
s.add_field("archived",ANT_attribute_schema::TYPE_BOOL, 0);

/* --- disk store (same doc table as test_filter_eval) --- */
char path[64]; strcpy(path,"/tmp/ant_fl_XXXXXX"); { int fd=mkstemp(path); if(fd>=0) close(fd); }
ANT_attribute_store_writer w; CHECK(w.create(path,&s)==0);
w.begin_document(); w.set_int(0,100); w.set_int(1,50);  w.set_string(2,"acme"); w.add_string(3,"en"); w.add_string(3,"fr"); w.set_bool(4,0); w.end_document();
w.begin_document(); w.set_int(0,200); w.set_int(1,150); w.set_string(2,"acme"); w.add_string(3,"en"); w.set_bool(4,1); w.end_document();
w.begin_document(); w.set_int(0,300); w.set_int(1,250); w.set_string(2,"beta"); w.add_string(3,"de"); w.set_bool(4,0); w.end_document();
w.begin_document(); w.set_int(1,250); w.set_string(2,"beta"); w.add_string(3,"fr"); w.set_bool(4,1); w.end_document();  /* created missing */
w.begin_document(); w.set_int(0,400); w.set_string(2,"gamma"); w.set_bool(4,0); w.end_document();                    /* price+lang missing */
w.begin_document(); w.end_document();                                                                               /* empty */
CHECK(w.finish()==0);
ANT_attribute_store *a = ANT_attribute_store::load(path,&s,DOCS); CHECK(a && a->document_count()==DOCS);

/* --- parallel live sets (identical values); doc5 is a NULL slot to exercise the missing path --- */
ANT_attribute_set *sets[DOCS];
sets[0]=new ANT_attribute_set(&s); sets[0]->set_int(0,100); sets[0]->set_int(1,50);  sets[0]->set_string(2,"acme"); sets[0]->add_string(3,"en"); sets[0]->add_string(3,"fr"); sets[0]->set_bool(4,0);
sets[1]=new ANT_attribute_set(&s); sets[1]->set_int(0,200); sets[1]->set_int(1,150); sets[1]->set_string(2,"acme"); sets[1]->add_string(3,"en"); sets[1]->set_bool(4,1);
sets[2]=new ANT_attribute_set(&s); sets[2]->set_int(0,300); sets[2]->set_int(1,250); sets[2]->set_string(2,"beta"); sets[2]->add_string(3,"de"); sets[2]->set_bool(4,0);
sets[3]=new ANT_attribute_set(&s); sets[3]->set_int(1,250); sets[3]->set_string(2,"beta"); sets[3]->add_string(3,"fr"); sets[3]->set_bool(4,1);
sets[4]=new ANT_attribute_set(&s); sets[4]->set_int(0,400); sets[4]->set_string(2,"gamma"); sets[4]->set_bool(4,0);
sets[5]=NULL;   /* empty doc == NULL set */

const char *fr[1]={"fr"}; const char *en_de[2]={"en","de"}; long long inids[2]={100,400};
ANT_filter *fs[16]; int nf=0;
fs[nf++]=ANT_filter::eq_int("created",300);
fs[nf++]=ANT_filter::eq_string("tenant","acme");
fs[nf++]=ANT_filter::eq_string("tenant","nope");
fs[nf++]=ANT_filter::in_string("lang",fr,1);
fs[nf++]=ANT_filter::in_string("lang",en_de,2);
fs[nf++]=ANT_filter::range_int("price",100,1,250,1,1,1);
fs[nf++]=ANT_filter::range_int("created",300,1,0,0,1,1);
fs[nf++]=ANT_filter::eq_bool("archived",1);
fs[nf++]=ANT_filter::in_int("created",inids,2);
fs[nf++]=ANT_filter::not_(ANT_filter::eq_string("tenant","acme"));
fs[nf++]=ANT_filter::not_(ANT_filter::eq_int("created",100));
fs[nf++]=ANT_filter::and_(2, ANT_filter::eq_string("tenant","beta"), ANT_filter::eq_bool("archived",0));
fs[nf++]=ANT_filter::or_(2, ANT_filter::eq_int("created",100), ANT_filter::eq_bool("archived",1));
fs[nf++]=ANT_filter::and_(2, ANT_filter::range_int("price",100,1,0,0,1,1), ANT_filter::not_(ANT_filter::eq_string("tenant","acme")));
for (int i=0;i<nf;i++){ CHECK(fs[i]->build(&s)==0); char lbl[16]; sprintf(lbl,"f%d",i); equiv(a,sets,fs[i],lbl); delete fs[i]; }

/* sets_count < documents: docs at/after the count are treated absent (all-NULL) -> equals a NOT filter over absent docs */
{ ANT_filter *f=ANT_filter::not_(ANT_filter::eq_int("created",100)); CHECK(f->build(&s)==0);
  unsigned char bl[(DOCS+7)/8]; memset(bl,0,sizeof(bl));
  CHECK(f->evaluate_live(sets, 3, DOCS, bl) == 0);   /* only sets[0..2] visible; docs 3,4,5 absent */
  /* docs 3,4,5 absent -> NOT true; doc0 created=100 -> NOT false; doc1,2 created!=100 -> NOT true */
  CHECK(((bl[0]>>0)&1)==0 && ((bl[0]>>1)&1)==1 && ((bl[0]>>2)&1)==1 && ((bl[0]>>3)&1)==1 && ((bl[0]>>4)&1)==1 && ((bl[0]>>5)&1)==1);
  delete f; }

/* sets == NULL entirely: every doc absent */
{ ANT_filter *f=ANT_filter::eq_int("created",100); CHECK(f->build(&s)==0);
  unsigned char bl[(DOCS+7)/8]; memset(bl,0xFF,sizeof(bl));
  CHECK(f->evaluate_live(NULL, 0, DOCS, bl) == 0);
  CHECK(bl[0]==0);   /* all absent -> eq matches nothing */
  delete f; }

for (int i=0;i<DOCS;i++) delete sets[i];
delete a; unlink(path);
printf("PASSED\n");
return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include "filter.h"
#define CHECK(c) do { if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)

int main(void)
{
ANT_attribute_schema s;
s.add_field("created", ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("price",   ANT_attribute_schema::TYPE_INT64, 0);
s.add_field("tenant",  ANT_attribute_schema::TYPE_STRING, 0);
s.add_field("lang",    ANT_attribute_schema::TYPE_STRING, 1);		/* multi-valued string */
s.add_field("archived",ANT_attribute_schema::TYPE_BOOL, 0);

/* well-typed trees build OK (return 0) */
ANT_filter *f1 = ANT_filter::eq_int("created", 1700000000LL);
CHECK(f1->build(&s) == 0);
CHECK(f1->node_kind() == ANT_filter::KIND_EQ_INT);
CHECK(f1->resolved_field() == 0);					/* "created" is field 0 */
delete f1;

const char *langs[2] = {"en", "fr"};
ANT_filter *f2 = ANT_filter::and_(3,
	ANT_filter::range_int("price", 100, 1, 500, 1, 1, 1),
	ANT_filter::in_string("lang", langs, 2),			/* IN on a multi-valued string field: valid */
	ANT_filter::not_(ANT_filter::eq_bool("archived", 1)));
CHECK(f2->build(&s) == 0);
delete f2;

long long ids[2] = {10, 20};
ANT_filter *f3 = ANT_filter::or_(2,
	ANT_filter::eq_string("tenant", "acme"),
	ANT_filter::in_int("price", ids, 2));
CHECK(f3->build(&s) == 0);
delete f3;

/* range with open upper bound (has_hi=0): valid */
ANT_filter *f4 = ANT_filter::range_int("created", 1700000000LL, 1, 0, 0, 1, 1);
CHECK(f4->build(&s) == 0);
delete f4;

/* ill-typed / unknown-field trees FAIL build (nonzero) */
ANT_filter *b1 = ANT_filter::eq_int("tenant", 5);			/* int leaf on string field */
CHECK(b1->build(&s) != 0); delete b1;

ANT_filter *b2 = ANT_filter::eq_string("created", "x");		/* string leaf on int field */
CHECK(b2->build(&s) != 0); delete b2;

ANT_filter *b3 = ANT_filter::range_int("tenant", 0, 1, 9, 1, 1, 1);	/* range on string field */
CHECK(b3->build(&s) != 0); delete b3;

ANT_filter *b4 = ANT_filter::eq_int("nonesuch", 1);			/* unknown field */
CHECK(b4->build(&s) != 0); delete b4;

ANT_filter *b5 = ANT_filter::eq_bool("created", 1);			/* bool leaf on int field */
CHECK(b5->build(&s) != 0); delete b5;

/* one bad leaf nested deep invalidates the whole tree */
ANT_filter *b6 = ANT_filter::and_(2,
	ANT_filter::eq_string("tenant", "acme"),
	ANT_filter::not_(ANT_filter::eq_int("tenant", 5)));		/* the NOT child is ill-typed */
CHECK(b6->build(&s) != 0); delete b6;

printf("PASSED\n");
return 0;
}

#ifndef ATTRIBUTE_STORE_H_
#define ATTRIBUTE_STORE_H_

class ANT_attribute_schema
{
public:
	enum { TYPE_INT64 = 0, TYPE_STRING = 1, TYPE_BOOL = 2 };
	enum { MAX_FIELDS = 64 };

private:
	long field_count;
	char names[MAX_FIELDS][64];
	int types[MAX_FIELDS];
	int multi[MAX_FIELDS];

public:
	ANT_attribute_schema() : field_count(0) {}
	long add_field(const char *name, int type, int multivalued);	// 0 ok; nonzero on dup name / bad type / multi-bool / overflow / long name
	long count(void) const { return field_count; }
	const char *name(long i) const { return names[i]; }
	int type(long i) const { return types[i]; }
	int is_multi(long i) const { return multi[i]; }
	long field_index(const char *name) const;						// -1 if absent
	long equals(const ANT_attribute_schema &o) const;				// 1 if identical (order+name+type+multi), else 0
} ;

#endif /* ATTRIBUTE_STORE_H_ */

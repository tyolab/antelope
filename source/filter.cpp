#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include "filter.h"

/*
	ANT_FILTER::ANT_FILTER()
	------------------------
*/
ANT_filter::ANT_filter(int kind)
{
this->kind = kind;
field_name[0] = '\0';
field_index = -1;

children = NULL;
child_count = 0;

int_value = 0;
bool_value = 0;
range_lo = 0;
range_hi = 0;
has_lo = 0;
has_hi = 0;
lo_incl = 0;
hi_incl = 0;

int_values = NULL;
int_values_count = 0;

string_values = NULL;
string_values_count = 0;
}

/*
	ANT_FILTER::~ANT_FILTER()
	--------------------------
*/
ANT_filter::~ANT_filter()
{
int i;

if (children != NULL)
	{
	for (i = 0; i < child_count; i++)
		delete children[i];
	delete [] children;
	}

if (int_values != NULL)
	delete [] int_values;

if (string_values != NULL)
	{
	for (i = 0; i < string_values_count; i++)
		delete [] string_values[i];
	delete [] string_values;
	}
}

/*
	Helper: bounded copy of a field name into a fixed buffer.
*/
static void ant_filter_copy_field(char *dest, size_t dest_size, const char *field)
{
strncpy(dest, field, dest_size - 1);
dest[dest_size - 1] = '\0';
}

/*
	Helper: heap-copy a NUL-terminated string.
*/
static char *ant_filter_strdup(const char *s)
{
size_t len;
char *copy;

len = strlen(s);
copy = new char[len + 1];
memcpy(copy, s, len + 1);
return copy;
}

/*
	ANT_FILTER::EQ_INT()
	---------------------
*/
ANT_filter *ANT_filter::eq_int(const char *field, long long value)
{
ANT_filter *node = new ANT_filter(KIND_EQ_INT);
ant_filter_copy_field(node->field_name, sizeof(node->field_name), field);
node->int_value = value;
return node;
}

/*
	ANT_FILTER::EQ_STRING()
	------------------------
*/
ANT_filter *ANT_filter::eq_string(const char *field, const char *value)
{
ANT_filter *node = new ANT_filter(KIND_EQ_STRING);
ant_filter_copy_field(node->field_name, sizeof(node->field_name), field);
node->string_values = new char *[1];
node->string_values[0] = ant_filter_strdup(value);
node->string_values_count = 1;
return node;
}

/*
	ANT_FILTER::EQ_BOOL()
	----------------------
*/
ANT_filter *ANT_filter::eq_bool(const char *field, int value)
{
ANT_filter *node = new ANT_filter(KIND_EQ_BOOL);
ant_filter_copy_field(node->field_name, sizeof(node->field_name), field);
node->bool_value = value;
return node;
}

/*
	ANT_FILTER::RANGE_INT()
	------------------------
*/
ANT_filter *ANT_filter::range_int(const char *field, long long lo, int has_lo, long long hi, int has_hi, int lo_incl, int hi_incl)
{
ANT_filter *node = new ANT_filter(KIND_RANGE_INT);
ant_filter_copy_field(node->field_name, sizeof(node->field_name), field);
node->range_lo = lo;
node->range_hi = hi;
node->has_lo = has_lo;
node->has_hi = has_hi;
node->lo_incl = lo_incl;
node->hi_incl = hi_incl;
return node;
}

/*
	ANT_FILTER::IN_INT()
	---------------------
*/
ANT_filter *ANT_filter::in_int(const char *field, const long long *values, int n)
{
int i;
ANT_filter *node = new ANT_filter(KIND_IN_INT);
ant_filter_copy_field(node->field_name, sizeof(node->field_name), field);
node->int_values = new long long[n];
for (i = 0; i < n; i++)
	node->int_values[i] = values[i];
node->int_values_count = n;
return node;
}

/*
	ANT_FILTER::IN_STRING()
	-------------------------
*/
ANT_filter *ANT_filter::in_string(const char *field, const char *const *values, int n)
{
int i;
ANT_filter *node = new ANT_filter(KIND_IN_STRING);
ant_filter_copy_field(node->field_name, sizeof(node->field_name), field);
node->string_values = new char *[n];
for (i = 0; i < n; i++)
	node->string_values[i] = ant_filter_strdup(values[i]);
node->string_values_count = n;
return node;
}

/*
	ANT_FILTER::AND_()
	-------------------
*/
ANT_filter *ANT_filter::and_(int n, ...)
{
int i;
va_list args;
ANT_filter *node = new ANT_filter(KIND_AND);
node->children = new ANT_filter *[n];
node->child_count = n;

va_start(args, n);
for (i = 0; i < n; i++)
	node->children[i] = va_arg(args, ANT_filter *);
va_end(args);

return node;
}

/*
	ANT_FILTER::OR_()
	------------------
*/
ANT_filter *ANT_filter::or_(int n, ...)
{
int i;
va_list args;
ANT_filter *node = new ANT_filter(KIND_OR);
node->children = new ANT_filter *[n];
node->child_count = n;

va_start(args, n);
for (i = 0; i < n; i++)
	node->children[i] = va_arg(args, ANT_filter *);
va_end(args);

return node;
}

/*
	ANT_FILTER::NOT_()
	-------------------
*/
ANT_filter *ANT_filter::not_(ANT_filter *child)
{
ANT_filter *node = new ANT_filter(KIND_NOT);
node->children = new ANT_filter *[1];
node->children[0] = child;
node->child_count = 1;
return node;
}

/*
	ANT_FILTER::BUILD()
	--------------------
	Resolve field names to indices and type-check the whole tree.  Returns
	0 iff every leaf's field exists in the schema and its value type
	matches the field's declared type.  Recurses through AND/OR/NOT.
*/
long ANT_filter::build(const ANT_attribute_schema *schema)
{
int i;
int required_type;

switch (kind)
	{
	case KIND_AND:
	case KIND_OR:
		for (i = 0; i < child_count; i++)
			if (children[i]->build(schema) != 0)
				return 1;
		return 0;

	case KIND_NOT:
		return children[0]->build(schema);

	case KIND_EQ_INT:
	case KIND_RANGE_INT:
	case KIND_IN_INT:
		required_type = ANT_attribute_schema::TYPE_INT64;
		break;

	case KIND_EQ_STRING:
	case KIND_IN_STRING:
		required_type = ANT_attribute_schema::TYPE_STRING;
		break;

	case KIND_EQ_BOOL:
		required_type = ANT_attribute_schema::TYPE_BOOL;
		break;

	default:
		return 1;
	}

field_index = schema->field_index(field_name);
if (field_index < 0)
	return 1;

if (schema->type(field_index) != required_type)
	return 1;

return 0;
}

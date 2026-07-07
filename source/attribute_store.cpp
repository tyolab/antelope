/*
	ATTRIBUTE_STORE.CPP
	-------------------
	Schema value-type describing filterable document attribute fields.
	See attribute_store.h.  Later tasks extend this file with the actual
	per-document attribute storage / filter evaluation.
*/
#include <string.h>
#include "attribute_store.h"

/*
	ANT_ATTRIBUTE_SCHEMA::FIELD_INDEX()
	------------------------------------
*/
long ANT_attribute_schema::field_index(const char *name) const
{
long i;

for (i = 0; i < field_count; i++)
	if (strcmp(names[i], name) == 0)
		return i;
return -1;
}

/*
	ANT_ATTRIBUTE_SCHEMA::ADD_FIELD()
	------------------------------------
*/
long ANT_attribute_schema::add_field(const char *name, int type, int multivalued)
{
if (type != TYPE_INT64 && type != TYPE_STRING && type != TYPE_BOOL)
	return 1;
if (multivalued && type == TYPE_BOOL)
	return 1;
if (field_count >= MAX_FIELDS)
	return 1;
if (name == NULL || strlen(name) >= sizeof(names[0]))
	return 1;
if (field_index(name) >= 0)
	return 1;

strcpy(names[field_count], name);
types[field_count] = type;
multi[field_count] = multivalued;
field_count++;
return 0;
}

/*
	ANT_ATTRIBUTE_SCHEMA::EQUALS()
	---------------------------------
*/
long ANT_attribute_schema::equals(const ANT_attribute_schema &o) const
{
long i;

if (field_count != o.field_count)
	return 0;
for (i = 0; i < field_count; i++)
	if (strcmp(names[i], o.names[i]) != 0 || types[i] != o.types[i] || multi[i] != o.multi[i])
		return 0;
return 1;
}

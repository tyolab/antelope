#ifndef ATTRIBUTE_STORE_H_
#define ATTRIBUTE_STORE_H_

#include <stdint.h>

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

/*
	class ANT_ATTRIBUTE_STORE
	--------------------------
	Read-only columnar loader for the per-segment .attr sidecar (see the
	on-disk layout comment in attribute_store.cpp).  Any validation failure
	in load() degrades to an empty store (document_count() == 0) rather than
	crashing -- the same forgiving posture as the vector/multivector stores.

	Every matcher returns 0 when the document lacks the field (presence bit
	0) -- "missing implies leaf false".  Multi-valued matchers are CONTAINS
	semantics: true iff ANY of the document's values satisfies the predicate.
*/
class ANT_attribute_store
{
private:
	ANT_attribute_schema schema;
	long long documents;

	unsigned char *presence[ANT_attribute_schema::MAX_FIELDS];		// presence bitset per field, NULL if unused

	long long *int_single[ANT_attribute_schema::MAX_FIELDS];			// INT64 single: [documents]
	int32_t *int_counts[ANT_attribute_schema::MAX_FIELDS];			// INT64 multi: [documents]
	long long *int_offsets[ANT_attribute_schema::MAX_FIELDS];		// INT64 multi: [documents+1]
	long long *int_pool[ANT_attribute_schema::MAX_FIELDS];			// INT64 multi: [offsets[documents]]

	unsigned char *bool_bits[ANT_attribute_schema::MAX_FIELDS];		// BOOL single: value bitset

	int32_t *str_single[ANT_attribute_schema::MAX_FIELDS];			// STRING single: dict id [documents]
	int32_t *str_counts[ANT_attribute_schema::MAX_FIELDS];			// STRING multi: [documents]
	long long *str_offsets[ANT_attribute_schema::MAX_FIELDS];		// STRING multi: [documents+1]
	int32_t *str_pool[ANT_attribute_schema::MAX_FIELDS];				// STRING multi: dict ids [offsets[documents]]

	char *dict_pool[ANT_attribute_schema::MAX_FIELDS];				// STRING fields only: concatenated bytes
	long long *dict_offset[ANT_attribute_schema::MAX_FIELDS];		// STRING fields only: [dict_count+1]
	long long dict_count[ANT_attribute_schema::MAX_FIELDS];			// STRING fields only

	ANT_attribute_store();

public:
	~ANT_attribute_store();
	static ANT_attribute_store *load(const char *filename, const ANT_attribute_schema *schema, long long expected_documents);
	long long document_count(void);
	long has_field(long field, long long docid);						// presence bit (0/1)
	// single-valued convenience:
	long get_int(long field, long long docid, long long *out);			// 0 if absent; else 1 + *out
	long get_bool(long field, long long docid, int *out);				// 0 if absent; else 1 + *out
	// dictionary + matchers (used by the filter):
	long string_id(long field, const char *literal);					// dict id in this field; <0 if not present
	long string_matches(long field, long long docid, long want_id);		// doc's string field (single OR multi) contains want_id; 0 if absent or want_id<0
	long int_equals(long field, long long docid, long long want);		// any value == want; 0 if absent
	long int_matches_range(long field, long long docid, long long lo, long long hi, int lo_incl, int hi_incl);	// any value in [lo,hi] per inclusivity; 0 if absent
	long bool_equals(long field, long long docid, int want);			// value == want; 0 if absent
	// enumeration (for compaction Task 13 + string-range later):
	long long value_count(long field, long long docid);					// #values (0 absent; 1 for single present)
	long get_int_at(long field, long long docid, long long idx, long long *out);
	long get_string_at(long field, long long docid, long long idx, char *out, long long out_size);	// copies value idx's string (single or multi); 0 if absent/idx OOB
} ;

/*
	class ANT_ATTRIBUTE_STORE_WRITER
	----------------------------------
	Buffered writer with atomic finish (write .tmp, rename).  Buffers grow
	geometrically; strings are interned into a per-field dictionary (first
	occurrence order == on-disk id).
*/
class ANT_attribute_store_writer
{
private:
	const ANT_attribute_schema *schema;
	char filename[4096];
	long long documents;			// documents committed via end_document()
	long long capacity;			// capacity (in documents) of the per-doc arrays below

	unsigned char *presence[ANT_attribute_schema::MAX_FIELDS];

	long long *int_single[ANT_attribute_schema::MAX_FIELDS];
	int32_t *int_counts[ANT_attribute_schema::MAX_FIELDS];
	long long *int_pool[ANT_attribute_schema::MAX_FIELDS];
	long long int_pool_size[ANT_attribute_schema::MAX_FIELDS];
	long long int_pool_capacity[ANT_attribute_schema::MAX_FIELDS];

	unsigned char *bool_bits[ANT_attribute_schema::MAX_FIELDS];

	int32_t *str_single[ANT_attribute_schema::MAX_FIELDS];
	int32_t *str_counts[ANT_attribute_schema::MAX_FIELDS];
	int32_t *str_pool[ANT_attribute_schema::MAX_FIELDS];
	long long str_pool_size[ANT_attribute_schema::MAX_FIELDS];
	long long str_pool_capacity[ANT_attribute_schema::MAX_FIELDS];

	char *dict_pool[ANT_attribute_schema::MAX_FIELDS];
	long long dict_pool_size[ANT_attribute_schema::MAX_FIELDS];
	long long dict_pool_capacity[ANT_attribute_schema::MAX_FIELDS];
	long long *dict_offset[ANT_attribute_schema::MAX_FIELDS];		// [dict_count+1]
	long long dict_count[ANT_attribute_schema::MAX_FIELDS];
	long long dict_offset_capacity[ANT_attribute_schema::MAX_FIELDS];

private:
	long grow_docs(void);
	long intern(long field, const char *literal);

public:
	ANT_attribute_store_writer();
	~ANT_attribute_store_writer();

	long create(const char *path, const ANT_attribute_schema *schema);	// 0 ok; resets state
	void begin_document(void);					// start a new doc (docid = documents so far)
	void set_int(long field, long long value);	// single-valued int
	void add_int(long field, long long value);	// multi-valued int (append)
	void set_string(long field, const char *value);
	void add_string(long field, const char *value);
	void set_bool(long field, int value);
	void end_document(void);					// commit the doc (documents++)
	long finish(void);							// atomic temp+rename; 0 ok
	void abandon(void);
} ;

/*
	class ANT_ATTRIBUTE_SET
	-------------------------
	A caller-filled, per-document typed attribute builder plus an opaque
	payload blob.  Deep-copyable (clone()) and enumerable so the NRT writer
	can capture a snapshot at add_document() time and later drain it to the
	.attr / payload sidecars at flush.  Setters that reference an out-of-range
	field index (or a NULL schema) are silent no-ops -- the real type checking
	happened at schema build time.
*/
class ANT_attribute_set
{
	const ANT_attribute_schema *schema;
	int present_flag[ANT_attribute_schema::MAX_FIELDS];
	long long *int_vals[ANT_attribute_schema::MAX_FIELDS];
	long int_count[ANT_attribute_schema::MAX_FIELDS];
	long int_cap[ANT_attribute_schema::MAX_FIELDS];
	char **str_vals[ANT_attribute_schema::MAX_FIELDS];
	long str_count[ANT_attribute_schema::MAX_FIELDS];
	long str_cap[ANT_attribute_schema::MAX_FIELDS];
	int bool_vals[ANT_attribute_schema::MAX_FIELDS];
	unsigned char *payload;
	long long payload_len;
public:
	ANT_attribute_set(const ANT_attribute_schema *schema);
	~ANT_attribute_set();
	// setters (field = schema field index; out-of-range or type-mismatched field is a silent no-op):
	void set_int(long field, long long value);		// single: clears then holds exactly this value
	void add_int(long field, long long value);		// multi: append
	void set_string(long field, const char *value);	// single: clears then holds exactly this value
	void add_string(long field, const char *value);	// multi: append
	void set_bool(long field, int value);
	void set_payload(const void *ptr, long long len);	// copies len bytes; len 0 / ptr NULL clears
	// accessors (Task 7 + test hooks):
	const ANT_attribute_schema *get_schema(void) const { return schema; }
	int has(long field) const;						// present_flag (0/1)
	long present_field_count(void) const;			// number of fields with present_flag set
	long ints(long field) const;					// # int values held
	long long int_get(long field, long i) const;
	long strings(long field) const;					// # string values held
	const char *string_get(long field, long i) const;
	int boolean(long field) const;					// bool value (0/1)
	const unsigned char *payload_bytes(long long *len) const;	// (ptr,len); (NULL,0) if none
	ANT_attribute_set *clone(void) const;			// deep heap copy
} ;

#endif /* ATTRIBUTE_STORE_H_ */

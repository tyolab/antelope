#ifndef FILTER_H_
#define FILTER_H_

#include "attribute_store.h"

class ANT_filter
{
public:
	enum { KIND_AND, KIND_OR, KIND_NOT, KIND_EQ_INT, KIND_EQ_STRING, KIND_EQ_BOOL,
	       KIND_RANGE_INT, KIND_IN_INT, KIND_IN_STRING };
private:
	int kind;
	char field_name[64];			// leaves only
	long field_index;				// resolved by build(); -1 until then
	// boolean:
	ANT_filter **children;
	int child_count;
	// leaf payloads:
	long long int_value;			// EQ_INT
	int bool_value;					// EQ_BOOL (0/1)
	long long range_lo, range_hi;	// RANGE_INT
	int has_lo, has_hi, lo_incl, hi_incl;
	long long *int_values; int int_values_count;		// IN_INT
	char **string_values; int string_values_count;		// EQ_STRING (1) / IN_STRING (n)
	ANT_filter(int kind);
	// Task 5/9: recursive evaluator; exactly one of (attrs) / (sets) drives the leaves.
	// store mode: attrs != NULL, sets == NULL.  set mode: attrs == NULL, sets drives leaves.
	long evaluate_node(ANT_attribute_store *attrs, ANT_attribute_set *const *sets, long long sets_count, long long documents, unsigned char *out_bits) const;
	// set-mode leaf test: does this leaf match the given (possibly NULL) captured set?
	long set_leaf_match(ANT_attribute_set *set) const;
public:
	~ANT_filter();
	// factories (all heap-allocate; caller owns the returned root and deletes it):
	static ANT_filter *eq_int(const char *field, long long value);
	static ANT_filter *eq_string(const char *field, const char *value);
	static ANT_filter *eq_bool(const char *field, int value);
	static ANT_filter *range_int(const char *field, long long lo, int has_lo, long long hi, int has_hi, int lo_incl, int hi_incl);
	static ANT_filter *in_int(const char *field, const long long *values, int n);
	static ANT_filter *in_string(const char *field, const char *const *values, int n);
	static ANT_filter *and_(int n, ...);		// n child pointers, ownership transferred to the AND node
	static ANT_filter *or_(int n, ...);
	static ANT_filter *not_(ANT_filter *child);	// ownership transferred
	long build(const ANT_attribute_schema *schema);		// 0 = ok (well-typed), nonzero = type/field error
	// Task 5 implements this — declared only:
	long evaluate(ANT_attribute_store *attrs, long long documents, unsigned char *out_bits) const;
	// Task 9: evaluate over live captured sets (unflushed NRT docs).  For doc d the set is
	// (sets && d < sets_count) ? sets[d] : NULL (NULL = doc has no attributes = every leaf false).
	// No degraded short-circuit -- the sets ARE ground truth.
	long evaluate_live(ANT_attribute_set *const *sets, long long sets_count, long long documents, unsigned char *out_bits) const;
	// introspection (for tests + Task 5):
	int node_kind(void) const { return kind; }
	long resolved_field(void) const { return field_index; }
} ;

#endif

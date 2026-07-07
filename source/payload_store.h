#ifndef PAYLOAD_STORE_H_
#define PAYLOAD_STORE_H_

#include <stdio.h>

/*
	class ANT_PAYLOAD_STORE
	------------------------
	Read-only loader for the per-segment .pay ragged opaque-blob sidecar
	(see the on-disk layout comment in payload_store.cpp).  Payload bytes
	are never indexed or filtered -- they are returned verbatim alongside a
	search hit.  Any validation failure in load() degrades to an empty
	store (document_count() == 0) rather than crashing, the same forgiving
	posture as attribute_store / vector_store / multivector_store.
*/
class ANT_payload_store
{
	long long documents;
	long long total_bytes;
	long long *offsets;			// documents+1 entries, or NULL when empty
	unsigned char *pool;		// total_bytes, or NULL when empty
	ANT_payload_store();
public:
	~ANT_payload_store();
	static ANT_payload_store *load(const char *filename, long long expected_documents);
	long long document_count(void) { return documents; }
	void get(long long docid, const unsigned char **out_ptr, long long *out_len);	// (NULL,0) if absent/empty/OOB
} ;

/*
	class ANT_PAYLOAD_STORE_WRITER
	--------------------------------
	Buffered writer with atomic finish (write .tmp, rename).  Offsets and
	the byte pool grow geometrically in memory as append() is called.
*/
class ANT_payload_store_writer
{
	FILE *fp;
	char *tempname;
	long long documents;
	long long *offsets;			// growable, offsets[0..documents]
	long long offsets_capacity;
	unsigned char *pool;		// growable byte pool
	long long pool_size;
	long long pool_capacity;
public:
	ANT_payload_store_writer();
	~ANT_payload_store_writer();
	long create(const char *path);							// 0 ok; resets state, records final path
	void append(const void *ptr, long long len);			// append one doc's payload (len 0 / ptr NULL allowed = empty doc)
	long finish(void);										// write header+offsets+pool atomically (temp+rename); 0 ok
	void abandon(void);										// discard, remove temp if any
} ;

#endif /* PAYLOAD_STORE_H_ */

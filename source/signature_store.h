/*
	SIGNATURE_STORE.H -- per-segment binary-signature sidecar (seg_G.vsig),
	mirroring ANT_vector_store's shape and forgiving-load posture.  On-disk:
	  uint64 magic ("ANTSIG01") | uint32 version | int64 bits | int64 document_count
	  byte[] presence bitmap ((count+7)/8) | byte[] signatures (count*bits/8)
	Any load failure (magic/version/bits/count/size) degrades to an empty store.
*/
#ifndef SIGNATURE_STORE_H_
#define SIGNATURE_STORE_H_

class ANT_index_tombstones;

class ANT_signature_store
{
private:
	long long bits;
	long long documents;
	unsigned char *presence;			// NULL when degraded/empty
	unsigned char *signatures;			// NULL when degraded/empty

	ANT_signature_store();

public:
	~ANT_signature_store();

	static ANT_signature_store *load(const char *filename, long long expected_bits, long long expected_documents);

	long long document_count(void) { return documents; }
	long long signature_bytes(void) { return (bits + 7) / 8; }
	long has(long long docid) { return presence != NULL && (presence[docid / 8] & (1 << (docid % 8))) != 0; }
	const unsigned char *get(long long docid) { return signatures + docid * signature_bytes(); }

	/*
		Fill out_docids[0..*out_count) with up to pool_size smallest-Hamming
		present, non-tombstoned docids for query_signature (order unspecified).
	*/
	void shortlist(const unsigned char *query_signature, ANT_index_tombstones *tombstones, long long pool_size, long long *out_docids, long long *out_count);
} ;

class ANT_signature_store_writer
{
private:
	char filename[4096];
	long long bits;
	long long documents;
	long long capacity;
	unsigned char *presence;
	unsigned char *signatures;

	long long sig_bytes(void) { return (bits + 7) / 8; }
	long grow(void);

public:
	ANT_signature_store_writer();
	~ANT_signature_store_writer();

	long create(const char *filename, long long bits);		// 0 on success
	long append(const unsigned char *signature_or_null);	// 0 on success
	long finish(void);										// write + rename; 0 on success
	void abandon(void);
} ;

#endif /* SIGNATURE_STORE_H_ */

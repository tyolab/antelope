/*
	WAL.H
	-----
	Write-ahead log for the segmented index's memory segment (see
	docs/superpowers/specs/2026-07-06-lexical-phase3-design.md section 1).

	Record format (binary, little-endian, length-prefixed):
		uint8  op ('A' | 'U' | 'D')
		int32  key_length      + key bytes            (1..8192)
		int64  document_length + document bytes       (0..256MB; 0 for 'D')
		uint8  has_vector      + dimension float32s when 1

	Appends are fflush()ed per record (optionally fsync()ed).  Replay stops
	at the first short read or out-of-bounds field: the torn-tail rule.  A
	failed append marks the log unhealthy; the engine state remains the
	source of truth and the next truncate() restores health.
*/
#ifndef WAL_H_
#define WAL_H_

#include <stdio.h>
#include <stdint.h>

class ANT_write_ahead_log
{
public:
	struct record
	{
	char op;
	char *key;			// owned by the log object; valid until the next replay_next/destruction
	char *document;		// NULL for 'D'
	float *vector;		// NULL when absent
	} ;

private:
	FILE *fp;			// open "a+b": append writes, seekable reads
	long long dimension;	// 0 = vectors disabled
	long is_healthy;
	long use_fsync;
	long long replay_position;
	char *key_buffer;
	long long key_buffer_size;
	char *document_buffer;
	long long document_buffer_size;
	float *vector_buffer;
	char *directory;		// path to the directory containing wal.log

private:
	ANT_write_ahead_log();

public:
	~ANT_write_ahead_log();

	static ANT_write_ahead_log *open(const char *directory, long long vector_dimension);	// NULL only on unopenable file
	long append(char op, const char *key, const char *document_or_null, const float *vector_or_null);	// 0 on success
	long replay_next(record *into);			// 1 = record produced; 0 = end (clean or torn)
	long truncate(void);					// empty the log; restores health; 0 on success
	long healthy(void) { return is_healthy; }
	void set_fsync(long on) { use_fsync = on; }
	long long size(void);					// current byte size (tests/diagnostics)
} ;

#endif /* WAL_H_ */

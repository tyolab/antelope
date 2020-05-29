/*
	INSTREAM_BZ2.C
	--------------
*/

#ifdef ANT_HAS_BZLIB
	#include "bzlib.h"
#endif

#include "instream_bz2.h"
#include "instream_bz2_internals.h"

/*
	ANT_INSTREAM_BZ2W::ANT_INSTREAM_BZ2()
	-------------------------------------
*/
ANT_instream_bz2::ANT_instream_bz2(ANT_memory *memory, ANT_instream *source) : ANT_instream(memory, source)
{
#ifdef ANT_HAS_BZLIB
	total_written = total_read = 0;
	internals = new (memory) ANT_instream_bz2_internals;

	internals->stream.bzalloc 	   = NULL;
	internals->stream.bzfree       = NULL;
	internals->stream.opaque       = NULL;
	internals->stream.avail_in     = 0;
	internals->stream.next_in      = NULL;
	buffer = NULL;
#else
	exit(printf("You are trying to decompress a bz2 file but BZLIB is not included in this build"));
#endif
}

/*
	ANT_INSTREAM_BZ2::~ANT_INSTREAM_BZ2()
	-------------------------------------
*/
ANT_instream_bz2::~ANT_instream_bz2()
{
#ifdef ANT_HAS_BZLIB
	BZ2_bzDecompressEnd(&internals->stream);
#endif
}

/*
	ANT_INSTREAM_BZ2::READ()
	------------------------
*/
long long ANT_instream_bz2::read(unsigned char *data, long long size)
{
#ifdef ANT_HAS_BZLIB
	long long got;
	long state;
	long long bytes_read;
	char* new_data;

	if (size == 0)
		return 0;

	if (buffer == NULL)
		{
		buffer = (unsigned char *)memory->malloc(buffer_length);
		if (BZ2_bzDecompressInit(&internals->stream, 0, 0) != BZ_OK)
			return -1;		// error
		}

	internals->stream.avail_out = (unsigned int)size;
	internals->stream.next_out = (char *)data;

	do
		{
		if (internals->stream.avail_in <= 0)
			{
			if ((got = source->read(buffer, buffer_length)) < 0)
				return -1;			// the instream is at EOF and so we are too

			buffer_read = got;

			internals->stream.avail_in = (unsigned int)got;
			internals->stream.next_in = (char *)buffer;	
			}

		state = BZ2_bzDecompress(&internals->stream);

		if (state != BZ_OK && state != BZ_STREAM_END)
			break;

		buffer_left = internals->stream.avail_in;

		if (state == BZ_STREAM_END)
			{
			got = size - internals->stream.avail_out;		// number of bytes that were decompressed
			total_written += got;

			// sometimes bzip2 files are joined together with STREAM_ENDs in between
			// so we will continue reading util we get an error
			if (buffer_left > 0)
				{
				// not finished yet
				internals->stream.bzalloc 	   = NULL;
				internals->stream.bzfree       = NULL;
				internals->stream.opaque       = NULL;
				internals->stream.avail_in     = 0;
				internals->stream.next_in      = NULL;
				if (BZ2_bzDecompressInit(&internals->stream, 0, 0) != BZ_OK)
					return -1;
				bytes_read = buffer_read - buffer_left;
				// not to worry, the "next_in" will be there, and waiting for reading in
				internals->stream.avail_in = buffer_left;
				internals->stream.next_in = (char *)buffer + bytes_read;  
				internals->stream.avail_out = (unsigned int)size - got;
				internals->stream.next_out = (char *) data + got;				
				state = BZ_OK;
				}
			else 
				{
				return got;			// at EOF
				}
			}

		if (internals->stream.avail_out == 0)
			{
			total_written += size;
			return size;			// filled the output buffer and so return bytes read
			}
		}
	while (state == BZ_OK);

	return -1;			// something has gone wrong
#else
	return -1;
#endif
}


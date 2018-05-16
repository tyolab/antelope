/*
	CHANNEL_MEMORY.H
	--------------
*/
#ifndef CHANNEL_MEMORY_H_
#define CHANNEL_MEMORY_H_

#include <stdio.h>
#include "channel.h"

class ANT_instream;
class ANT_memory;

/*
	class ANT_CHANNEL_MEMORY
	----------------------
*/
class ANT_channel_memory : public ANT_channel
{
protected:
	static const long memory_size = (16 * 1024);

private:
	char *memory;
	long position;
	
protected:
	virtual long long block_write(char *source, long long length);
	virtual char *block_read(char *into, long long length);
	virtual char *getsz(char terminator = '\0');

public:
	ANT_channel_memory();
	virtual ~ANT_channel_memory();
} ;

#endif /* CHANNEL_MEMORY_H_ */

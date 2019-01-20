/*
	CHANNEL_FILE.C
	--------------
*/
#include "channel_memory.h"

/*
	ANT_CHANNEL_FILE::ANT_CHANNEL_FILE()
	------------------------------------
	if filename == NULL then use stdin and stout else use the named file
*/
ANT_channel_memory::ANT_channel_memory()
{
memory = new char[(memory_size)];
position = 0;
}

/*
	ANT_CHANNEL_FILE::~ANT_CHANNEL_FILE()
	-------------------------------------
*/
ANT_channel_memory::~ANT_channel_memory()
{
delete [] memory;
}

/*
	ANT_CHANNEL_FILE::BLOCK_WRITE()
	-------------------------------
*/
long long ANT_channel_memory::block_write(char *source, long long length)
{
memcpy(memory + position, source, length);
position += length;
return length;
}

/*
	ANT_CHANNEL_FILE::BLOCK_READ()
	------------------------------
*/
char *ANT_channel_memory::block_read(char *into, long long length)
{
long max_len = length > memory_size ? memory_size : length;
memcpy(into, memory, max_len);
return NULL;
}

/*
	ANT_CHANNEL_FILE::GETSZ()
	-------------------------
*/
char *ANT_channel_memory::getsz(char terminator)
{
long length = position + 1;
memory[length] = terminator;
position = 0;
return memory;
}


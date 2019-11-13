/*
	INDEX.C
	-------
*/
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "memory.h"
#include "indexer_param_block.h"
#include "indexer.h"

#ifndef FALSE
	#define FALSE 0
#endif
#ifndef TRUE
	#define TRUE (!FALSE)
#endif

int atire_index(int argc, char *argv[]);
int atire_index(char *options);

#ifndef ATIRE_LIBRARY

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
return atire_index(argc, argv);
}

#else

// do nothing
int atire_exit(int errno) {
	return errno;
}

#define exit(x) (x)

#endif

/*
	ATIRE_INDEX()
	-------------
	for the simplicity of JNI calling
	options are separated with +
*/
int atire_index(char *options)
{
static const char *seperators = "+";
char **argv, **file_list;
char *token;
size_t total_length = (options ? strlen(options) : 0) + 7;
char *copy, *copy_start;

copy = copy_start = new char[total_length];

memset(copy, 0, sizeof(*copy) * total_length);

memcpy(copy, "index+", 6);
copy += 6;
if (options) {
	memcpy(copy, options, strlen(options));
	copy += strlen(options);
}
*copy = '\0';

argv = file_list = new char *[total_length];
int argc = 0;
token = strtok(copy_start, seperators);

#ifdef DEBUG
	fprintf(stderr, "Start indexing with options: %s\n", options);
#endif
for (; token != NULL; token = strtok(NULL, seperators))
	{
#ifdef DEBUG
	fprintf(stderr, "%s ", token);
#endif
	*argv++ = token;
	++argc;
	}
#ifdef DEBUG
	fprintf(stderr, "\n");
#endif
*argv = NULL;
int result = atire_index(argc, file_list);
delete [] copy_start;
delete [] file_list;

return result;
}

/*
	ATIRE_INDEX()
	-------------
*/
int atire_index(int argc, char *argv[])
{
	
if (argc < 2) 
	{
	ANT_indexer_param_block param_block(argc, argv);
	param_block.usage();
	}

ATIRE_indexer indexer;
indexer.init(argc, argv);
indexer.index();


return 0;
}


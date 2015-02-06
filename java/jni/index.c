#include "index.h"

#include <string.h>

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
	int ret = 0;

	if (argc == 2 && strchr(argv[1], ' ') != NULL)
		atire_index(argv[1]);
	else
		atire_index(argc, argv);

	return ret;
}

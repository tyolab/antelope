#include "atire.h"

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
	int ret = 0;

	if (argc == 2)
		run_atire(argv[1]);
	else
		run_atire(argc, argv);

	return ret;
}

/*
	ATIRE.C
	-------
*/
#include "atire_api_server.h"

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
ATIRE_API_server server;
return server.run(argc, argv);
}


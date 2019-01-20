/*
	ATIRE.C
	-------
*/
#include <stdio.h>

#include "atire_api_server.h"

/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
ATIRE_API_server server;

server.set_params(argc, argv);

int ret = 0;

/**
 * Option 1)
 */
// ret = server.run();

/**
 * Option 2)
 */
/*
   Test the channel memory
 */
// server.set_outchannel(ATIRE_API_server::CHANNEL_MEMORY);
server.initialize();

server.start_stats();

server.start();

server.prompt();
server.poll();
for (; server.has_new_command() && !server.is_interrupted(); server.prompt(), server.poll())
	{
	server.process_command();

	// if the outchannel is in memory we need to output to stdout
	puts(server.get_outchannel_content());

	if (server.is_interrupted())
		break;
	}

server.finish();

return ret; 
}


var antelope = require('./build/Release/antelope_api.node');

// if you want to use a debug copy of the library
// var antelope = require('./build/Debug/antelope_api');

var server = new antelope.ATIRE_API_server();

// don't have to set it when we don't have one
server.set_params("-nologo");

server.initialize();

server.start();


/**
 * Print out Reach Console version 
 */

function version () {
	console.log(server.version());
}

/**
 * Check if the input is an empty string 
 */

function isEmpty(str) {
    return (!str || /^\s*$/.test(str));
}

/**
 * Do what need to be done on exit 
 */

function onExit() {
	
	process.stdin.destroy();
	process.exit(0);
}

server.prompt();
server.poll();

for (; server.has_new_command() && !server.is_interrupted(); server.prompt(), server.poll())
	{
	server.process_command();

	if (server.is_interrupted())
		break;
	}

server.finish();



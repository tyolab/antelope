var restify = require('restify');

var onExit = require('death'); //this is intentionally ugly

var atire = require('./build/Release/atire_api');
// var atire = require('./build/Debug/atire_api');

// RESTIFY Server
var restServer = restify.createServer();

// ATIRE Server
var server = new atire.ATIRE_API_server();

/* no embeded loop */
//server.loop();
function respond(req, res, next) {
    // the query req.params.query
    server.insert_command(req.params.query);
    server.process_command();
    res.send('hello ' + req.params.query);
  next();
}

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
function finalize () {
    // clean up resources
    server.finish();

	process.stdin.destroy();
	process.exit(0);
}

onExit(() => {
    finalize();
});

/**
 * The main code now 
 */
restServer.get('/search/:query', respond);

// don't have to set it when we don't have one
server.set_params("-nologo");

server.initialize();

server.start();

// server.prompt();
// server.poll();

// for (; server.has_new_command() && !server.is_interrupted(); server.prompt(), server.poll())
// 	{
// 	server.process_command();

// 	if (server.is_interrupted())
// 		break;
// 	}

// finalize();




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
    server.search(req.params.query);
    // server.result_to_outchannel();
    var results = {};
    results.results = [];
    var ret = server.next_result();
    while (ret) {
        var hit = server.result_to_json();

        try {
            var obj = JSON.parse(hit);
            results.results.push(obj);
        }
        catch (err) {
            console.error(err);
        }
        ret = server.next_result();
    }
    res.send(results);
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
// RESTIFY SERVER
restServer.get('/search/:query', respond);
restServer.listen(8080, function() {
  console.log('%s listening at %s', restServer.name, restServer.url);
});

// ATIRE SERVER
// don't have to set it when we don't have one
var options = "-nologo";
if (process.argv.length > 2) {
    options += "+-findex+" + process.argv[2];
}

server.set_params(options);

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




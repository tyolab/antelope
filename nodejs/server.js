var restify = require('restify');
var plugins = require('restify-plugins');

var onExit = require('death'); //this is intentionally ugly

var atire = require('./build/Release/atire_api');
// var atire = require('./build/Debug/atire_api');

var PAGE_SIZE_DEFAULT = 20; // hard-coded page size, @todo add ajustable page size later
var PAGE_SIZE_MAX = 50;

// RESTIFY Server
var restServer = restify.createServer({
        name: 'api.tyo.com.au',
        version: '1.0.0'    
    }
);

restServer.use(plugins.acceptParser(restServer.acceptable));
restServer.use(plugins.queryParser());
restServer.use(plugins.bodyParser());

// ATIRE Server
var engine = new atire.ATIRE_API_server();

function searchRespondGet (req, res, next) {
        // the query req.params.query
    var query = req.params.query;

    if (!query || query.length === 0) {
        res.status(204);
        next();
        return;
    }



    res.send(results);
    next();
}

function searchRespondPost (req, res, next) {

}

function search (query, pageNumber, pageSize) {
    pageNumber = pageNumber || 1;
    pageSize = pageSize || PAGE_SIZE_DEFAULT;

    if (pageSize >= PAGE_SIZE_MAX) 
        pageSize = PAGE_SIZE_MAX;

    var hits = engine.search(query);

    if (pageNumber > 1) 
        engine.goto((pageNumber - 1) * pageSize);

    // server.result_to_outchannel();
    var results = {};
    results.list = [];
    var ret = engine.next_result();
    while (ret) {
        var hit = engine.result_to_json();

        try {
            var obj = JSON.parse(hit);
            results.results.push(obj);
        }
        catch (err) {
            console.error(err);
        }
        ret = engine.next_result();
    }
}

function getdoc(req, res, next) {

    next();
}

/**
 * Print out Reach Console version 
 */

function version () {
	console.log(engine.version());
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
    engine.finish();

	process.stdin.destroy();
	process.exit(0);
}

onExit(() => {
    console.log("Exception / Signal caught, existing...");
    finalize();
});

/**
 * The main code now 
 */
// RESTIFY SERVER
restServer.get('/search/:query', searchRespondGet);
restServer.post('/search', searchRespondPost);
restServer.get('/doc/:id', getDocRespondGet);

restServer.listen(8080, function() {
  console.log('%s listening at %s', restServer.name, restServer.url);
});

// ATIRE SERVER
// don't have to set it when we don't have one
var options = "-nologo";
if (process.argv.length > 2) {
    options += "+-findex+" + process.argv[2];
}

engine.set_params(options);

engine.initialize();

engine.start();

// server.prompt();
// server.poll();

// for (; server.has_new_command() && !server.is_interrupted(); server.prompt(), server.poll())
// 	{
// 	server.process_command();

// 	if (server.is_interrupted())
// 		break;
// 	}

// finalize();




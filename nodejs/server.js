var restify = require('restify');
var plugins = require('restify-plugins');

var logger = require('bunyan').createLogger({name: "api.tyo.com.au"});

var onExit = require('death'); //this is intentionally ugly

var atire = require('./build/Release/atire_api');
// var atire = require('./build/Debug/atire_api');

// RESTIFY Server
var server = restify.createServer({
        name: 'api.tyo.com.au',
        version: '1.0.0'    
    }
);

server.use(plugins.acceptParser(server.acceptable));
server.use(plugins.queryParser());
server.use(plugins.bodyParser());

// ATIRE Server
var engine = new atire.ATIRE_API_server();

function searchRespondGet (req, res, next) {
    // the query req.params.query
    searchRespond(req, res, next);
}

function searchRespondPost (req, res, next) {
    // request = {query: , index, size: }
    searchRespond(req, res, next);
}

function searchRespond (req, res, next) {
    var query = req.params.query;
    var index = parseInt(req.params.index);
    var size = parseInt(req.params.size);

    if (size < 1 || size > 50)
        size = 50;

    if (!query || query.length === 0) {
        res.status(204);
        next();
        return;
    }

    var results = search(query, index);

    res.send(results);
    next();
}

function search (query, index, size) {
    var hits = engine.search(query);
    index = index || 0;
    size = size || 20;

    if (index > 0) 
        engine.goto_result(index);

    // server.result_to_outchannel();
    var results = {total:hits};
    results.list = [];
    var ret = engine.next_result();
    var count = 0;
    while (ret && count < size) {
        var hit = engine.result_to_json();

        try {
            var obj = JSON.parse(hit);
            results.list.push(obj);
        }
        catch (err) {
            logger.error(err);
        }
        ret = engine.next_result();
        ++count;
    }

    logger.info({query: query, index: index, page_size: size, hits: hits, size: results.list.length});

    return results;
}

function getdoc(req, res, next) {

    next();
}

/**
 * Print out Reach Console version 
 */

function version () {
	logger.info(engine.version());
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
    logger.info("Exception / Signal caught, existing...");
    finalize();
});

/**
 * The main code now 
 */
// RESTIFY SERVER
server.get('/search/:query/:index/:size', searchRespondGet);
server.post('/search', searchRespondPost);
server.get('/doc/:id', getdoc);

server.listen(8080, function() {
  logger.info('%s listening at %s', server.name, server.url);
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




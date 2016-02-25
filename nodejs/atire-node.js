//var atire = require('./build/Release/atire_api');
var atire = require('./build/Debug/atire_api');
//console.log(atire.version()); // 'world'


//var client = atire.ATIRE_API_remote();

//var indexer = new atire.ATIRE_indexer();

var server = new atire.ATIRE_API_server();

//console.log(server.version());

// don't have to set it when we don't have one
server.set_params("-nologo");

server.initialize();

server.start();

server.loop();

server.finish();



var atire = require('./build/Release/atire_api');

//console.log(atire.version()); // 'world'


//var client = atire.ATIRE_API_remote();

//var indexer = new atire.ATIRE_indexer();

var server = new atire.ATIRE_API_server();

console.log(server.version());


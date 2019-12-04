/**
 * The indexer for creating file(s) index used by Antelope Search Engine
 */

var antelope = require('./index');
var indexer = new antelope.ATIRE_indexer();

var Params = require('node-programmer/params');

var optsAvailable = {
    "processorjs": null
};

var params = new Params(optsAvailable, false);

var opts = params.getOpts();
var optCount = params.getOptCount();

if (optCount <= 0) {
    indexer.usage();
    process.exit(-1);
}

/**
 * The processor for handling the document(s)
 */
var processor = null;
if (opts.processorjs) {
    const Processor = require(opts.processorjs);
    processor = new Processor();
}

/**
 * Need to delete it from the actual parameters that will be passed to antelope
 */
delete opts.processorjs;

var inputs = opts["---"];

antelope.initialize(indexer, opts, processor === null? inputs : null);

if (null == processor) {
    console.info("indexing...");
    indexer.index();
    indexer.finish();
}
else {
    console.info("Using processor to index file");
    processor.process(indexer, inputs);
}

/**
 * The indexer for creating file(s) index used by Antelope Search Engine
 */

var antelope = require('./index');
var indexer = new antelope.ATIRE_indexer();
const utils = require('tyo-utils').utils;

var Params = require('node-programmer/params');

var optsAvailable = {
    "processorjs": null,
    "processor-opt": []
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
    const processorOptsArray = utils.assign([], opts["processor-opt"]);
    var processorOpts = {};
    if (processorOptsArray && processorOptsArray.length) {
        for (var i = 0; i < processorOptsArray.length; i++) {
            var optString = processorOptsArray[i];
            var optsObj = optString.split(":");
            if (optsObj && optsObj.length > 1) {
                processorOpts[optsObj[0]] = utils.assign(processorOpts[optsObj[0]], optsObj[1]);
            }
        }
    }
    processor = new Processor(processorOpts);
}

/**
 * Need to delete it from the actual parameters that will be passed to antelope
 */
delete opts.processorjs;
delete opts["processor-opt"];

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

/**
 * The indexer for creating file(s) index used by Antelope Search Engine
 */

var antelope = require('./build/Release/antelope_api.node');
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

var optionsStr = "";

/**
 * The processor for handling the document(s)
 */
var processor = null;
if (opts.processorjs) {
    const Processor = require(opts.processorjs);
    processor = new Processor();
    delete opts.processorjs;
}

for (var key in opts) {
    if (key[0] !== '-')
        optionsStr += "+-" + key + (opts[key] !== null ? "+" + opts[key] : "");
}

var inputs = opts["---"];
if (inputs) {
    if (typeof(inputs ) === 'string')
        inputs = [inputs];
}
else {
    inputs = [];
}

// if (!processor) {
    var inputsStr = inputs.join("+");

    if (inputsStr.length > 0)
        optionsStr += "+" + inputsStr; 
// }

// console.debug("Options: " + optionsStr);

indexer.init(optionsStr);

if (null == processor)
    indexer.index();
else {
    processor.process(indexer, inputs);
    indexer.finish();
}
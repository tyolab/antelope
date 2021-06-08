var antelope = null;
const fs = require('fs');
const path = require('path');

var node_version = parseInt(process.version.split('.')[0].substr(1));

if (node_version % 2 !== 0)
    throw new Error("This version of NodeJs is not supported");

if (node_version > 14) 
    throw new Error("Only NodeJs version 10 or lower is supported at the moment");

var nodelib = "antelope_api.node";
var parentpath;
var binpath = "." + path.sep + "bin";

// if (process.platform.startsWith("win")) {
//     parentpath = path.join(binpath, 'win');
// } else if (process.platform == "darwin") {
//     parentpath = path.join(binpath, 'darwin');
// } else 
if (process.platform == "linux") {
    parentpath = path.join(binpath, 'linux');
}
else 
    throw new Error("This platform is not supported at the moment: " + process.platform);

var loaded = false;

var nodepath =  "." + path.sep + parentpath + path.sep + nodelib;

if (fs.existsSync(nodepath))
    try {
        antelope = require(nodepath);
        loaded = true;
    } catch (err) {
        console.error(err);
    }

if (!loaded) {
    // Load the new built binary for other platforms.
    var target = __dirname + '/build/' + (process.env.ANTELOPE_BUILD || 'Release') + '/' + nodelib;
    if (fs.existsSync(target))
        antelope = require(target);
    else
        throw new Error("No antelope binary can be found. Please try run \"npm rebuild antelope-search --update-binary\"");
}

if (!antelope)
    throw new Error("Failed to load antelope module");

function createOptionsString(opts, inputs) {
    var optionsStr = "";

    for (var key in opts) {
        if (key[0] !== '-') {
            if (optionsStr.length > 0)
                optionsStr += "+";

            optionsStr += "-" + key + (opts[key] !== null ? "+" + opts[key] : "");
        }
    }

    // Inputs
    if (inputs) {
        var inputsStr = null;

        if (Array.isArray(inputs))
            inputsStr = inputs.join("+");
        else if (typeof inputs === 'string')
            inputsStr = inputs;

        if (inputsStr.length > 0)
            optionsStr += "+" + inputsStr;
    }

    return optionsStr;
}

// antelope.ATIRE_API_server.prototype.name = "engine";
// antelope.ATIRE_indexer.prototype.name = "indexer";

antelope.initialize_engine = function (engine, optionsStr) {
    engine.set_params(optionsStr);
    engine.initialize();
}

antelope.initialize_indexer = function (instance, optionsStr) {
    instance.init(optionsStr);
}

antelope.initialize = function (instance, opts, inputs) {
    var optionsStr;
    if (typeof opts === 'string')
        optionsStr = opts;
    else
        optionsStr = createOptionsString(opts, inputs);

    if (instance.constructor.name === "ATIRE_indexer")
        antelope.initialize_indexer(instance, optionsStr);
    else if (instance.constructor.name === "ATIRE_API_server")
        antelope.initialize_engine(instance, optionsStr);
}

antelope.create_engine = function (opts) {
    var instance = new antelope.ATIRE_API_server();
    antelope.initialize(instance, opts);
    return instance;
}

antelope.create_indexer = function (opts, inputs) {
    var instance = new antelope.ATIRE_API_server();
    antelope.initialize(instance, opts, inputs);
    return instance;
}

module.exports = antelope;

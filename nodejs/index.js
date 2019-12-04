var antelope = null;
const fs = require('fs');

var loaded;
try {
    if(process.platform.startsWith("win")) {
        antelope = require('./bin/win/antelope_api.node');  
    } else if(process.platform == "darwin") {
        antelope = require('./bin/darwin/antelope_api.node');  
    } 
    else if(process.platform == "linux") {
        antelope = require('./bin/linux/antelope_api.node');  
    }
    loaded = true;
}
catch (err) {
    loaded = false;
}

if (!loaded) {
    // Load the new built binary for other platforms.
    var target = './build/' + (process.env.ANTELOPE_BUILD || 'Release')  + '/antelope_api.node';
    if (fs.existsSync(target))
        antelope = require(target); 
    else
        throw new Error("No antelope binary can be found");
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

antelope.initialize_engine = function(engine, optionsStr) {
    engine.set_params(optionsStr);
    engine.initialize();
}   

antelope.initialize_indexer = function(instance, optionsStr) {
    instance.init(optionsStr);
}

antelope.initialize = function(instance, opts, inputs) {
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

antelope.create_engine = function(opts) {
    var instance = new antelope.ATIRE_API_server();
    antelope.initialize(instance, opts);
    return instance;
}

antelope.create_indexer = function(opts, inputs) {
    var instance = new antelope.ATIRE_API_server();
    antelope.initialize(instance, opts, inputs);
    return instance;
}

module.exports = antelope;
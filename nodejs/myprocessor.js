/**
 * @file myprocessor.js
 * 
 * For this processor, we only want to use antelope as a database
 */

'use strict'

const Processor = require("./processor");
const util = require("util");

/**
 * 
 * @param {*} opts 
 */

function MyProcessor (opts) {
    Processor.call(this, opts);

    this.opts.encoding = "binary";
}

util.inherits(MyProcessor, Processor);

/**
 * 
 */

MyProcessor.prototype.index_document = function (indexer, filename, data, content) {
    var name = filename.split(".")[0];
    var contentJson = {
        "title": name,
        "content": ""
    };
    Processor.prototype.index_document(indexer, filename, filename, JSON.stringify(contentJson));
}

 module.exports = MyProcessor;


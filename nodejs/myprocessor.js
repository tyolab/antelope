/**
 * @file myprocessor.js
 * 
 * For this processor, we only want to use antelope as a database,
 * The target files are stored, and encoded with base64
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
 * To convert back to binary data
 * let buff = new Buffer(data, 'base64');
 */

MyProcessor.prototype.index_document = function (indexer, filename, data, content) {
    var name = filename.split(".")[0];
    var contentJson = {
        "title": name,
        "content": data.toString('base64')
    };
    Processor.prototype.index_document(indexer, filename, filename, JSON.stringify(contentJson));
}

 module.exports = MyProcessor;


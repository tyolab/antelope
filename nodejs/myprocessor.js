/**
 * @file myprocessor.js
 * 
 * For this processor, we only want to use antelope as a database,
 * The target files are stored, and encoded with base64
 */

'use strict'

const Processor = require("./processor");
const util = require("util");

const crypto = require('crypto');

const md5sum = crypto.createHash('md5');

/**
 * 
 * @param {*} opts 
 */

function MyProcessor (opts) {
    Processor.call(this, opts);
    // if it is binary file, set encoding to null, not "binary"
    // readFileSync / readFile won't recognize this encoding
    this.opts.encoding = null; // "binary";
}

util.inherits(MyProcessor, Processor);

/**
 * To convert back to binary data
 * let buff = new Buffer(data, 'base64');
 * 
 * @param indexer
 * @param filename
 * @param data The binary data of the index file
 * @param content The extra content can be appended to the original doc
 */

MyProcessor.prototype.index_document = function (indexer, filename, data, content) {
    var name = filename.split(".")[0];
    var contentBase64 = data.toString('base64');
    var contentJson = {
        "file": filename,
        "title": name,
        "content": contentBase64,
        size: data.length,
        "hash-md5": crypto.createHash('md5').update(data).digest('hex')
    };
    Processor.prototype.index_document(indexer, filename, filename, JSON.stringify(contentJson));
}

 module.exports = MyProcessor;


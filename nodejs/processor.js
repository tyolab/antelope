/**
 * @file processor.js
 * 
 * An example for how to create a custom processor
 */

const async = require("async");

const fs = require('fs');

const path = require('path');

/**
 * Processor class
 * 
 * @param {*} opts 
 */

function Processor(opts) {
    this.opts = opts || 

    // The default should be
    {
        "save-content": false,
        "encoding": "utf8",
        "includes": null, // []
        "excludes": []
    };

    if (this.opts["save-content"] === undefined)
        this.opts["save-content"] = false;
    
    this.opts.encoding = this.opts.encoding || "utf8";

    if (this.opts.includes) {
        this.includesRegExp = new RegExp(this.opts.includes, "i");
    }
}

/**
 * 
 */

Processor.prototype.process_folder = function (indexer, folder, callback) {
    var self = this;

    fs.readdir(folder, (err, files) => {
        if (err)
            return;

        async.eachSeries(files, (file, done) => {
            self.process_file(indexer, folder + "/" + file, done);
        },
        (err) => {
            if (err)
                console.error(err);
            callback();
        });
    });

}

/**
 * 
 */

Processor.prototype.process_file = function (indexer, file, done) {

    var self = this;

    if (fs.lstatSync(file).isDirectory()) {
        self.process_folder(indexer, file, done);
        return;
    }

    if (this.includesRegExp && file.match(this.includesRegExp)) {

        var filename = path.basename(file);
        var data = fs.readFileSync(file, this.opts.encoding);

        var save_content = this.opts ? this.opts["save-content"] : false;
        this.index_document(indexer, filename, data, save_content  ? data : null);
    }

    if (done)
        done();
}

/**
 * 
 */

Processor.prototype.index_document = function (indexer, name, data, content) {
    var xml_text = `
        <DOC>
            <TITLE>${name}</TITLE>
            <TEXT>
            ${data}
            </TEXT>
        </DOC>
    `;
    if (content)
        indexer.index_document(name, xml_text, content);
    else
        indexer.index_document(name, xml_text);
}

/**
 * Override me to add the logic
 * By the default, we will use the file name as title
 * the whole content the file as text
 */

Processor.prototype.process = function (indexer, inputs) {
    if (!Array.isArray(inputs))
        inputs = [inputs];
        
    var self = this;
    async.eachSeries(inputs, (inputFile, done) => {
        if (fs.lstatSync(inputFile).isDirectory())
            self.process_folder(indexer, inputFile, done);
        else {
            self.process_file(indexer, inputFile, done);
        }
    },
    (err) => {
        indexer.finish();
    }
    );
}

module.exports = Processor;
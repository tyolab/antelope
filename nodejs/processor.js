/**
 * @file processor.js
 * 
 * An example for how to create a custom processor
 */

const async = require("async");

const fs = require('fs');

const path = require('path');

function Processor(opts) {
    this.opts = opts || {
        "save-content": false
    };
}

Processor.prototype.process_folder = function (indexer, folder, callback) {
    var self = this;
    fs.readdir(folder, (err, files) => {
        if (err)
            return;

        async.eachSeries(files, (file, done) => {
            self.process_file(indexer, folder + "/" + file);
            done();
        },
        (err) => {
            if (err)
                console.error(err);
            callback();
        });
    });

}

Processor.prototype.process_file = function (indexer, file) {
    var name = path.basename(file).split(".")[0];
    var data = fs.readFileSync(file);
    var xml_text = `
        <DOC>
            <TITLE>${name}</TITLE>
            <TEXT>
            ${data}
            </TEXT>
        </DOC>
    `;
    var save_content = this.opts ? this.opts["save-content"] : false;
    if (save_content)
        indexer.index_document(name, xml_text, data);
    else
        indexer.index_document(name, xml_text);
}

/**
 * Override me to add the logic
 * By the default, we will use the file name as title
 * the whole content the file as text
 */

Processor.prototype.process = function (indexer, inputs) {
    var self = this;
    async.eachSeries(inputs, (inputFile, done) => {
        if (fs.lstatSync(inputFile).isDirectory())
            self.process_folder(indexer, inputFile, done);
        else {
            self.process_file(indexer, inputFile);
            done();
        }
    },
    (err) => {
        // indexer.finish();
        console.debug("done");
    }
    );
}

module.exports = Processor;
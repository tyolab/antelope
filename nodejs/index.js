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

module.exports = antelope;
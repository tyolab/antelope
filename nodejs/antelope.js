var antelope = require('.');

// if you want to use a debug copy of the library
// var antelope = require('./build/Debug/antelope_api');

// don't have to set it when we don't have one
var engine = antelope.create_engine("-nologo");
engine.start();

/**
 * Print out Reach Console version 
 */

function version () {
	console.log(engine.version());
}

/**
 * Check if the input is an empty string 
 */

function isEmpty(str) {
    return (!str || /^\s*$/.test(str));
}

/**
 * Do what need to be done on exit 
 */

function onExit() {
	
	process.stdin.destroy();
	process.exit(0);
}

engine.prompt();
engine.poll();

for (; engine.has_new_command() && !engine.is_interrupted(); engine.prompt(), engine.poll())
	{
	engine.process_command();

	if (engine.is_interrupted())
		break;
	}

engine.finish();



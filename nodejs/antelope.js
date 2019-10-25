var antelope = require('./build/Release/antelope_api.node');



// var antelope = require('./build/Debug/antelope_api');

var server = new antelope.ATIRE_API_server();

// don't have to set it when we don't have one
server.set_params("-nologo");

server.initialize();

server.start();


/**
 * Print out Reach Console version 
 */

function version () {
	console.log(server.version());
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

/**
 * The main code now 
 */

//var prompt = 'Atire>';

//version();

//var rl = readline.createInterface({
//  input: process.stdin,
//  output: process.stdout,
//  terminal: false
//});
//
//rl.setPrompt(prompt, prompt.length);
//rl.prompt();
//
//rl.on('line', function (line) {
////    console.log(line);
//	var tokens = line.split(" ");
//	
//	if (tokens.length > 0) {
//		var cmd = tokens[0];
//		if (!isEmpty(cmd)) {
//		    switch (cmd) {
//		    	case ".quit":
//		    		onExit();
//			    	break;
//		    	case ".help":
//		    		help();
//		    		break;
//		    	default:
//		    		console.log("Unknown command: " + line);
//		    		console.log("Enter .help for usage information.");
//		    		break;
//		    }
//	    }
//    }
//    
//    
//    rl.setPrompt(prompt, prompt.length);
//    rl.prompt();
//});

server.prompt();
server.poll();

for (; server.has_new_command() && !server.is_interrupted(); server.prompt(), server.poll())
	{
	server.process_command();

	if (server.is_interrupted())
		break;
	}

server.finish();



{
  "targets": [
    {
      "target_name": "atire_api",
      "sources": [ "antelope_wrap.cxx" ],
      "cflags!": [ '-DSWIG_V8_VERSION=0x045103' ],
      "cflags_cc!": [ '-DSWIG_V8_VERSION=0x045103' ],
      "include_dirs": ["../atire", "../source"],
	  "libraries": ["-L../build/debug", "-lantelope_api", 
            "-lantelope_core", 
            "-lantelope_index_param", 
            "-lantelope_ant_param"]
    }
  ]
}
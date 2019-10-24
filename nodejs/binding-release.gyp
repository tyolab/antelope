{
  "targets": [
    {
      "target_name": "antelop_api",
      "sources": [ "antelope_wrap.cxx" ],
      "cflags!": [ '-DSWIG_V8_VERSION=0x045103' ],
      "cflags_cc!": [ '-DSWIG_V8_VERSION=0x045103' ],
      "include_dirs": ["../atire", "../source"],
	  "libraries": ["-lantelope_api", "-lantelope_core", "-lantelope_index_param", "-lantelope_ant_param"]
    }
  ]
}
{
  "targets": [
    {
      "target_name": "antelope_api",
      "sources": [ "antelope_wrap.cxx" ],
      "cflags!": [ '-DSWIG_V8_VERSION=0x062414' ],
      "cflags_cc!": [ '-DSWIG_V8_VERSION=0x062414' ],
      "include_dirs": ["../atire", "../source"],
	  "libraries": ["-lantelope_api", "-lantelope_core", "-lantelope_index_param", "-lantelope_ant_param"]
    }
  ]
}
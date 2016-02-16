{
  "targets": [
    {
      "target_name": "atire_api",
      "sources": [ "atire_wrap.cxx" ],
      "cflags!": [ '-DSWIG_V8_VERSION=0x040685' ],
      "cflags_cc!": [ '-DSWIG_V8_VERSION=0x040685' ],
      "include_dirs": ["../atire", "../source"],
	  "libraries": ["-latire_api", "-latire_core", "-latire_index_param", "-latire_ant_param"]
    }
  ]
}
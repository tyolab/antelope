{
  "targets": [
    {
      "target_name": "atire_api",
      "sources": [ "atire_node.cpp", "atire_wrap.cxx" ],
      "include_dirs": ["../atire", "../source"],
	  "libraries": ["-latire_api", "-latire_core", "-latire_index_param", "-latire_ant_param"]
	  "cflags": ["-Wall", "-std=c++11"],
      'xcode_settings': {
        'OTHER_CFLAGS': [
          '-std=c++11'
        ],
      },
    }
  ]
}
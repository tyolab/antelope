{
  "targets": [
    {
      "target_name": "atire",
      "sources": [ "atire_node.cpp", "atire_wrap.cxx" ],
      "include_dirs": ["../atire", "../source"],
	  "libraries": ["-latire_api", "-latire_core", "-latire_index_param", "-latire_ant_param"]
    }
  ]
}
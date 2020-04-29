{
  "targets": [
    {
      "target_name": "antelope_api",
      "sources": [ "antelope_wrap.cxx" ],
      "cflags!": [ '-DSWIG_V8_VERSION=0x062414' ],
      "cflags_cc!": [ '-DSWIG_V8_VERSION=0x062414' ],
      'ldflags!': ["-static"],
      "include_dirs": ["../atire", "../source"],
	  "libraries": ["-lantelope_ant_param", "-lantelope_index_param", "-lantelope_api", "-lantelope_core"]
    }
  ],
  "conditions": [
    ["OS=='mac'", {
      "xcode_settings": {
        "GCC_ENABLE_CPP_RTTI": "YES",
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "OTHER_CFLAGS": [
          "-std=c++11",
          "-stdlib=libc++"
        ]
      },
      "libraries": [
      ]
    }],
    ["OS=='win'", {
    "msvs_settings": {
      "VCCLCompilerTool": {
        'StringPooling': 'true',
         #pool string literals 'DebugInformationFormat': 3,
         #Generate a PDB 'WarningLevel': 3,
        'BufferSecurityCheck': 'true',
        'ExceptionHandling': 1,
        'SuppressStartupBanner': 'true',
        'WarnAsError': 'false',
      }
    },
    "libraries": [
    ]
    }],
    ["OS=='linux'", {
      "node_shared": "false",
      "cflags!": [ ],
      "cflags_cc!": [ ],
      'ldflags!': ["-static"],
      "libraries": []
    }],
  ]
}
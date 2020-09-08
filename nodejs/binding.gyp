{
  "targets": [
    {
      "target_name": "antelope_api",
      "sources": [ "antelope_wrap.cxx" ],
      "cflags!": [ '-DSWIG_V8_VERSION=0x062414' ],
      "cflags_cc!": [ '-DSWIG_V8_VERSION=0x062414' ],
      "include_dirs": ["./include"],
	  "libraries": ["-lantelope_api", "-lantelope_core", "-lantelope_index_param", "-lantelope_ant_param"]
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
      "include_dirs": [
      ],
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
    "include_dirs": [
    ],
    "libraries": [
    ]
    }]
    ]
}
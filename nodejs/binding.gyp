{
  "targets": [
    {
      "target_name": "antelope_api",
      "sources": [ "antelope_wrap.cxx" ],
      "defines": [
        "NAPI_VERSION=8",
        "BUILDING_NODE_EXTENSION", "ATIRE_LIBRARY", "ONE_PARSER",
        "_CRT_SECURE_NO_WARNINGS", "ATIRE_MOBILE", "ATIRE_JNI",
        "ANT_HAS_ZLIB", "HASHER=1", "HEADER_NUM=1",
        "SPECIAL_COMPRESSION=1", "TWO_D_ACCUMULATORS", "TOP_K_READ_AND_DECOMPRESSOR",
        "NOMINMAX", "FILENAME_INDEX"
      ],
      "include_dirs": [
        "./include",
        "../source",
        "../atire",
        "<!@(node -p \"require('node-addon-api').include_dir\")"
      ],
      "library_dirs": ["./bin/linux"],
      "libraries": ["-lantelope_api", "-lantelope_core", "-lantelope_index_param", "-lantelope_ant_param"],
      "cflags_cc": ["-std=c++17", "-fexceptions", "-frtti"],
      "cflags_cc!": ["-fno-exceptions", "-fno-rtti"],
      "ldflags": ["-Wl,-rpath,<(module_root_dir)/bin/linux"]
    },
    {
      "target_name": "antelope_segment",
      "sources": [ "addon/segment_index.cpp" ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [ "NAPI_VERSION=8", "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "cflags_cc": [ "-std=c++17", "-fno-exceptions" ],
      "libraries": [
        "<(module_root_dir)/../lib/libantelope_engine.a",
        "<(module_root_dir)/../external/unencumbered/zlib/libz.a",
        "<(module_root_dir)/../external/unencumbered/bzip/libbz2.a",
        "<(module_root_dir)/../external/gpl/lzo/liblzo2.a",
        "<(module_root_dir)/../external/unencumbered/snappy/libsnappy.a",
        "<(module_root_dir)/../external/unencumbered/snowball/libstemmer.a",
        "-lpthread", "-ldl"
      ]
    }
  ],
  "conditions": [
    ["OS=='mac'", {
      "xcode_settings": {
        "GCC_ENABLE_CPP_RTTI": "YES",
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "OTHER_CFLAGS": ["-std=c++11", "-stdlib=libc++"]
      }
    }]
  ]
}

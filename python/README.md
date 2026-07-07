# antelope (Python binding)

Python binding for the [Antelope/ATIRE](https://github.com/tyolab/antelope) search
engine, plus an MCP server (added in a later layer).

The extension (`antelope._core`) is a pybind11 module that links directly against
the engine static archive (`lib/libantelope_engine.a`) and the bundled external
archives (zlib, bzip2, lzo, snappy, snowball).

## Requirements

- Python 3.10+
- A C++ toolchain (the engine and its externals are compiled from source)
- GNU make (the build driver invokes `make all` + `make engine_lib` in the repo
  root automatically — you do not need to run it yourself)

## Install (build from source)

There is no PyPI release. Build the extension from a checkout of the repo:

```sh
pip install ./python
# or, for development:
pip install -e ./python
```

The first build compiles the whole engine and its external archives, so expect it
to take several minutes.

## Test

```sh
python -m pytest python/tests -v
```

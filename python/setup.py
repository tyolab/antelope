import os, subprocess
from setuptools import setup
from pybind11.setup_helpers import Pybind11Extension, build_ext

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ARCHIVES = [os.path.join(REPO, p) for p in [
    "lib/libantelope_engine.a",
    "external/unencumbered/zlib/libz.a",
    "external/unencumbered/bzip/libbz2.a",
    "external/gpl/lzo/liblzo2.a",
    "external/unencumbered/snappy/libsnappy.a",
    "external/unencumbered/snowball/libstemmer.a",
]]

class make_then_build(build_ext):
    def run(self):
        # externals + engine objects, then archive; externals are gitignored artifacts absent in a fresh checkout
        subprocess.check_call(["make", "all"], cwd=REPO)
        subprocess.check_call(["make", "engine_lib"], cwd=REPO)
        missing = [a for a in ARCHIVES if not os.path.exists(a)]
        if missing:
            raise SystemExit("missing engine archives after make: %s" % missing)
        super().run()

ext = Pybind11Extension(
    "antelope._core",
    ["src/antelope_core.cpp"],
    include_dirs=[os.path.join(REPO, "source"), os.path.join(REPO, "atire")],
    extra_objects=ARCHIVES,
    libraries=["pthread", "dl"],
    cxx_std=14,
)
setup(cmdclass={"build_ext": make_then_build}, ext_modules=[ext])

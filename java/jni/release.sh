#!/bin/bash
rm ../libs
mkdir -p build/release
ln -sf `pwd`/build/release `pwd`/../libs
ndk-build

#!/bin/bash
rm ../libs
mkdir -p build/debug
ln -sf `pwd`/build/debug `pwd`/../libs
ndk-build

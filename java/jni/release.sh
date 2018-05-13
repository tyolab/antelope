#!/bin/bash
mkdir -p build/release
ln -sf `pwd`/build/release `pwd`/../libs
ndk-build

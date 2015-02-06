#!/bin/bash
mkdir -p ../libs.release
ln -sf ../libs.release ../libs
ndk-build

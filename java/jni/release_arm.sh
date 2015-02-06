#!/bin/bash
mkdir -p ../libs.arm
ln -sf ../libs.arm ../libs
ndk-build

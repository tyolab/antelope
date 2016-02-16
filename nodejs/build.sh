#!/bin/bash

# by default with build.sh we create release build

. ./env.sh

ln -sf ./binding-release.gyp ./binding.gyp

./configure.sh

node-gyp build "$@"

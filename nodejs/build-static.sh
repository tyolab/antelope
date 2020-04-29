#!/bin/bash

# by default with build.sh we create release build

. ./env.sh

ln -sf ./binding-static.gyp ./binding.gyp

./configure.sh

node-gyp build "$@"

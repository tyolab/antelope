#!/bin/bash

. ./env.sh

##  for example, --debug ##
ln -sf ./binding-debug.gyp ./binding.gyp

./configure.sh --debug "$@"

node-gyp build --debug "$@"


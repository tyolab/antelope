#!/bin/bash

# by default with build.sh we create release build

. ./env.sh

cp -f ./binding-release.gyp ./binding.gyp

./configure.sh

node-gyp build "$@"

NODE_VERSION=`node -e "console.log(process.version)" | cut -f 1 -d "." | (read v; echo ${v:1:10})`

mv bin/linux/antelope_api.node bin/linux/antelope_api.node"$NODE_VERSION"

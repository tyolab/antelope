#!/bin/bash
dest=`pwd`/../libs

if ! [ -e "$dest" ]
then
    mkdir -p build/debug
    ln -sf `pwd`/build/debug $dest
fi

ndk-build

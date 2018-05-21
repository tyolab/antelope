#!/bin/bash
#rm ../libs

dest=`pwd`/../libs

if [ -e "$dest" ]
then
    mkdir -p build/release
    ln -sf `pwd`/build/release $dest
fi

ndk-build

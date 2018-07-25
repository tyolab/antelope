#!/bin/bash


#####
echo "This script file depends on the acc-{arm, arm64, x86, x86-64}.sh"
echo "from: https://github.com/atomba/svn/"
echo "Also you need to 'export ANDROID_TOOLCHAINS=/path/to/toolchains' in your shell profile"

home=`dirname $0`/..

platform=`uname`
rl=readlink
if [ "$platform" == "Darwin" ] 
then
    rl=greadlink
fi

home=`$rl -f $home`
#echo $home
targets=$home/build/targets
libs=$home/build/libs

mkdir -pv $libs

for dir in arm arm64 x86 x86-64
do
    . `which acc-$dir.sh` 
    if ! [ -e $CC ] 
    then
        echo "Cannot find compiler $CC"
        exit -1
    fi 

    build_dir=$targets/$dir
    mkdir -p $build_dir
    host=
    case $dir in
        arm)
            host=arm-linux-androideabi
            ;;
        arm64)
            host=aarch64-linux-android
            ;;
        x86-64)
            host=x86_64-linux-android
            ;;
        x86)
            host=i686-linux-android
            ;;            
        *)
            ;;
    esac

    cd $targets/$dir
    $home/configure --prefix=$libs --host=$host
done

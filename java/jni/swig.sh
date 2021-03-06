#!/bin/bash


swig -java -c++ -package au.com.tyo.antelope.jni ../antelope.swig

java_path='../android-native/antelope_droid/src/main/java'

if ! [ -z "$1" ]
then
	java_path=$1
fi

java_src=${java_path}/au/com/tyo/antelope/jni

if ! [ -e "$java_src" ]
then
	mkdir -p $java_src
fi

\rm -rf $java_src/*
mv -v ../*.java $java_src/
mv -v ../*.cxx ./

for i in `ls *.cxx`
do
	name=`echo $i | cut -f 1 -d "."`
	ln -sf $i $name.cpp
done

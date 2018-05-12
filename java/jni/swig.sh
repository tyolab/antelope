#!/bin/bash


swig -java -c++ -package au.com.tyo.antelope.jni antelope.swig

java_src=src/au/com/tyo/antelope/jni

if ! [ -e "$java_src" ]
then
	mkdir -p $java_src
fi

\rm -f src/au/com/tyo/antelope/jni/*
mv *.java $java_src/

for i in `ls *.cxx`
do
	name=`echo $i | cut -f 1 -d "."`
	ln -sf $i $name.c
done

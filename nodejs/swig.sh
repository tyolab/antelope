#!/bin/bash

# -o antelope_wrap.cpp
swig -c++ -javascript -node antelope.swig 
#swig -c++ -javascript -node  -DV8_VERSION=0x045103 antelope.swig 
# perl -pi -e 's/#define SWIG_V8_VERSION 0x031110/#define SWIG_V8_VERSION 0x045103/g' antelope_wrap.cxx

#!/bin/bash

# -o atire_wrap.cpp
swig -c++ -javascript -node  -DV8_VERSION=0x040685 atire.swig 

# perl -pi -e 's/#define SWIG_V8_VERSION 0x031110/#define SWIG_V8_VERSION 0x040685/g' atire_wrap.cxx

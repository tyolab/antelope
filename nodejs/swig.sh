#!/bin/bash

swig -c++ -javascript -node atire.swig 

perl -pi -e 's/#define SWIG_V8_VERSION 0x031110/#define SWIG_V8_VERSION 0x040685/g' atire_wrap.cxx

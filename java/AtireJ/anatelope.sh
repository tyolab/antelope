#!/bin/bash

base=`dirname $0`

# because by default we install the jni library in /usr/local/lib
java  -Djava.library.path="/usr/local/lib"  -cp $base/bin:$base/lib/AtireJNI.jar org.atire.Atire "$@" 

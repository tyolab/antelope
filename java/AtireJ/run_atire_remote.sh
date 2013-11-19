#!/bin/bash

base=`dirname $0`
# -verbose:jni 
java -Djava.library.path="/usr/local/lib" -cp $base/bin:$base/lib/AtireJNI.jar::$base/lib/CommonUtils.jar org.atire.AtireRemoteClient "$@" 
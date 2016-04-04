/*
	ATIRE.SWIG
	---------------------
	This is the SWIG interface file for the all ATIRE APIs.
	see swig.org for information on SWIG
*/

%module atire_api

/*
%define %swig_v8_define_version(version)
%begin %{
#ifndef V8_VERSION
#define V8_VERSION version
#endif
%}
%enddef
*/

#ifdef SWIGJAVASCRIPT
#ifdef SWIG_V8_VERSION
%swig_v8_define_version(SWIG_V8_VERSION)
#endif
#endif

%{
#include "../atire/atire_api_remote.h"
#include "../atire/atire_api_server.h"
#include "../atire/indexer.h"
//#include "../java/jni/include/index.h"
//#include "../java/jni/include/atire.h"
%}

%include "../atire/atire_api_remote.h"
%include "../atire/atire_api_server.h"
%include "../atire/indexer.h"

/*
%include "../java/jni/include/index.h"
%include "../java/jni/include/atire.h"
*/


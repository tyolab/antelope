/*
	ATIRE.SWIG
	---------------------
	This is the SWIG interface file for the all ATIRE APIs.
	see swig.org for information on SWIG
*/

%module atire_api
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


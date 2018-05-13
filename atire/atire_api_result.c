/*
	ATIRE_API_RESULT.C
	------------------
*/

#include "atire_api_result.h"

#ifndef NULL
	#define NULL 0
#endif

ATIRE_API_result::ATIRE_API_result() {
    name = NULL;
    title = NULL;
    snippet = NULL;

    doc_id = -1;
    rank = -1;
    rsv = 0.0f;
}

ATIRE_API_result::~ATIRE_API_result() {
    if (name)
        delete [] name;
    if (title)
        delete [] title;
    if (snippet)
        delete [] snippet;                
}
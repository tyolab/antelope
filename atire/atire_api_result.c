/*
	ATIRE_API_RESULT.C
	------------------
*/

#include "atire_api_result.h"

#ifndef NULL
	#define NULL 0
#endif

const char *ATIRE_API_result::EMPTY_STRING  = "";

ATIRE_API_result::ATIRE_API_result() {
#ifdef FILENAME_INDEX
	document_name = new char [MAX_TITLE_LENGTH];
#else
	answer_list = NULL;
#endif

    title = NULL;
    snippet = NULL;

    docid = -1;
    rank = -1;
    rsv = 0.0f;
}

ATIRE_API_result::~ATIRE_API_result() {
    if (document_name)
        delete [] document_name;
    if (title)
        delete [] title;
    if (snippet)
        delete [] snippet;                
}
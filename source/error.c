/*
	ERROR.C
	---------
*/
#include <stdio.h>
#include <stdlib.h>

#include "error.h"

long ANT_error_code = ANT_ERROR_NONE;

/*
	ANT_ERROR()
	-------------
*/
void ANT_error(const char *error_info, long error_code)
{
ANT_error_code = error_code;

#ifdef ATIRE_API
    // do nothing
#else
    exit(printf(error_info));
#endif
}
/*
	ERROR.H
	---------
*/
#ifndef ERROR_H_
#define ERROR_H_

#include "fundamental_types.h"

enum {
        ANT_ERROR_UNKNOWN = -1,
        ANT_ERROR_NONE = 0, 
        ANT_ERROR_SIGNATURE_MISMATCH = 1,
        ANT_ERROR_VERSION_MISMATH = 2, 
        ANT_ERROR_FILE_TYPE_MISMATCH = 3,
        ANT_ERROR_INDEX_READING = 4,
        ANT_ERROR_BAD_INDEX = 5
        };

extern long ANT_error_code;

extern void ANT_on_error(const char *error_info, long error_code = ANT_ERROR_UNKNOWN);

#endif  /* ERROR_H_ */

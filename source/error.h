/*
	ERROR.H
	---------
*/
#ifndef ERROR_H_
#define ERROR_H_

#include "fundamental_types.h"

enum {ANT_ERROR_NONE, 
        ANT_ERROR_SIGNATURE_MISMATCH,
        ANT_ERROR_VERSION_MISMATH, 
        ANT_ERROR_FILE_TYPE_MISMATCH,
        ANT_ERROR_INDEX_READING,
        ANT_ERROR_UNKNOWN
        };

extern long ANT_error_code;

void on_error(const char *error_info, long error_code = ANT_ERROR_UNKNOWN);

#endif  /* ERROR_H_ */

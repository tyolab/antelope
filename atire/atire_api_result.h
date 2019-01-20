/*
	ATIRE_API_RESULT.H
	------------------
*/
#ifndef ATIRE_API_RESULT_H_
#define ATIRE_API_RESULT_H_

#ifndef NULL
#define NULL 0x0
#endif

class ATIRE_API_result 
{
public:
    static const char *EMPTY_STRING;

    static const long MAX_TITLE_LENGTH = 1024;

public:
    int rank;
    long docid;

    float rsv;

    char *title;
    char *snippet;

	#ifdef FILENAME_INDEX
		char *document_name;
	#else
		char **answer_list;
	#endif

    ATIRE_API_result();
    ~ATIRE_API_result();
};

#endif /* ATIRE_API_RESULT_H_ */
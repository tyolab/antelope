/*
	ATIRE_API_RESULT.H
	------------------
*/

class ATIRE_API_result {
public:
    int rank;
    long doc_id;
    char *name;
    float rsv;
    char *title;
    char *snippet;

    ATIRE_API_result();
    ~ATIRE_API_result();
};

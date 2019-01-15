/*
	SEARCH_ENGINE_RESULT_ITERATOR.H
	-------------------------------
*/
#ifndef SEARCH_ENGINE_RESULT_DOC_ITERATOR_H_
#define SEARCH_ENGINE_RESULT_DOC_ITERATOR_H_

#include "search_engine_result_iterator_base.h"

class ANT_search_engine;
class ANT_search_engine_result;
/*
	class ANT_SEARCH_ENGINE_RESULT_DOC_ITERATOR
	--------------------------------------------
*/
class ANT_search_engine_result_doc_iterator: public ANT_search_engine_result_iterator_base
{
protected:
	ANT_search_engine *search_engine;
	ANT_search_engine_result *result;
	long long results_list_length;
	long long current;

public:
	ANT_search_engine_result_doc_iterator(ANT_search_engine *engine);
	virtual ~ANT_search_engine_result_doc_iterator() {}

	virtual long long first(long long start = 0);	// start is the position in the results list from which to start (counting from 0)
	virtual long long next(void);
} ;

#endif /* SEARCH_ENGINE_RESULT_DOC_ITERATOR_H_ */

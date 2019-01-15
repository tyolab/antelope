/*
	SEARCH_ENGINE_RESULT_ID_ITERATOR.H
	----------------------------------
*/
#ifndef SEARCH_ENGINE_RESULT_ID_ITERATOR_H_
#define SEARCH_ENGINE_RESULT_ID_ITERATOR_H_

#include "search_engine_result_iterator_base.h"

class ANT_search_engine;
class ANT_search_engine_result;

/*
	class ANT_SEARCH_ENGINE_RESULT_ID_ITERATOR
	------------------------------------------
*/
class ANT_search_engine_result_id_iterator: public ANT_search_engine_result_iterator_base
{
private:
	ANT_search_engine_result *result;
	long long results_list_length;
	long long current;

public:
	ANT_search_engine_result_id_iterator(ANT_search_engine_result *results_list);
	virtual ~ANT_search_engine_result_id_iterator() {}

	virtual long long first(long long start = 0);	// start is the position in the results list from which to start (counting from 0)
	virtual long long next(void);
} ;

#endif /* SEARCH_ENGINE_RESULT_ID_ITERATOR_H_ */

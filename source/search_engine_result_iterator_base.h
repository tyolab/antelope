/*
	SEARCH_ENGINE_RESULT_ITERATOR.H
	-------------------------------
*/
#ifndef SEARCH_ENGINE_RESULT_ITERATOR_BASE_H_
#define SEARCH_ENGINE_RESULT_ITERATOR_BASE_H_

class ANT_search_engine;
class ANT_search_engine_result;
/*
	class ANT_SEARCH_ENGINE_RESULT_ITERATOR_BASE
	--------------------------------------------
*/
class ANT_search_engine_result_iterator_base
{
public:
	ANT_search_engine_result_iterator_base() {}
	virtual ~ANT_search_engine_result_iterator_base() {}

	virtual long long first(long long start = 0) = 0;	// start is the position in the results list from which to start (counting from 0)
	virtual long long next(void) = 0;
} ;

#endif /* SEARCH_ENGINE_RESULT_ITERATOR_BASE_H_ */

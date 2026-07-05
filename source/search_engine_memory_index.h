/*
	SEARCH_ENGINE_MEMORY_INDEX.H
	----------------------------
*/
#ifndef SEARCH_ENGINE_MEMORY_INDEX_H_
#define SEARCH_ENGINE_MEMORY_INDEX_H_

#include "search_engine.h"
#include "file.h"

class ANT_memory_index;
class ANT_memory;

/*
	class ANT_SEARCH_ENGINE_MEMORY_INDEX
	------------------------------------
*/
class ANT_search_engine_memory_index : public ANT_search_engine, public ANT_file
{
protected:
	ANT_memory_index *index;
	ANT_memory *memory;
	char *postings_buffer, *postings_buffer_location;
	size_t postings_buffer_length;
	long owns_index;			// if false, the destructor leaves the shared ANT_memory_index alone

public:
	ANT_search_engine_memory_index(ANT_memory_index *index, ANT_memory *memory);
	virtual ~ANT_search_engine_memory_index();

	virtual unsigned char *get_postings(ANT_search_engine_btree_leaf *term_details, unsigned char *destination);
	virtual ANT_search_engine_btree_leaf *get_postings_details(char *term, ANT_search_engine_btree_leaf *term_details);
	virtual int open(const char *filename = "index.aspt", unsigned long header_offset = 0);
	virtual long open(const char *filename, char *mode) { return open(filename); }
	virtual long write(unsigned char *data, long long size);
	void set_index_ownership(long owns) { owns_index = owns; }
} ;

#endif /* SEARCH_ENGINE_MEMORY_INDEX_H_ */


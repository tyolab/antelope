/*
	readability_tag_finder.h
	------------------------
 */

#ifndef READABILITY_TAG_FINDER_H_
#define READABILITY_TAG_FINDER_H_

#include "readability_factory.h"
#include "btree_iterator.h"

class ANT_memory_indexer;

class ANT_string_pair;

class ANT_readability_tag_finder : public ANT_readability_factory
{
private:
	static char 	**special_tags;

private:
	const char 	*matching_tag;
	int				where;
	char				info_buf[MAX_TERM_LENGTH];

public:
	ANT_readability_tag_finder();
	virtual ~ANT_readability_tag_finder();

	void handle_tag(ANT_string_pair *token);

	void index(ANT_memory_indexer *index);
};

#endif /* READABILITY_TAG_FINDER_H_ */

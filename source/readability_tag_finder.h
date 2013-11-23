/*
	readability_tag_finder.h
	------------------------
 */

#ifndef READABILITY_TAG_FINDER_H_
#define READABILITY_TAG_FINDER_H_

#include "readability.h"

class ANT_memory_indexer;

class ANT_string_pair;

class ANT_readability_tag_finder : public ANT_readability
{
private:
	static char **special_tags;

public:
	ANT_readability_tag_finder();
	virtual ~ANT_readability_tag_finder();

	unsigned long is_the_tag_looking_for(ANT_string_pair *string);
	void handle_tag(ANT_memory_indexer *indexer, ANT_string_pair *string, long long docno);
};

#endif /* READABILITY_TAG_FINDER_H_ */

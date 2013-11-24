/*
	READABILITY_TAG_FINDER.H
	------------------------
 */

#ifndef READABILITY_TAG_FINDER_H_
#define READABILITY_TAG_FINDER_H_

#include "readability.h"
#include "btree_iterator.h"

#define MAX_TERM_COUNT 1000

class ANT_memory_indexer;

class ANT_string_pair;

class ANT_readability_tag_finder : public ANT_readability
{
private:
	static char 	**special_tags;

private:
	const char 	*matching_tag;
	int				number_of_tags;
	char				**terms;
	int				where;
	char				info_buf[MAX_TERM_LENGTH];
	long				tag_processing_on;
	int				term_count;
	char				prefix_char;

private:
	void clean_up();

public:
	ANT_readability_tag_finder();
	virtual ~ANT_readability_tag_finder();

	void handle_tag(ANT_string_pair *token, long tag_open);
	void handle_token(ANT_string_pair *token);

	void index(ANT_memory_indexer *index, long long doc);
};

#endif /* READABILITY_TAG_FINDER_H_ */

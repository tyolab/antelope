/*
	READABILITY_TAG_WEIGHTING.H
	---------------------------
 */

#ifndef READABILITY_TAG_WEIGHTING_H_
#define READABILITY_TAG_WEIGHTING_H_

#include "readability.h"
#include "btree_iterator.h"

#include <string>

#define MAX_TERM_COUNT 1000

class ANT_memory_indexer;

class ANT_parser_token;

class ANT_directory_iterator_object;

class ANT_readability_TAG_WEIGHTING : public ANT_readability
{
public:
	static char *special_tags_extra;
	static char buffer[MAX_TERM_LENGTH];

private:
	static char *special_tags_general[];

private:
	const char *matching_tag;
	int number_of_tags;
	char **terms;
	int where;
	char info_buf[MAX_TERM_LENGTH];
	long tag_processing_on;
	int term_count;
	char prefix_char;
	long should_segment;  // for keeping the old segment information
	char **special_tags;
	long has_title_tag;
	char *title_start;
	char *title_end;

private:
	void clean_up();
	void add_term(ANT_memory_indexer *index, int term_index, ANT_parser_token& what, char **start, long long doc);

public:
	ANT_readability_TAG_WEIGHTING();
	virtual ~ANT_readability_TAG_WEIGHTING();

	void handle_tag(ANT_parser_token *token, long tag_open, ANT_parser *parser, ANT_memory_indexer *index = NULL, long long doc = -1);
	void handle_token(ANT_parser_token *token);

	void index(ANT_memory_indexer *index, long long doc, ANT_directory_iterator_object *current_file);

	static void unscape_xml(std::string& text);
};

#endif /* READABILITY_TAG_WEIGHTING_H_ */

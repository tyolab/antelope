/*
 * readability_tag_finder.cpp
 *
 *  Created on: 23/11/2013
 *      Author: monfee
 */

#include "readability_tag_finder.h"

#ifndef FALSE
	#define FALSE 0
#endif
#ifndef TRUE
	#define TRUE (!FALSE)
#endif

char **ANT_readability_tag_finder::special_tags = (char *[]) {"category", "title"};

/*
	ANT_READABILITY_TAG_FINDER::~ANT_READABILITY_TAG_FINDER()
	---------------------------------------------------
*/
ANT_readability_tag_finder::ANT_readability_tag_finder()
{
	matching_tag = NULL;
	where = -1;
}

/*
	ANT_READABILITY_TAG_FINDER::~ANT_READABILITY_TAG_FINDER()
	---------------------------------------------------
*/
ANT_readability_tag_finder::~ANT_readability_tag_finder()
{

}

/*
	READABILITY_TAG_FINDER::HANDLE_TAG()
	----------------------------------
*/
void ANT_readability_tag_finder::handle_tag(ANT_string_pair *tag)
{
ANT_parser_token *token;

for (int i = 0; i < sizeof(*special_tags); ++i)
	if (strncmp(tag->start, special_tags[i], tag->string_length) == 0)
		{
		matching_tag = special_tags[i];
		where = i;
		}

if (where > -1)
	while ((token = parser->get_next_token()) != NULL && token->type != TT_TAG_CLOSE)
	if (where == 0)
		{

		}
	else if (where == 1)
		{

		}

where = -1;
}

/*
	ANT_READABILITY_DALE_CHALL::INDEX()
	-----------------------------------
*/
void ANT_readability_tag_finder::index(ANT_memory_indexer *indexer)
{

}

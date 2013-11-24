/*
 * readability_tag_finder.cpp
 *
 *  Created on: 23/11/2013
 *      Author: monfee
 */

#include "readability_tag_finder.h"
#include "unicode.h"

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
	term_count = 0;
	tag_processing_on = FALSE;
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
void ANT_readability_tag_finder::clean_up()
{

tag_processing_on;
where = -1;
matching_tag = NULL;
}


/*
	READABILITY_TAG_FINDER::HANDLE_TAG()
	----------------------------------
*/
void ANT_readability_tag_finder::handle_tag(ANT_string_pair *tag, long tag_open)
{
ANT_parser_token *token;

if (tag_open)
	{
	for (int i = 0; i < sizeof(*special_tags); ++i)
		if (strncmp(tag->start, special_tags[i], tag->string_length) == 0)
			{
			matching_tag = special_tags[i];
			where = i;
			tag_processing_on = TRUE;
			}
	}
else
	//if (strncmp(tag->start, matching_tag, tag->string_length) == 0) // we are not checking if the closing tag is the one being opened at the moment
		tag_processing_on = FALSE;
}

void ANT_readability_tag_finder::handle_token(ANT_string_pair* token)
{
if (tag_processing_on)
	{
	++term_count;
	if (where == 0)
		{

		}
	else if (where == 1)
		{

		}
	}
}

/*
	ANT_READABILITY_DALE_CHALL::INDEX()
	-----------------------------------
*/
void ANT_readability_tag_finder::index(ANT_memory_indexer *indexer, long long doc)
{
// now we add the full title, full category information in the dictionary
if (where == 0)
	{

	}
else if (where == 1)
	{

	}
clean_up();
}

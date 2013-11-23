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

ANT_readability_tag_finder::ANT_readability_tag_finder()
{

}

ANT_readability_tag_finder::~ANT_readability_tag_finder()
{

}

unsigned long ANT_readability_tag_finder::is_the_tag_looking_for(
		ANT_string_pair* token)
{
	for (int i = 0; i < sizeof(special_tags); ++i)
		if (strncmp(token->start, special_tags[i], token->string_length) == 0)
			return TRUE;
	return FALSE;
}

void ANT_readability_tag_finder::handle_tag(ANT_memory_indexer* indexer,
		ANT_string_pair* string, long long docno)
{

}

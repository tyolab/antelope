/*
	READABILITY_TAG_WEIGHTING.C
	---------------------------
 */

#include "readability_tag_weighting.h"
#include "unicode.h"
#include "directory_iterator_object.h"

#include <string.h>

#ifndef FALSE
	#define FALSE 0
#endif
#ifndef TRUE
	#define TRUE (!FALSE)
#endif

/*
  to add special term into vocab, it can't contain upper case letter, because it is for tag
 */
char *ANT_readability_TAG_WEIGHTING::special_tags_general[] = {"CATEGORY", "TITLE"};

char *ANT_readability_TAG_WEIGHTING::special_tags_extra = "";

char ANT_readability_TAG_WEIGHTING::buffer[MAX_TERM_LENGTH];

/*
	ANT_READABILITY_TAG_WEIGHTING::~ANT_READABILITY_TAG_WEIGHTING()
	---------------------------------------------------------------
*/
ANT_readability_TAG_WEIGHTING::ANT_readability_TAG_WEIGHTING()
{
number_of_tags = 2;
matching_tag = NULL;
where = -1;
term_count = 0;
tag_processing_on = FALSE;
terms = new char*[MAX_TERM_COUNT + 1];
has_title_tag = FALSE;
title_start = NULL;
title_end = NULL;

int tag_count = sizeof(special_tags_general)/sizeof(special_tags_general[0]);

int special_tag_count = tag_count;
special_tag_count += strlen(special_tags_extra);
special_tags = new char *[special_tag_count];
//	char **current = special_tags_general;
int count = 0;
for (; count < tag_count; ++count)
	special_tags[count] = strnew(special_tags_general[count]);

char *p = strtok(special_tags_extra, "+:-");
while (p)
	{
	special_tags[count] = strnew(p);
	++count;
	p = strtok(NULL, "+:-");
	}
number_of_tags = count;
}

/*
	ANT_READABILITY_TAG_WEIGHTING::~ANT_READABILITY_TAG_WEIGHTING()
	---------------------------------------------------------------
*/
ANT_readability_TAG_WEIGHTING::~ANT_readability_TAG_WEIGHTING()
{
clean_up();
delete [] terms;

for (int i = 0; i < number_of_tags; ++i)
	delete [] special_tags[i];
delete [] special_tags;
}

/*
	READABILITY_TAG_WEIGHTING::HANDLE_TAG()
	---------------------------------------
*/
void ANT_readability_TAG_WEIGHTING::clean_up()
{
buffer[0] = '\0';
if (term_count > 0)
	{
	for (int i = 0; i < term_count; ++i)
		delete terms[i];
	term_count = 0;
	}
tag_processing_on = FALSE;
where = -1;
/*
 don't do this, because matching_tag is the address of special tag, it might cause the special tag error later
 matching_tag = NULL;
 */
}

/*
	READABILITY_TAG_WEIGHTING::ADD_TERM()
	-------------------------------------
*/
void ANT_readability_TAG_WEIGHTING::add_term(ANT_memory_indexer *indexer, int i, ANT_parser_token& what, char **start, long long doc)
{
long long length = strlen(terms[i]);
memcpy(*start, terms[i], length);
*start += length;

**start = '\0';
what.string_length += length;

indexer->add_term(&what, doc, 20);
}

/*
	READABILITY_TAG_WEIGHTING::HANDLE_TAG()
	---------------------------------------
*/
void ANT_readability_TAG_WEIGHTING::handle_tag(ANT_parser_token *tag, long tag_open, ANT_parser *parser, ANT_memory_indexer *indexer, long long doc)
{
if (tag_open)
	{
	matching_tag = NULL;
	for (int i = 0; i < number_of_tags; ++i)
		if (strncmp(tag->start, special_tags[i], tag->string_length) == 0)
			{
			matching_tag = special_tags[i];
			where = i;
			prefix_char = ANT_tolower(matching_tag[0]);
			tag_processing_on = TRUE;
			should_segment = parser->get_segment_info();
			parser->set_segment_info(ANT_parser::NOSEGMENTATION);
			break;
			}
	if (tag_processing_on && matching_tag &&strcmp(matching_tag, "TITLE") == 0) 
		{
		title_start = tag->start + 6; // "TITLE>..."
		title_end = NULL;
		}
	}
else
	{
	if (tag_processing_on)
		{
		tag_processing_on = FALSE;
		parser->set_segment_info(should_segment);

		if (term_count > 0)
			{
			ANT_parser_token what;
			long long length = 0;
			int i;
			char *start;

			//if (strncmp(tag->start, matching_tag, tag->string_length) == 0) // we are not checking if the closing tag is the one being opened at the moment
			buffer[0] = info_buf[0] = prefix_char;
			buffer[1] = ':';
			/*
				put each special term into index, but term count has be greater than 1

				this might dramatically increase the size of index, we might just use bigram for it

				furthermore, bigram (phrase search) might be more usefull than one word

				also, we apply it on the title with term count of 3 and over

				and two terms have no spaces in between
			 */
			if (NULL != matching_tag && strcmp(matching_tag, "TITLE") == 0)
				{
				// closing TITLE tag
				title_end = tag->start - 2; // "</TITLE

				has_title_tag = TRUE;
				what.start = buffer;

				start = buffer + 2;
				what.string_length = 2; // including prefix string "C:" or "T:"

//				if (term_count > 2)
//					{
					int is_first_term_punct = ANT_ispunct(terms[0][0]) || utf8_ispuntuation(terms[0]);

					for (i = 0;;)
						{
					//	if (prefix_char == 'T' && i == 0)
					//		continue;
						/*
						  first term
						 */
						add_term(indexer, i, what, &start, doc);
						/*
						  just simply ignore all the spaces
						 */
		//				if (!is_first_term_punct && !is_cjk_language(terms[i-1]) && !ANT_ispunct(terms[i-1][0]) && !utf8_ispuntuation(terms[i-1])) // we need to restore the title, so only put spaces between characters that are not puntuations
		//					{
		//					*start++ = ' ';
		//					what.string_length++;
		//					}
						/*
						  second term
						 */
						if (++i < term_count)
							add_term(indexer, i, what, &start, doc);
						else
							break;

						start = buffer + 2;
						what.string_length = 2; // including prefix string "C:" or "T:"
						}
//					}
//				else
//					{
//					length = strlen(terms[0]);
//					memcpy(start, terms[0], length);
//					indexer->add_term(&what, doc, 20);
//					}
				}
			else
				for (i = 0; i < term_count; ++i)
					{
					what.start = buffer;
					what.string_length = 2;

					start = buffer + 2;

					length = strlen(terms[i]);
					memcpy(start, terms[i], length);
					start += length;

					*start = '\0';
					what.string_length += length;

					indexer->add_term(&what, doc, 20);
					}
			}

		clean_up();
		}
	}
}

/*
	READABILITY_TAG_WEIGHTING::HANDLE_TOKEN()
	-----------------------------------------
	when handling the tokens in the special tags, we don't want to break words from . - _, only space
	for example, <site>com.google.www</site>, we want it s:com.google.www not s:com s:google s:www
*/
void ANT_readability_TAG_WEIGHTING::handle_token(ANT_parser_token *token)
{
/*
  the title may begin with "Wikipedia:", if term_count is greater than 1, then we ignoring it
  it is not best solution, but we can live with it.


 */
//static const char *wiki_prefix = "wikipedia";
char *term, *start;

if (tag_processing_on && term_count <= MAX_TERM_COUNT)
	{

	if (token->type == TT_WORD || token->type == TT_NUMBER)
		{
		/*
			Wikipedia abstract file dump give title in such a way "wikipedia : XXXXX", term count is 1 when encounter the colon;
			for for Chinese, the unicode colon ':' = "\357\274\232" in OCT, and the term count for word Wikipedia is 4.
		 */
		if (matching_tag && (strcmp(matching_tag, "TITLE") == 0)
				&& (term_count == 1 && (strncmp(token->start, ":", token->string_length) == 0))
				|| (term_count == 4 && (strncmp(token->start, "\357\274\232", token->string_length) == 0))
				/*&& strcmp(wiki_prefix, terms[0]) == 0*/)
			{
				while (term_count > 0)
					delete terms[--term_count];

				term_count = 0;
				return;
			}

		terms[term_count] = strnnew(token->start, token->string_length);
		term = start = terms[term_count];
		while (start != NULL && (start - term) < strlen(term))
			start = utf8_tolower(start);
		++term_count;

		// finish dealing with the token, which should not be indexed again
		token->type = TT_INVALID;
		}
	}
}

/*
	ANT_READABILITY_DALE_CHALL::INDEX()
	-----------------------------------
*/
void ANT_readability_TAG_WEIGHTING::index(ANT_memory_indexer *indexer, long long doc, ANT_directory_iterator_object *current_file)
{
/*
  the boundary of the buffer wasn't checked assuming the text in the tile or category node won't be longer than that
 */

// now we add the full title, full category information in the dictionary
char *start, *info_buf_start;
ANT_parser_token what;
long long length = 0;

if (has_title_tag)
	{

	what.start = buffer;
	buffer[0] = info_buf[0] = 't'; // this is for the title

	/*
	   Now the full text enclosed in the node, for example, title "Bill Clinton"
	   we will put "TF:bill clinton" into dictionary
	 */

	info_buf[1] = 'f';
	info_buf[2] = ':';

	info_buf_start = info_buf + 3;
	*info_buf_start = '\0';

	what.start = info_buf;
	what.string_length = 3;

	/*
	  TODO
	 
	  this is assuming the filename fed in the title string
	  we might need a better a way to get the title

	  current_file->filename
	 */
	if (title_start && title_end)
		{
		std::string title(title_start, title_end - title_start);
		unscape_xml(title);

		length = title.length();

		memcpy(info_buf_start, title.c_str(), length);
		what.string_length += length;
		*(info_buf_start + length) = '\0';

		size_t normalized_string_length = 0;
		strcpy(what.normalized_buf, "tf:");
		start = what.normalized_buf + 3;

		int result = ANT_UNICODE_normalize_string_tolowercase(start, MAX_TERM_LENGTH, &normalized_string_length, (char *)title.c_str());

		if (result)
			{
			what.normalized.string_length = normalized_string_length + 3;

			indexer->add_term(&what.normalized, doc, 99); // avoid being culled of optimization

			}
		else
			{
			*what.normalized_buf = '\0';
			what.normalized.string_length = 0;


			while (*info_buf_start != '\0')
				info_buf_start = utf8_tolower(info_buf_start);

			indexer->add_term(&what, doc, 99); // avoid being culled of optimization
			}
		}
	}

clean_up();
}

/*
	ANT_READABILITY_DALE_CHALL::UNSCAPE_XML()
	-----------------------------------
*/
void ANT_readability_TAG_WEIGHTING::unscape_xml(std::string& source)
{
	static const char *entities[] = {"&quot;", "&lt;", "&gt;", "&apos;", "&amp;" };
	static const char *to_chars[] = {"\"", "<", ">", "'", "&" };
	static const std::size_t num = sizeof(entities)/sizeof(entities[0]);

	std::size_t j = 0;
	for (std::size_t i = 0; i < num; ++i)
		for (;(j = source.find(entities[i], j)) != std::string::npos;)
			{
			source.replace(j, strlen(entities[i]), to_chars[i]);
			}
}


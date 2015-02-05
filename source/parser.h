/*
	PARSER.H
	--------
*/
#ifndef PARSER_H_
#define PARSER_H_

#include "string_pair.h"
#include "parser_token.h"
#include "ctypes.h"
#include "unicode_tables.h"
#include "unicode.h"

#ifndef FALSE
	#define FALSE 0
#endif

#ifndef TRUE
	#define TRUE (!FALSE)
#endif

#ifdef _MSC_VER
	#define inline __forceinline
#endif

/*
	class ANT_PARSER
	----------------
*/
class ANT_parser
{
public:
	/*
		This might be confusing as previous definition.
		To index documents containing Chinese, by default Chinese is indexed by characters only; that we should segment it.
		DOUBLE_SEGMENTATION means string is segmented into words and also single characters for indexing
		ONFLY_SEGMENTATION means segment the sentence using the Chinese segmentation module when parsing the document
		NOSEGMENTATION is set that means the text is segmented / or just leave as it is.
	 */
	enum { NOSEGMENTATION = 0, SHOULD_SEGMENT = 1, ONFLY_SEGMENTATION = 2, DOUBLE_SEGMENTATION = 4, BIGRAM_SEGMENTATION = 8 };

protected:
	unsigned char *document;
	unsigned char *current;
	ANT_parser_token current_token;
	unsigned char *segmentation;
	long should_segment;

public:
	ANT_parser(long should_segment = SHOULD_SEGMENT);
	virtual ~ANT_parser();

	virtual void segment(unsigned char *start, long length);
	void set_document(char *document) { set_document((unsigned char *)document); }
	void set_document(unsigned char *document);
	virtual ANT_parser_token *get_next_token(void);
	long get_segment_info();
	void set_segment_info(long should_segment);
} ;

#endif  /* PARSER_H_ */



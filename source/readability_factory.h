/*
	READABILITY_FACTORY.H
	---------------------
*/
#ifndef READABILITY_FACTORY_H_
#define READABILITY_FACTORY_H_

#include "parser.h"
#include "readability.h"

/*
	class ANT_READABILITY_FACTORY
	-----------------------------
*/
class ANT_readability_factory : public ANT_readability
{
public:
	enum {
		NONE           = 0,
		DALE_CHALL     = 1,
		FLESCH_KINCAID = 2,
		TAG_FINDER =3
	};

private:
	long number_of_measures;
	ANT_readability **measure;
	unsigned long measures_to_use;

protected:
	ANT_parser *parser;

public:
	ANT_readability_factory();
	virtual ~ANT_readability_factory();
	
	ANT_parser_token *get_next_token(void);
	void handle_node(ANT_memory_indexer_node *node);
	void index(ANT_memory_indexer *index);
	
	void set_document(unsigned char *document);
	void set_measure(unsigned long value);
	void set_parser(ANT_parser *parser);

	unsigned long get_measure();
	ANT_readability *get_measure_to_use();
};


#endif  /* READABILITY_FACTORY_H_ */

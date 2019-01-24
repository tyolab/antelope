/*
	ATIRE_API_SERVER.H
	------------------
	Created on: 30 Nov 2015
    Author: Eric Tang (eric.tang@tyo.com.au)

	This server class is not thread safe, and it can only serve one request a time. 
	For parallel querying, should consider using multiple instances
*/

#ifndef ATIRE_API_SERVER_H_
#define ATIRE_API_SERVER_H_

#include "atire_api_result.h"
#include "compress.h"

class ATIRE_API;
class ANT_stop_word;
class ANT_channel;
class ANT_snippet;
class ANT_stem;
class ANT_stats_time;
class ANT_stats;
class ANT_ANT_param_block;
class ANT_btree_iterator;
class ANT_search_engine_btree_leaf;

class ATIRE_API_server
{
private:
    static const long MAX_RESULT_LENGTH = 1024 * 1024;
	static const long MAX_COMMAND_LENGTH = 4 * 1024;

	static const char *PROMPT;

	static const char *new_stop_words[];

	/*
		release memory on exit
	 */
	ATIRE_API *atire;
	ANT_ANT_param_block *params_ptr;
	ANT_ANT_param_block *params_rank_ptr;

	int interrupted;

	ANT_stop_word *stop_word_list;

	/*
	 	the run time fields block
	 */
	char *print_buffer, *pos;
	ANT_stats_time *post_processing_stats;
	char *command;
	char *command_buffer;
	char *query, *ranker;
	long topic_id, number_of_queries, number_of_queries_evaluated, evaluation;
	long long line;
	long long hits, result, last_to_list, first_to_list, search_time, page_size;

	int custom_ranking;
	double *average_precision, *sum_of_average_precisions, *mean_average_precision;
	double relevance;
	long length_of_longest_document;
	unsigned long current_document_length;
	long long docid;

	/*
		for ANT index version 3
	*/
	char **answer_list;

	/*
		for term listing
	*/
	ANT_btree_iterator *iterator;
	ANT_search_engine_btree_leaf *leaf;
	char *term;
	char *first_term;
	char *last_term;
	ANT_compressable_integer *current, *end;
	long long lookup_term_docid, lookup_term_max_id;

	#ifdef IMPACT_HEADER
	ANT_compressable_integer *impact_offset_start;
	ANT_compressable_integer *doc_count_ptr;
	#endif

	long term_position;
	unsigned char *postings_list_mem, *postings_list;
	ANT_compressable_integer *raw, *raw_mem;
	long long global_trim;
	long long document_frequency;
	char *lookup_term_buffer;

	char *document_buffer;
	ANT_channel *inchannel, *outchannel;

	ANT_snippet *snippet_generator;
	ANT_snippet *title_generator;
	ANT_stem *snippet_stemmer;

	ATIRE_API_result result_document;

	/*
		by default, we convert the result to JSON format
	 */
	char *formatted_result;

	ANT_stats *stats;

	// for keeping the list of arguments not the same as argv in term of memory
	char *options_copy;
	char **arg_list;
	int	argc;

	int output_format;

	long ant_version;

public:
	enum ATIRE_channel_type {CHANNEL_FILE = 0, CHANNEL_SOCKET = 1, CHANNEL_MEMORY = 3};
	enum ATIRE_output_format {XML = 0, JSON = 1};

public:
	ATIRE_API_server();
	virtual ~ATIRE_API_server();

	void initialize();

	ATIRE_API *get_atire() { return atire; }

	void set_params(int argc, char *argv[]);
	void set_params(char *args);

	void set_ant_version(long version);

	/* before ready */
	void start();

	/* Option 1. standalone*/
	void loop();

	/* Option 2. embeded in other process / application */
	void poll();
	void poll_and_process();
	void process_command();

	/*
		may need to implement a lock mechanism
	 */

	void interrupt();

	void finish();

	/* run the server in loop */
	int run();
	int run(char *files);

	void ant();

	void start_stats();
	void end_stats();

	ANT_stats *get_stats();
	long long get_search_time() { return search_time; }

	char *version();

	void prompt();

	int is_interrupted() const	{ return interrupted; }
	void set_interrupted(int interrupted) {	this->interrupted = interrupted; }

	/*
		Query Commands
	*/

	int has_new_command() { return command != NULL; };
	void insert_command(const char *cmd);

	/*
		Set the search result output channel
	*/
	void set_outchannel(long type);
	char *get_outchannel_content();

	/*
		Search
	*/
	long search(const char *query);
	long search();

	/*
		Go to next search result
	*/
	void goto_result(long index);

	/*
		List Term(s)
	*/
	ATIRE_API_result *list_term(char *term, long position = 0);

	/*
		Terms list relating positioning
	*/
	ATIRE_API_result *next_term(long to_position = 0);
	ATIRE_API_result *goto_term(long index);

	const char *result_to_json();
	ATIRE_API_result *get_result();
	long next_result();
	void result_to_outchannel(long last_to = -1);

	const char *load_document();
	const char *get_document(long docid);
	const char *get_current_document() { return document_buffer; }

	void set_page_size(const long page_size) { this->page_size = page_size; }

	/*
		List terms
	 */
	/* Not in use
	void listterms(const char *term);
	long next_term();
	*/
	void set_output_format(int format);

private:

	ATIRE_API *init();

	char *between(char *source, char *open_tag, char *close_tag);
	long ant_init_ranking();
	char *stop_query(char *query, long stop_type);
	double *perform_query(long topic_id, ANT_channel *outchannel, ANT_ANT_param_block *params, char *query, long long *matching_documents);

	void cleanup();
};

#endif /* ATIRE_API_SERVER_H_ */

/*
	ATIRE_API_REMOTE.H
	------------------
	Created on: 30 Nov 2015
    Author: Eric Tang (eric.tang@tyo.com.au)
*/

#ifndef ATIRE_API_SERVER_H_
#define ATIRE_API_SERVER_H_

#include "atire_api_result.h"

class ATIRE_API;
class ANT_stop_word;
class ANT_channel;
class ANT_snippet;
class ANT_stem;
class ANT_stats_time;
class ANT_stats;
class ANT_ANT_param_block;

class ATIRE_API_server
{
private:
    static const long MAX_RESULT_LENGTH = 1024 * 1024;

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
	char *command, *query, *ranker;
	long topic_id, number_of_queries, number_of_queries_evaluated, evaluation;
	long long line;
	long long hits, result, last_to_list, first_to_list, search_time;;

	int custom_ranking;
	double *average_precision, *sum_of_average_precisions, *mean_average_precision;
	double relevance;
	long length_of_longest_document;
	unsigned long current_document_length;
	long long docid;

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

public:
	enum {CHANNEL_FILE, CHANNEL_SOCKET, CHANNEL_STREAM};

public:
	ATIRE_API_server();
	virtual ~ATIRE_API_server();

	void initialize();

	ATIRE_API *get_atire() { return atire; }

	void set_params(int argc, char *argv[]);
	void set_params(char *args);

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

	/*
		Search
	*/
	long search(const char *query);
	long search();

	void goto_result(long index);

	const char *result_to_json();
	ATIRE_API_result *get_result();
	long next_result();
	void result_to_outchannel(long last_to = -1);

	const char *load_document();
	const char *get_current_document() { return document_buffer; }

	/*
		List terms
	 */
	/* Not in use
	void listterms(const char *term);
	long next_term();
	*/

private:

	ATIRE_API *init();

	char *between(char *source, char *open_tag, char *close_tag);
	long ant_init_ranking();
	char *stop_query(char *query, long stop_type);
	double *perform_query(long topic_id, ANT_channel *outchannel, ANT_ANT_param_block *params, char *query, long long *matching_documents);

	void cleanup();
};

#endif /* ATIRE_API_SERVER_H_ */

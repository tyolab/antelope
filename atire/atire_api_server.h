/*
	ATIRE_API_REMOTE.H
	------------------
	Created on: 30 Nov 2015
    Author: Eric Tang (eric.tang@tyo.com.au)
*/

#ifndef ATIRE_API_SERVER_H_
#define ATIRE_API_SERVER_H_

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
	static const char * PROMPT;
	static const char *new_stop_words[];
	static const long MAX_TITLE_LENGTH = 1024;

	/*
	 * release memory on exit
	 */
	ATIRE_API *atire;
	ANT_ANT_param_block *params_ptr;
	ANT_ANT_param_block *params_rank_ptr;

	int interrupted;

	ANT_stop_word *stop_word_list;

	/*
	 * the run time fields block
	 */
	char *print_buffer, *pos;
	ANT_stats_time *post_processing_stats;
	char *command, *query, *ranker;
	long topic_id, number_of_queries, number_of_queries_evaluated, evaluation;
	long long line;
	long long hits, result, last_to_list, first_to_list;

	int custom_ranking;
	double *average_precision, *sum_of_average_precisions, *mean_average_precision;
	double relevance;
	long length_of_longest_document;
	unsigned long current_document_length;
	long long docid;
	char *document_buffer;
	ANT_channel *inchannel, *outchannel;
	char *snippet, *title;
	ANT_snippet *snippet_generator;
	ANT_snippet *title_generator;
	ANT_stem *snippet_stemmer;
	#ifdef FILENAME_INDEX
		char *document_name;
	#else
		char **answer_list;
	#endif

	ANT_stats *stats;

public:
	ATIRE_API_server();
	virtual ~ATIRE_API_server();

	void initialize(int argc, char *argv[]);

	ATIRE_API *get_atire() { return atire; }

	void set_params(int argc, char *argv[]);

	/* before ready */
	void start();

	/* Option 1. standalone*/
	void loop();

	/* Option 2. embeded in other process / application */
	void poll();
	void poll_and_process();
	void process_command();

	/**
	 * may need to implement a lock mechanism
	 */

	void interrup();

	void finish();

	/* run the server in loop */
	int run(int argc, char *argv[]);
	int run(char *files);

	void ant();

	void start_stats();
	void end_stats();

	ANT_stats *get_stats();

private:

	ATIRE_API *init();

	void prompt();
	char *between(char *source, char *open_tag, char *close_tag);
	long ant_init_ranking();
	char *stop_query(char *query, long stop_type);
	double *perform_query(long topic_id, ANT_channel *outchannel, ANT_ANT_param_block *params, char *query, long long *matching_documents);
};

#endif /* ATIRE_API_SERVER_H_ */

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

class ATIRE_API_server
{
private:
	static const char *new_stop_words[];
	const long MAX_TITLE_LENGTH = 1024;

	/*
	 * release memory on exit
	 */
	ATIRE_API *atire;
	ANT_ANT_param_block *params_ptr;
	ANT_ANT_param_block *params_rank_ptr;

	int interrupted;

	ANT_stop_word *stop_word_list = NULL;

	/*
	 * the run time fields block
	 */
	char *print_buffer, *pos;
	ANT_stats_time post_processing_stats;
	char *command, *query, *ranker;
	long topic_id = -1, number_of_queries, number_of_queries_evaluated, evaluation;
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
	char *snippet = NULL, *title = NULL;
	ANT_snippet *snippet_generator = NULL;
	ANT_snippet *title_generator = NULL;
	ANT_stem *snippet_stemmer = NULL;
	#ifdef FILENAME_INDEX
		char *document_name;
	#else
		char **answer_list;
	#endif

public:
	ATIRE_API_server();
	virtual ~ATIRE_API_server();

	ATIRE_API *get_atire() { return atire; }

	void initialize();

	/* before ready */
	double *start();

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

	double *finish();

	int run(int argc, char *argv[]);
	int run(char *files);

	double *ant();

private:
	void prompt();
	char *between(char *source, char *open_tag, char *close_tag);
	long ant_init_ranking();
	char *stop_query(char *query, long stop_type);
	double *perform_query(long topic_id, ANT_channel *outchannel, ANT_ANT_param_block *params, char *query, long long *matching_documents);
};

#endif /* ATIRE_API_SERVER_H_ */

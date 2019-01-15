/*
	ATIRE_API_SERVER.C
	------------------
	Created on: 30 Nov 2015
    Author: Eric Tang (eric.tang@tyo.com.au)
*/
#include <stdio.h>
#include <string.h>
#include <sstream>
#include <new>
#include <ctype.h>
#include "atire_api.h"
#include "str.h"
#include "maths.h"
#include "ant_param_block.h"
#include "stats_time.h"
#include "channel_file.h"
#include "channel_socket.h"
#include "channel_trec.h"
#include "channel_inex.h"
#include "channel_memory.h"
#include "relevance_feedback_factory.h"
#include "ranking_function_pregen.h"
#include "ranking_function_factory_object.h"
#include "snippet.h"
#include "snippet_factory.h"
#include "stemmer_factory.h"
#include "stem.h"
#include "thesaurus_wordnet.h"
#include "focus.h"
#include "focus_lowest_tag.h"
#include "focus_result.h"
#include "focus_results_list.h"
#include "search_engine.h"
#include "memory_index_one_node.h"
#include "unicode.h"
#include "atire_api_server.h"

const char *ATIRE_API_server::PROMPT  = "]";

const char *ATIRE_API_server::new_stop_words[] =
		{
	//	"alternative",				 Needed for TREC topic 84
		"arguments",
		"can",
		"current",
		"dangers",
		"data",
		"description",
		"developments",
		"document",
		"documents",
		"done",
		"discuss",
		"discusses",
		"efforts",
		"enumerate",
		"examples",
		"help",
		"ideas",
		"identify",
		"inform",
		"information",
		"instances",
		"latest",
		"method",
		"narrative",
		"occasions",
		"problems",
		"provide",
		"relevant",
		"report",
		"reports",
		"state",
		"topic",
		NULL
		} ;


/*
	ATIRE_API_SERVER()
	------------------
*/
ATIRE_API_server::ATIRE_API_server()
{
atire = NULL;

formatted_result = new char [MAX_RESULT_LENGTH];

interrupted = 0;
length_of_longest_document = 0;
current_document_length = 0;
number_of_queries = 0;

sum_of_average_precisions = NULL;
average_precision = NULL;

number_of_queries_evaluated = 0;

first_to_list = 0;
last_to_list = 0;
docid = 0;
hits = 0;
custom_ranking = 0;
line = 0;

pos = NULL;
print_buffer = NULL;
document_buffer = NULL;

evaluation = 0;

relevance = 0.0;

query = NULL;
ranker = NULL;
command = command_buffer =  new char [MAX_COMMAND_LENGTH];

inchannel = NULL;
outchannel = NULL;

result = 0;

params_ptr = NULL;
params_rank_ptr = NULL;

stop_word_list = NULL;

snippet_generator = NULL;
title_generator = NULL;
snippet_stemmer = NULL;

post_processing_stats = NULL;
stats = NULL;

mean_average_precision = NULL;

topic_id = -1;

options_copy = NULL;
arg_list = NULL;
argc = 0; // argc should be at least 1, because argv[0] == program itself

output_format = JSON;
ant_version = ANT_V5;
}

/*
	~ATIRE_API_SERVER()
	------------------
*/
ATIRE_API_server::~ATIRE_API_server()
{
cleanup();
}

/*
	SET_OUTPUT_FORMAT()
	-------------------
*/
void ATIRE_API_server::set_output_format(int format) 
{
output_format = format;
}

/*
	ATIRE_API_SERVER::SET_ANT_VERSION()
	--------------------
*/
void ATIRE_API_server::set_ant_version(long version_number)
{
ant_version = version_number;
}

/*
	CLEANUP()
	---------
*/
void ATIRE_API_server::cleanup() {
if (atire) 
	{
	delete atire;
	atire = NULL;
	}

if (params_ptr) 
	{
	delete params_ptr;
	params_ptr = NULL;
	}

if (params_rank_ptr)
	{
	delete params_rank_ptr;
	params_rank_ptr = NULL;
	}

if (post_processing_stats)
	{
	delete post_processing_stats;
	post_processing_stats = NULL;
	}

if (stats)
	{
	delete stats;
	stats = NULL;
	}

if (mean_average_precision) 
	{
	delete [] mean_average_precision;
	mean_average_precision = NULL;
	}

if (arg_list)
	{
	delete [] arg_list;
	arg_list = NULL;
	}

if (options_copy)
	{
	delete [] options_copy;
	options_copy = NULL;
	}

if (formatted_result)
	{
	delete [] formatted_result;
	formatted_result = NULL;
	}

if (command_buffer)
	{
	delete [] command_buffer;
	command_buffer = NULL;
	}

}

/*
	BETWEEN()
	---------
*/
char *ATIRE_API_server::between(char *source, char *open_tag, char *close_tag)
{
char *start,*finish;

if ((start = strstr(source, open_tag)) == NULL)
	return NULL;

start += strlen(open_tag);

if ((finish = strstr(start, close_tag)) == NULL)
	return NULL;

return strnnew(start, finish - start);
}

/*
	INIT()
	------
*/
ATIRE_API *ATIRE_API_server::init()
{
ANT_ANT_param_block& params = *params_ptr;
ant_version = params.ant_version;
/*
	Instead of overwriting the global API, create a new one and return it.
	This way, if loading the index fails, we can still use the old one.
*/
ATIRE_API *atire = new ATIRE_API();
atire->set_ant_version(ant_version);

long fail;
ANT_thesaurus *expander;

if (params.logo)
	puts(atire->version());				// print the version string if we parsed the parameters OK

if (params.ranking_function == ANT_ranking_function_factory_object::READABLE)
	fail = atire->open(ATIRE_API::READABILITY_SEARCH_ENGINE | params.file_or_memory, params.index_filename, params.doclist_filename, params.quantization, params.quantization_bits, params.header_offset);
else
	fail = atire->open(params.file_or_memory, params.index_filename, params.doclist_filename, params.quantization, params.quantization_bits, params.header_offset);

if (params.inversion_type == ANT_indexer_param_block_topsig::TOPSIG)
	atire->load_topsig(params.topsig_width, params.topsig_density, params.topsig_global_stats);

if (fail)
	{
	delete atire;

	atire = NULL;
	}
else
	{
	/* Load in all the pregens */
	for (int i = 0 ; i < params.pregen_count; i++)
		{
		//Derive the pregen's filename from the index filename and pregen fieldname
		std::stringstream buffer;

		buffer << params.index_filename << "." << params.pregen_names[i];

		if (!atire->load_pregen(buffer.str().c_str()))
			fprintf(stderr, "Failed to load pregen %s, ignoring...\n", params.pregen_names[i]);
		}

	if (params.assessments_filename != NULL)
		atire->load_assessments(params.assessments_filename, params.evaluator);

	if (params.output_forum != ANT_ANT_param_block::NONE)
		atire->set_forum(params.output_forum, params.output_filename, params.participant_id, params.run_name, params.results_list_length);

	atire->set_trim_postings_k(params.trim_postings_k);
	atire->set_stemmer(params.stemmer, params.stemmer_similarity, params.stemmer_similarity_threshold);
	atire->set_feedbacker(params.feedbacker, params.feedback_documents, params.feedback_terms);
	if (params.feedbacker == ANT_relevance_feedback_factory::BLIND_RM)
		atire->set_feedback_interpolation(params.feedback_lambda);

	if ((params.query_type & ATIRE_API::QUERY_EXPANSION_INPLACE_WORDNET) != 0)
		{
		atire->set_inplace_query_expansion(expander = new ANT_thesaurus_wordnet("wordnet.aspt"));
		expander->set_allowable_relationships(params.expander_tf_types);
		}
	if ((params.query_type & ATIRE_API::QUERY_EXPANSION_WORDNET) != 0)
		{
		atire->set_query_expansion(expander = new ANT_thesaurus_wordnet("wordnet.aspt"));
		expander->set_allowable_relationships(params.expander_query_types);
		}

	atire->set_segmentation(params.segmentation);

	// can't be initialized here
	// ant_init_ranking(); //Error value ignored...

	atire->set_processing_strategy(params.processing_strategy, params.quantum_stopping);

	// set the pregren to use for accumulator initialisation
	if (params.ranking_function != ANT_ranking_function_factory_object::PREGEN)
		atire->get_search_engine()->results_list->set_pregen(atire->get_pregen(), params.pregen_ratio);
	}
return atire;
}

/*
	SET_OUTCHANNEL()
	----------------
*/
void ATIRE_API_server::set_outchannel(long type)
{
	if (outchannel)
		delete outchannel;

	switch (type)
		{
		case CHANNEL_FILE:
			outchannel = new ANT_channel_file;
			break;
		case CHANNEL_SOCKET:
			outchannel = new ANT_channel_socket(params_ptr->port);
			break;
		case CHANNEL_MEMORY:
			outchannel = new ANT_channel_memory;
			break;
		default:
			break;
		}
}

/*
	GET_OUTCHANNEL_CONTENT()
	------------------------
*/
char *ATIRE_API_server::get_outchannel_content()
{
return outchannel->gets();	
}

/*
	START()
	--------
*/
void ATIRE_API_server::start()
{
ANT_ANT_param_block *params = params_ptr;
/*ANT_ANT_param_block **/params_rank_ptr = new ANT_ANT_param_block(params->argc, params->argv);

if (params->port != 0)
	inchannel = outchannel = new ANT_channel_socket(params->port);	// in/out to given port
else
	{
	inchannel = new ANT_channel_file(params->queries_filename);		// my stdin

	if (!outchannel)
		outchannel = new ANT_channel_file();							// my stdout

	if (params->queries_filename != NULL)
		{
		char first_bytes[0x200];		// enough to hold the INEX DTD and then a bit
		/*
			We might be a TREC topic file (usually a .gz) or an INEX topic file (usually a .zip) so we'll file out which.
			Its unreasonable to make the assumption based on the file type (who knows, INEX might start using .gz or
			TREC might start using .zip) so we'll open the file, read the first few bytes, and guess.  Guessing also
			allows us to handle ANT query files (one topic per line, in the form <id> <query>).
		*/
		ANT_channel_file test_channel(params->queries_filename);
		if (test_channel.read(first_bytes, sizeof(first_bytes)) == NULL)
			{
			/*
				Does the file exist?
			*/
			ANT_channel_file test_channel(params->queries_filename);
			if (test_channel.read(first_bytes, 1) == NULL)
				exit(printf("Query file '%s' not found!", params->queries_filename));

			/*
				else
					probably and ANT file as its very small.
			*/
			//exit(printf("Cannot determine the format of the queries file... is it valid?"));
			}
		else
			{
			/*
				We can use the first few bytes to guess the file type
			*/
			if (atoll(first_bytes) != 0)
				{
				/*
					no work required because we're a compressed ANT formatted queries file
				*/
				}
			else if (strstr(first_bytes, "inex-topic-file") != NULL)
				inchannel = new ANT_channel_inex(inchannel, params->query_fields);
			else if (strstr(first_bytes, "<top>") != NULL)
				inchannel = new ANT_channel_trec(inchannel, params->query_fields);
			else
				{
				/*
					We're going to guess based on file type
				*/
				if (strrcmp(params->queries_filename, "gz") == 0)			// probably a TREC file
					inchannel = new ANT_channel_trec(inchannel, params->query_fields);
				else if (strrcmp(params->queries_filename, "zip") == 0)		// probably an INEX file
					inchannel = new ANT_channel_trec(inchannel, params->query_fields);
				}
			}
		}
	}

print_buffer = new char [ATIRE_API_result::MAX_TITLE_LENGTH + 1024];

if (atire != NULL)
	{
	length_of_longest_document = atire->get_longest_document_length();
	document_buffer = new char [length_of_longest_document + 1];
	}
else
	{
	length_of_longest_document = 0;
	document_buffer = NULL;
	}

sum_of_average_precisions = new double[params->evaluator->number_evaluations_used];
for (evaluation = 0; evaluation < params->evaluator->number_evaluations_used; evaluation++)
	sum_of_average_precisions[evaluation] = 0.0;
number_of_queries = number_of_queries_evaluated = 0;
line = 0;
custom_ranking = 0;

if (params->snippet_algorithm != ANT_ANT_param_block::NONE)
	{
	result_document.snippet = new (std::nothrow) char [params->snippet_length + 1];
	*result_document.snippet = '\0';
	if (params->snippet_stemmer == ANT_stemmer_factory::NONE)
		snippet_stemmer = NULL;
	else
		if ((snippet_stemmer = ANT_stemmer_factory::get_core_stemmer(params->snippet_stemmer)) == NULL)
			exit(printf("Invalid snippet stemmer requested somehow\n"));

	snippet_generator = ANT_snippet_factory::get_snippet_maker(params->snippet_algorithm, params->snippet_length, atire->get_longest_document_length(), params->snippet_tag, atire->get_search_engine(), snippet_stemmer, params->snippet_word_cloud_terms);
	}

if (params->title_algorithm != ANT_ANT_param_block::NONE)
	{
	result_document.title = new (std::nothrow) char [params->title_length + 1];
	*result_document.title = '\0';
	title_generator = ANT_snippet_factory::get_snippet_maker(params->title_algorithm, params->title_length, atire->get_longest_document_length(), params->title_tag);
	}
}

void ATIRE_API_server::finish()
{
ANT_ANT_param_block *params = params_ptr;
/*
	delete the document buffer (and the "filename" buffer).
*/
delete [] document_buffer;

/*
	Compute Mean Average Precision
*/
mean_average_precision = new double[params->evaluator->number_evaluations_used];
for (evaluation = 0; evaluation < params->evaluator->number_evaluations_used; evaluation++)
	mean_average_precision[evaluation] = sum_of_average_precisions[evaluation] / (double)number_of_queries_evaluated;

/*
	Report MAP
*/
if (params->assessments_filename != NULL && params->stats & ANT_ANT_param_block::PRECISION)
	{
	printf("\nProcessed %ld topics (%ld evaluated):\n", number_of_queries, number_of_queries_evaluated);
	for (evaluation = 0; evaluation < params->evaluator->number_evaluations_used; evaluation++)
		printf("%s: %f\n", params->evaluator->evaluation_names[evaluation], mean_average_precision[evaluation]);
	puts("");
	}

/*
	Report the summary of the stats
*/
if (params->stats & ANT_ANT_param_block::SUM)
	atire->stats_all_text_render();

/*
	Clean up
*/
if (outchannel != inchannel)
	delete outchannel;
delete inchannel;

delete [] print_buffer;
delete [] sum_of_average_precisions;
delete snippet_generator;
delete title_generator;
delete snippet_stemmer;

/*
	And finally report MAP
*/
//return mean_average_precision;

// clean up main resources
end_stats();

cleanup();
}

long ATIRE_API_server::ant_init_ranking()
{
ANT_ANT_param_block& params = *params_ptr;
long ranker_ok;

if (params.ranking_function == ANT_ranking_function_factory_object::PREGEN)
	ranker_ok = atire->set_ranking_function_pregen(params.field_name, params.p1) == 0;
else
	ranker_ok = atire->set_ranking_function(params.ranking_function, params.quantization, params.quantization_bits, params.p1, params.p2, params.p3) == 0;

if (!ranker_ok)
	return ranker_ok;

return atire->set_feedback_ranking_function(params.feedback_ranking_function, params.quantization, params.quantization_bits, params.feedback_p1, params.feedback_p2, params.feedback_p3) == 0;

}

/*
	ANT()
	-----
*/
void ATIRE_API_server::ant()
{
/*
 * start the server but just doing the
 */
start_stats();

start();

loop();

finish();
}

int ATIRE_API_server::run()
{

initialize();

ant();

return 0;
}

/*
	RUN()
	-----
*/
int ATIRE_API_server::run(char* options)
{

set_params(options);

int result = run();

return result;
}

/*
	LOOP()
	------
*/
void ATIRE_API_server::loop()
{
prompt();
poll();
for (; has_new_command() && !is_interrupted(); prompt(), poll())
	{
	process_command();

	if (is_interrupted())
		break;
	}
}

/*
	PROMPT()
	--------
*/
void ATIRE_API_server::prompt()
{
if (params_ptr->queries_filename == NULL && params_ptr->port == 0)		// coming from stdin
	printf("%s", PROMPT);
}

/*
	INSERT_COMMAND()
	----------------
*/
void ATIRE_API_server::insert_command(const char *cmd)
{
long len = strlen(cmd);
long max_len = MAX_COMMAND_LENGTH < len ? MAX_COMMAND_LENGTH : len;
memcpy(command_buffer, cmd, max_len);
command_buffer[len] = '\0';
command = command_buffer;
}

/*
	RESULT_TO_OUTCHANNEL()
	----------------------
*/
void ATIRE_API_server::result_to_outchannel(long last_to)
{
	
if (last_to <= 0) 
	last_to_list = first_to_list + params_ptr->results_list_length;	
else
	last_to_list = last_to;		

/*
	How many results to display on the screen.
*/
if (first_to_list > hits)
	first_to_list = last_to_list = hits;
if (first_to_list < 0)
	first_to_list = 0;
if (last_to_list > hits)
	last_to_list = hits;
if (last_to_list < first_to_list)
	last_to_list = first_to_list;

ANT_ANT_param_block *params = params_ptr;	
if (params->stats & ANT_ANT_param_block::SHORT)
	outchannel->puts("<ATIREsearch>");

/*
	Report
*/
if (params->stats & ANT_ANT_param_block::SHORT)
	*outchannel << "<query>" << query << "</query>" << "<numhits>" << hits << "</numhits>" << "<time>" << search_time << "</time>" << ANT_channel::endl;
	

/*
	Report the average precision for the query
*/
if ((params->assessments_filename != NULL) && ((params->stats & ANT_ANT_param_block::SHORT) != 0) && (average_precision != NULL))
	{
	*outchannel << "<topic>" << topic_id << "</topic>" << ANT_channel::endl;
	*outchannel << "<evaluations>";
	for (evaluation = 0; evaluation < params->evaluator->number_evaluations_used; evaluation++)
		{
		sprintf(print_buffer, "%f", average_precision[evaluation]);
		*outchannel << "<" << params->evaluator->evaluation_names[evaluation] << ">" << print_buffer << "</" << params->evaluator->evaluation_names[evaluation] << ">";
		}
	outchannel->puts("</evaluations>");
	}

if (first_to_list < last_to_list)
	outchannel->puts("<hits>");	

	while (next_result())
		{
		*outchannel << "<hit>";
		*outchannel << "<rank>" << result_document.rank << "</rank>";
		*outchannel << "<id>" << result_document.docid << "</id>";
		// #ifdef FILENAME_INDEX
		if (ant_version == ANT_V5) 
			{
			*outchannel << "<name>" << atire->get_document_filename(result_document.document_name, result_document.docid) << "</name>";
			}
		else 
			{
			// #else
			*outchannel << "<name>" << answer_list[result] << "</name>";
			// #endif
			}
		sprintf(print_buffer, "%0.2f", result_document.rsv);
		*outchannel << "<rsv>" << print_buffer << "</rsv>";
		if (result_document.title != NULL && *result_document.title != '\0')
			*outchannel << "<title>" << result_document.title << "</title>";
		if (result_document.snippet != NULL && *result_document.snippet != '\0')
			*outchannel << "<snippet>" << result_document.snippet << "</snippet>";
		*outchannel << "</hit>" << ANT_channel::endl;
		}

	if (first_to_list < last_to_list)
		outchannel->puts("</hits>");
if (params->stats & ANT_ANT_param_block::SHORT)
	outchannel->puts("</ATIREsearch>");
}

/*
	SEARCH()
	--------
*/
long ATIRE_API_server::search(const char *q)
{
// make a copy of the query (q)
insert_command(q);

topic_id = -1;
query = command;

return search();
}

/*
	SEARCH()
	--------
*/
long ATIRE_API_server::search()
{
ANT_ANT_param_block *params = params_ptr;	
first_to_list = 0;
/*
	Do the query and compute average precision
*/
number_of_queries++;
average_precision = perform_query(topic_id, outchannel, params, query, &hits);
if (average_precision != NULL)
	{
	number_of_queries_evaluated++;
	for (evaluation = 0; evaluation < params->evaluator->number_evaluations_used; evaluation++)
		sum_of_average_precisions[evaluation] += average_precision[evaluation];		// zero if we're using a focused metric
	}
	
result = first_to_list;
last_to_list = hits;
/*
	Convert from a results list into a list of documents and then display (or write to the forum file)
*/
// if (params->output_forum != ANT_ANT_param_block::NONE)
// 	atire->write_to_forum_file(topic_id);
// else
// 	{
if (ant_version != ANT_V5) 
//#ifndef FILENAME_INDEX
	answer_list = atire->generate_results_list();
// #endif

/*
	We're going to generate snippets to parse the query for that purpose
*/
if (snippet_generator != NULL)
	snippet_generator->parse_query(query);

return hits;
}

/*
	GOTO_RESULT()
	-------------
*/
void ATIRE_API_server::goto_result(long index)
{
if (index >= last_to_list) 
	result = last_to_list;
else if (index < 0)
	result = 0;
else
	result = index;
}

/*
	RESULT_TO_JSON()
	----------------
*/
ATIRE_API_result *ATIRE_API_server::get_result()
{
	return &result_document;
}

/*
	RESULT_TO_JSON()
	----------------
*/
const char *ATIRE_API_server::result_to_json()
{
	static const char *json_template = "{"
					"\"rank\":%lld,"
					"\"id\":%lld,"
					"\"name\":\"%s\","
					"\"rsv\":%0.2f,"
					"\"title\":\"%s\","
					"\"snippet\":\"%s\""
					"}";
	sprintf(formatted_result, 
				json_template, 
				result_document.rank, 
				result_document.docid, 
				result_document.document_name, 
				result_document.rsv, 
				!result_document.title ? ATIRE_API_result::EMPTY_STRING : result_document.title, 
				!result_document.snippet ? ATIRE_API_result::EMPTY_STRING : result_document.snippet);
	return formatted_result;
}

/*
	NEXT_RESULT()
	-------------
*/
long ATIRE_API_server::next_result()
{
*document_buffer = '\0';

if (result < last_to_list) 
	{
	result_document.rank = result;
	result_document.docid = atire->get_relevant_document_details(result, &docid, &relevance);
	result_document.rsv = relevance;

	if ((current_document_length = length_of_longest_document) != 0)
		{
		/*
			Load the document if we need a snippet or a title
		*/
		if (title_generator != NULL || snippet_generator != NULL)
			load_document();

		/*
			Generate the title
		*/
		if (title_generator != NULL)
			title_generator->get_snippet(result_document.title, document_buffer);
		else
			result_document.title = NULL;

		/*
			Generate the snippet
		*/
		if (snippet_generator != NULL)
			snippet_generator->get_snippet(result_document.snippet, document_buffer);
		else
			result_document.snippet = NULL;
		}	 

	// #ifdef FILENAME_INDEX
	if (ant_version == ANT_V5) 
		atire->get_document_filename(result_document.document_name, result_document.docid);
	else
	// #else
		memcpy(result_document.document_name, answer_list[result], strlen(answer_list[result]));
	// #endif
	
	result++;
	return TRUE;
	}

return FALSE;
}

/*
	LOAd_DOCUMENT()
	---------------
*/
const char *ATIRE_API_server::load_document()
{
return get_document(result_document.docid);
}

/*
	GET_DOCUMENT()
	---------------
*/
const char *ATIRE_API_server::get_document(long docid)
{
atire->get_document(document_buffer, &current_document_length, docid);
return document_buffer;
}

/*
	PROCESS_COMMAND()
	-----------------
*/
void ATIRE_API_server::process_command()
{
ANT_ANT_param_block *params = params_ptr;
if (custom_ranking)
	{
	/* Revert to default ranking function for next query */
	ant_init_ranking(); //Just assume that it worked.
	custom_ranking = 0;
	}

first_to_list = 0;
last_to_list = first_to_list + params->results_list_length;

line++;
/*
	Parsing to get the topic number
*/
strip_space_inplace(command);

if (strcmp(command, ".loadindex") == 0 || strncmp(command, "<ATIREloadindex>", 16) == 0)
	{
	/*
		NOTE: Do not expose this command to untrusted users as it could almost certainly
		cause arbitrary code execution by loading specially-crafted attacker-controlled indexes.
	*/
	char *oldindexfilename, *olddoclistfilename;

	if (strcmp(command, ".loadindex") == 0)
		{
		olddoclistfilename = params->swap_doclist_filename(strip_space_inplace(inchannel->gets()));
		oldindexfilename = params->swap_index_filename(strip_space_inplace(inchannel->gets()));
		}
	else
		{
		olddoclistfilename = params->swap_doclist_filename(between(command, "<doclist>", "</doclist>"));
		oldindexfilename = params->swap_index_filename(between(command, "<index>", "</index>"));
		}

	if (strlen(params->doclist_filename) == 0 && strlen(params->index_filename) == 0)
		{
		/* This is a request to unload the index */
		delete atire;
		atire = NULL;

		delete [] olddoclistfilename;
		delete [] oldindexfilename;

		outchannel->puts("<ATIREloadindex>1</ATIREloadindex>");
		}
	else
		{
		ATIRE_API *new_api = init();

		if (new_api)
			{
			delete [] olddoclistfilename;
			delete [] oldindexfilename;

			delete atire;
			atire = new_api;

			length_of_longest_document = atire->get_longest_document_length();
			delete [] document_buffer;
			document_buffer = new char [length_of_longest_document + 1];

			outchannel->puts("<ATIREloadindex>1</ATIREloadindex>");
			}
		else
			{
			/* Leave global 'atire' unchanged */
			outchannel->puts("<ATIREloadindex>0</ATIREloadindex>");

			/* Restore the filenames in params for later .describeindex queries to return, and delete the filenames
			 * we tried to load */
			delete [] params->swap_doclist_filename(olddoclistfilename);
			delete [] params->swap_index_filename(oldindexfilename);
			}
		}
	// delete [] command;
	return; // continue;
	}
else if (strcmp(command, ".quit") == 0)
	{
	outchannel->puts("<ATIREresult>");
	outchannel->puts("ATIRE is now existing.");
	outchannel->puts("</ATIREresult>");
	// delete [] command;
	interrupted = 1; // break;
	return;
	}
else if (*command == '\0')
	{
	// delete [] command;
	return; // continue;			// ignore blank lines
	}
else
	{
	/* Commands that require a working atire instance */
	if (atire == NULL)
		{
		outchannel->puts("<ATIREerror>");
		outchannel->puts("<description>No index loaded</description>");
		outchannel->puts("</ATIREerror>");
		// delete [] command;
		return; // continue;
		}

	if (strncmp(command, "<ATIREdescribeindex>", 18) == 0)
		{
		// delete [] command;

		outchannel->puts("<ATIREdescribeindex>");

		outchannel->write("<doclist filename=\"");
		outchannel->write(params->doclist_filename);
		outchannel->puts("\"/>");

		outchannel->write("<index filename=\"");
		outchannel->write(params->index_filename);
		outchannel->puts("\"/>");
		outchannel->write("<docnum>");
		outchannel->write(atire->get_document_count());
		outchannel->puts("</docnum>");
		outchannel->write("<termnum>");
		outchannel->write(atire->get_term_count());
		outchannel->puts("</termnum>");
#ifdef IMPACT_HEADER
		outchannel->write("<uniquetermnum>");
		outchannel->write(atire->get_unique_term_count());
		outchannel->puts("</uniquetermnum>");
#endif
		outchannel->write("<quantized>");
		long long var = atire->get_search_engine()->get_is_quantized();
		outchannel->write(var);
		outchannel->puts("</quantized>");
		var = atire->get_search_engine()->get_variable("~quantmax");
		outchannel->write("<quantmax>");
		printf("%f", *(double *)&var);
		outchannel->puts("</quantmax>");
		var = atire->get_search_engine()->get_variable("~quantmin");
		outchannel->write("<quantmin>");
		printf("%f", *(double *)&var);
		outchannel->puts("</quantmin>");
		outchannel->write("<longestdoc>");
		outchannel->write(atire->get_longest_document_length());
		outchannel->puts("</longestdoc>");
		outchannel->puts("</ATIREdescribeindex>");

		return; // continue;
		}
	else if (strcmp(command, ".describeindex") == 0)
		{
		// delete [] command;

		outchannel->puts(params->doclist_filename);
		outchannel->puts(params->index_filename);
		outchannel->write(atire->get_document_count());
		outchannel->puts("");
		return; // continue;
		}
	else if (strncmp(command, ".morelike ", 10) == 0)
		{
		*document_buffer = '\0';
		if ((current_document_length = length_of_longest_document) != 0)
			{
			atire->get_document(document_buffer, &current_document_length, atoll(command + 10));
			query = atire->extract_query_terms(document_buffer, 10);		// choose top 10 terms

			// delete [] command;
			command = query;
			}
		else
			{
			// delete [] command;
			return; // continue;
			}
		}
	else if (strncmp(command, ".get ", 5) == 0)
		{
		*document_buffer = '\0';
		if ((current_document_length = length_of_longest_document) != 0)
			{
			atire->get_document(document_buffer, &current_document_length, atoll(command + 5));

			if (params->focussing_algorithm == ANT_ANT_param_block::RANGE)
				{
				/*
					Setup for the focussing game
				*/
				ANT_focus_results_list focused_results_list(10);		// top 10 focused results
				ANT_focus_result *document_range;
				ANT_focus *focusser = new ANT_focus_lowest_tag(&focused_results_list);
				long focus_length;
				unsigned char zero_x_ff[] = {0xff, 0x00};
				ANT_string_pair *search_terms, *current_search_term, *token;
				ANT_parser parser;

				/*
					Prime the focusser by adding the search terms to the matcher
				*/
				current_search_term = search_terms = new ANT_string_pair [strcountchr(command, ' ')];
				parser.set_document(command + 1);			// skip over the '.'
				parser.get_next_token();					// get
				parser.get_next_token();					// docid
				while ((token = parser.get_next_token()) != NULL)
					{
					*current_search_term = *token;
					focusser->add_term(current_search_term);			// search terms
					current_search_term++;
					}

				/*
					Focus the document
				*/
				document_range = focusser->focus((unsigned char *)document_buffer, &focus_length);

				/*
					Send the document length (with space added for the markers)
				*/
				*outchannel << current_document_length + 2 << ANT_channel::endl;

				/*
					Send:
						The first part of the document
						The start marker
						the middle of the document
						The end marker
						The end of the document

				*/
				outchannel->write(document_buffer, document_range->start - document_buffer);
				outchannel->write(zero_x_ff, 1);
				outchannel->write(document_range->start, document_range->finish- document_range->start);
				outchannel->write(zero_x_ff, 1);
				outchannel->write(document_range->finish, current_document_length - (document_range->finish - document_range->start));

				/*
					Clean up
				*/
				delete [] search_terms;
				}
			else
				{
				*outchannel << current_document_length << ANT_channel::endl;
				outchannel->write(document_buffer, current_document_length);
				}
			}
		// delete [] command;
		return; // continue;
		}
	else if (strncmp(command, ".listterm ", 10) == 0)
		{
		char *start = command + 10;
		size_t buffer_length = strlen(start) * 2 + 1;
		char *buffer = new char[buffer_length];
		size_t normalized_string_length = 0;
		int result = ANT_UNICODE_normalize_string_tolowercase(buffer, buffer_length, &normalized_string_length, start);

		long limits = 100;
		static ANT_compression_factory factory;
//			static char metaphone_buffer[1024];
		long long postings_list_size = 5* 1024 * 1024;  // for the use of mobile,
		static long long raw_list_size = 5 * 1024 * 1024; // same as above
		char *term, *first_term, *last_term = NULL;
		long long global_trim;
		unsigned char *postings_list_mem = NULL, *postings_list = NULL;
		ANT_compressable_integer *raw = NULL;
//			ANT_stem *meta = NULL;
		long long docid = -1, max;
		ANT_compressable_integer *current;
		int count ;
		long long tf = 0;
		ANT_search_engine *search_engine = atire->get_search_engine();
		count = 0;
//			first_term = command + 10;
		if (result)
			{
			// delete [] command;
			first_term = command = buffer;
			}
		else
			{
			delete [] buffer;
			first_term = start;
			}

		ANT_btree_iterator iterator(search_engine);
		ANT_search_engine_btree_leaf leaf;

//			long metaphone, print_wide, print_postings, one_postings_per_line;

		global_trim = search_engine->get_global_trim_postings_k();

#ifdef IMPACT_HEADER
		quantum_count_type the_quantum_count;
		beginning_of_the_postings_type beginning_of_the_postings;
		long long impact_header_info_size = ANT_impact_header::INFO_SIZE;
		long long impact_header_size = ANT_impact_header::NUM_OF_QUANTUMS * sizeof(ANT_compressable_integer) * 3;
		ANT_compressable_integer *impact_header_buffer = (ANT_compressable_integer *)malloc(impact_header_size);
#endif

#ifndef ATIRE_MOBILE
		postings_list_size = search_engine->get_postings_buffer_length();
#ifdef IMPACT_HEADER
		raw_list_size = sizeof(*raw) * (search_engine->document_count() + ANT_COMPRESSION_FACTORY_END_PADDING);
#else
		raw_list_size = sizeof(*raw) * (512 + search_engine->document_count() + ANT_COMPRESSION_FACTORY_END_PADDING);
#endif
#endif
		postings_list_mem = postings_list = (unsigned char *)malloc((size_t)postings_list_size);
		raw = (ANT_compressable_integer *)malloc((size_t)raw_list_size);
		*outchannel << "<ATIREresult>" << ANT_channel::endl;
		for (term = iterator.first(first_term); term != NULL && count < limits; term = iterator.next())
			{
			docid = -1;

			iterator.get_postings_details(&leaf);
			if (first_term != NULL && strncmp(first_term, term, strlen(first_term)) != 0)
				break;
			else
				{
				if (last_term != NULL && strcmp(last_term, term) < 0)
					break;
				else
					{
					if (*term != '~')		// ~length and others aren't encoded in the usual way
						{
						postings_list = search_engine->get_postings(&leaf, postings_list_mem);

						long long document_frequency = ANT_min(leaf.local_document_frequency, global_trim);

#ifdef IMPACT_HEADER
						// decompress the header
						the_quantum_count = ANT_impact_header::get_quantum_count(postings_list);
						beginning_of_the_postings = ANT_impact_header::get_beginning_of_the_postings(postings_list);
						factory.decompress(impact_header_buffer, postings_list + impact_header_info_size, the_quantum_count * 3);

						postings_list = postings_list + beginning_of_the_postings;

						// print thcde postings
//							max = process(&factory, the_quantum_count, impact_header_buffer, raw, postings_list + beginning_of_the_postings, ANT_min(leaf.local_document_frequency, global_trim), print_postings, one_postings_per_line);
						long long docid, max_docid, sum;
						ANT_compressable_integer *current, *end;
						ANT_compressable_integer *impact_value_ptr, *doc_count_ptr, *impact_offset_ptr;
						ANT_compressable_integer *impact_offset_start;

						max_docid = sum = 0;
						impact_value_ptr = impact_header_buffer;
						doc_count_ptr = impact_header_buffer + the_quantum_count;
						impact_offset_start = impact_offset_ptr = impact_header_buffer + the_quantum_count * 2;

						while (doc_count_ptr < impact_offset_start)
							{
							factory.decompress(raw, postings_list + *impact_offset_ptr, *doc_count_ptr);

							docid = -1;
							current = raw;
							end = raw + *doc_count_ptr;

							while (current < end && count < limits)
								{
								docid += *current++;

								if (docid < 0)
									return; // continue;

								++count;
								if (ant_version == ANT_V5) 
									{
// #ifdef FILENAME_INDEX
									static char filename[1024*1024];
									*outchannel << atire->get_document_filename(filename, docid) << ":";
// #endif
									}
								*outchannel << docid << ANT_channel::endl;
//									if (verbose)
//										if (one_postings_per_line)
//											printf("\n<%lld,%lld>", docid, (long long)*impact_value_ptr);
//										else
//											printf("<%lld,%lld>", docid, (long long)*impact_value_ptr);

								if (docid > max_docid)
									max_docid = docid;
								}

							sum += *doc_count_ptr;
							if (sum >= document_frequency)
								break;

							impact_value_ptr++;
							impact_offset_ptr++;
							doc_count_ptr++;
							}

#else
						factory.decompress(raw, postings_list, leaf.impacted_length);

//							max = process((ANT_compressable_integer *)raw, document_frequency, print_postings, one_postings_per_line);
						ANT_compressable_integer *current, *end;

						max = 0;
						current = raw;
						end = raw + document_frequency;

						while (current < end)
							{
							end += 2;
							docid = -1;
							tf = *current++;
							while (*current != 0 && count < limits)
								{
								docid += *current++;
								if (ant_version == ANT_V5) 
									{
// #ifdef FILENAME_INDEX
									static char filename[1024*1024];
									*outchannel << atire->get_document_filename(filename, docid) << ":";
// #endif
									}
								*outchannel << docid << ANT_channel::endl;
//									if (verbose)
//										if (one_postings_per_line)
//											printf("\n<%lld,%lld>", docid, (long long)tf);
//										else
//											printf("<%lld,%lld>", docid, (long long)tf);
								}
							if (docid > max)
								max = docid;
							current++;						// zero
							}
#endif
						}
					}
//					if (*term != '~')		// ~length and others aren't encoded in the usual way
//						{
//						if (leaf.local_document_frequency > 2)
//							if (leaf.postings_length > postings_list_size)
//								{
//								postings_list_size = 2 * leaf.postings_length;
//								postings_list = (unsigned char *)realloc(postings_list, (size_t)postings_list_size);
//								}
//						postings_list = atire->get_search_engine()->get_postings(&leaf, postings_list);
//						if (leaf.impacted_length > raw_list_size)
//							{
//							raw_list_size = 2 * leaf.impacted_length;
//							raw = (ANT_compressable_integer *)realloc(raw, (size_t)raw_list_size);
//							}
//						factory.decompress(raw, postings_list, leaf.impacted_length);
//
//						current = raw;
//						tf = *current++;
//						docid += *current++;
//
//#ifdef FILENAME_INDEX
//						static char filename[1024*1024];
//						*outchannel << atire->get_document_filename(filename, docid) << ":";
//#endif
//						*outchannel << docid << ANT_channel::endl;
//						}
				}
			}

		*outchannel << "</ATIREresult>" << ANT_channel::endl;
//			outchannel->puts("\n");
		free(postings_list_mem);
		free(raw);
#ifdef IMPACT_HEADER
		free(impact_header_buffer);
#endif
		// delete [] command;
		return; // continue;
		}
	else if (strncmp(command, ".normalizequery ", 16) == 0)
		{
		topic_id = -1;
		char *start = command + 16;
		size_t buffer_length = strlen(start) * 2 + 1;
		char *buffer = new char[buffer_length];
		size_t normalized_string_length = 0;
		int result = ANT_UNICODE_normalize_string_tolowercase(buffer, buffer_length, &normalized_string_length, start);
		if (result)
			{
			// delete [] command;
			query = command = buffer;
			}
		else
			{
			delete [] buffer;
			query = start;
			}
		}
	else if (strncmp(command, "<ATIREsearch>", 13) == 0)
		{
		topic_id = -1;
		if ((query = between(command, "<query>", "</query>")) == NULL)
			{
			// delete [] command;
			return; // continue;
			}

		if ((pos = strstr(command, "<top>")) != NULL)
			first_to_list = atol(pos + 5)  - 1;
		else
			first_to_list = 0;

		if ((pos = strstr(command, "<n>")) != NULL)
			last_to_list = first_to_list + atol(pos + 3);
		else
			last_to_list = first_to_list + params->results_list_length;

		if ((ranker = between(command, "<ranking>", "</ranking>")) != NULL)
			{
			if (params_rank_ptr->set_ranker(ranker) && ant_init_ranking())
				custom_ranking = 1;
			else
				{
				outchannel->puts("<ATIREsearch>");
				outchannel->puts("<error>Bad ranking function</error>");
				outchannel->puts("</ATIREsearch>");
				delete [] query;
				delete [] ranker;
				// delete [] command;

				return; // continue;
				}

			delete [] ranker;
			}

		// delete [] command;
		command = query;
		}
	else if (strncmp(command, "<ATIREgetdoc>", 13) == 0)
		{
		*document_buffer = '\0';
		if ((current_document_length = length_of_longest_document) != 0)
			{
			atire->get_document(document_buffer, &current_document_length, atoll(strstr(command, "<docid>") + 7));
			outchannel->puts("<ATIREgetdoc>");
			*outchannel << "<length>" << current_document_length << "</length>" << ANT_channel::endl;
			outchannel->write(document_buffer, current_document_length);
			outchannel->puts("</ATIREgetdoc>");
			}
		else
			{
			outchannel->puts("<ATIREgetdoc>");
			outchannel->puts("<length>0</length>");
			outchannel->puts("</ATIREgetdoc>");
			}
		// delete [] command;
		return; // continue;
		}
	else if (params->assessments_filename != NULL || params->output_forum != ANT_ANT_param_block::NONE || params->queries_filename != NULL)
		{
		topic_id = atol(command);
		if ((query = strchr(command, ' ')) == NULL)
			exit(printf("Line %lld: Can't process query as badly formed:'%s'\n", line, command));
		}
	else
		{
		topic_id = -1;
		query = command;
		}

	search();
	result_to_outchannel();
	}
}

/*
	POLL()
	------
*/
void ATIRE_API_server::poll()
{
const char *cmd = inchannel->gets();
insert_command(cmd);
delete [] cmd;
}

/*
	INTERRUPT()
	-----------
*/
void ATIRE_API_server::interrupt()
{
interrupted = 1;
}

/*
	POLL_AND_PROCESS()
	------------------
*/
void ATIRE_API_server::poll_and_process()
{
poll();
if (command != NULL)
	process_command();
}

/*
	STOP_QUERY()
	------------
*/
char *ATIRE_API_server::stop_query(char *query, long stop_type)
{
static const char *SEPERATORS = ",./;'[]!@#$%^&*()_+-=\\|<>?:{}\r\n\t \"`~";
char *ch, *copy;
std::ostringstream stopped_query;

copy = strnew(query);
strip_space_inplace(query);

for (ch = strtok(query, SEPERATORS); ch != NULL; ch = strtok(NULL, SEPERATORS))
	{
	if ((stop_type & ANT_ANT_param_block::STOPWORDS_NCBI | ANT_ANT_param_block::STOPWORDS_PUURULA) && stop_word_list->isstop(ch))
		continue;

	if ((stop_type & ANT_ANT_param_block::STOPWORDS_NUMBERS) && isdigit(*ch))
		continue;

	if ((stop_type & ANT_ANT_param_block::STOPWORDS_SHORT) &&strlen(ch) <= 2)
		continue;

	stopped_query << ' ' << ch;
	}

if (stopped_query.str().empty())
	strcpy(query, copy);
else
	{
	stopped_query << std::ends;
	strcpy(query, (char *)stopped_query.str().c_str());
	}

return query;
}

/*
	START_STATS()
	-------------
*/
void ATIRE_API_server::start_stats()
{
if (stats)
	delete stats;
stats = new ANT_stats();
}

/*
	END_STATS()
	-----------
*/
void ATIRE_API_server::end_stats()
{
if (stats) 
	{
	printf("Total elapsed time including startup and shutdown ");
	stats->print_elapsed_time();
	ANT_stats::print_operating_system_process_time();
	}
}

/*
	GET_STATS()
	-----------
*/
ANT_stats* ATIRE_API_server::get_stats()
{
return stats;
}


/*
	INITIALIZE()
	------------
*/
void ATIRE_API_server::initialize()
{

if (!params_ptr)
	set_params(0, NULL);

atire = init();

if (atire)
	ant_init_ranking(); //Error value ignored...
}

/*
	PERFORM_QUERY()
	---------------
*/
double *ATIRE_API_server::perform_query(long topic_id, ANT_channel *outchannel, ANT_ANT_param_block *params, char *query, long long *matching_documents)
{
ANT_stats_time stats;
long long now;
long valid_to_evaluate;
double *evaluations;

if (params->query_stopping != ANT_ANT_param_block::NONE)
	stop_query(query, params->query_stopping);

/*
	Search
*/
now = stats.start_timer();
*matching_documents = atire->search(query, params->sort_top_k, params->query_type);
search_time = stats.time_to_milliseconds(stats.stop_timer(now));

if (params->stats & ANT_ANT_param_block::QUERY)
	atire->stats_text_render();

/*
	Return average precision
*/
if (params->evaluator)
	{
	evaluations = params->evaluator->perform_evaluation(atire->get_search_engine(), topic_id, &valid_to_evaluate);
	if (evaluations == NULL || !valid_to_evaluate)
		return NULL;

	return evaluations;
	}

return NULL;
}

void ATIRE_API_server::set_params(char *options)
{
static char *seperators = "+ ";
char **argv/*, **arg_list*/;
char *token;
size_t total_length = (options ? strlen(options) : 0) + 7;
char *copy/*, *copy_start*/;

copy = options_copy = new char[total_length];
memset(copy, 0, sizeof(*copy) * total_length);

memcpy(copy, "atire+", 6);
copy += 6;
if (options)
	{
	memcpy(copy, options, strlen(options));
	copy += strlen(options);
	}
*copy = '\0';

argv = arg_list = new char *[total_length * sizeof(char *)];
token = strtok(options_copy, seperators);

#ifdef DEBUG
	fprintf(stderr, "Start atire with options: %s\n", options);
#endif
for (; token != NULL; token = strtok(NULL, seperators))
	{
#ifdef DEBUG
	fprintf(stderr, "%s ", token);
#endif
	*argv++ = token;
	++argc;
	}
#ifdef DEBUG
	fprintf(stderr, "\n");
#endif
*argv = NULL;
//delete [] copy_start;

set_params(argc, arg_list);
}

void ATIRE_API_server::set_params(int argc, char* argv[])
{
params_ptr = new ANT_ANT_param_block(argc, argv);
ANT_ANT_param_block& params = *params_ptr;

params.parse();
if (params.query_stopping & ANT_ANT_param_block::STOPWORDS_NCBI)
	stop_word_list = new ANT_stop_word(ANT_stop_word::NCBI);
else if (params.query_stopping & ANT_ANT_param_block::STOPWORDS_PUURULA)
	stop_word_list = new ANT_stop_word(ANT_stop_word::PUURULA);
if (params.query_stopping & ANT_ANT_param_block::STOPWORDS_ATIRE)
	stop_word_list->addstop((const char **)new_stop_words);

}

char* ATIRE_API_server::version()
{
return atire->version();
}

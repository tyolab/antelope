/*
	ATIRE.C
	-------
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

using namespace std;

const char * const PROMPT = "]";		// tribute to Apple
const long MAX_TITLE_LENGTH = 1024;

ATIRE_API_server server;
ATIRE_API *atire = NULL;

ATIRE_API *ant_init(ANT_ANT_param_block & params);
long ant_init_ranking(ATIRE_API *atire, ANT_ANT_param_block &params);
extern int run_atire(int argc, char *argv[]);
extern int run_atire(char *files);

ANT_stop_word *stop_word_list = NULL;



/*
	MAIN()
	------
*/
int main(int argc, char *argv[])
{
return run_atire(argc, argv);
}

#endif

/*
	INDEXER.C
	---------
*/

#include "indexer.h"
#include "index_document.h"
#include "indexer_param_block.h"
#include "index_document_topsig.h"
#include "index_document.h"
#include "index_document_topsig.h"
#include "ranking_function_factory.h"
#include "directory_iterator_tar.h"
#include "directory_iterator_warc.h"
#include "directory_iterator_warc_gz_recursive.h"
#include "directory_iterator_recursive.h"
#include "directory_iterator_multiple.h"
#include "directory_iterator_compressor.h"
#include "directory_iterator_deflate.h"
#include "directory_iterator_file.h"
#include "directory_iterator_file_buffered.h"
#include "directory_iterator_csv.h"
#include "directory_iterator_tsv.h"
#include "directory_iterator_object.h"
#include "directory_iterator_pkzip.h"
#include "directory_iterator_preindex.h"
#include "directory_iterator_mysql.h"
#include "directory_iterator_filter.h"
#include "directory_iterator_filter_spam.h"
#include "directory_iterator_mime_filter.h"
#include "directory_iterator_scrub.h"
#include "file.h"
#include "parser.h"
#include "parser_readability.h"
#include "readability_factory.h"
#include "memory.h"
#include "memory_index.h"
#include "memory_index_one.h"
#include "indexer_param_block.h"
#include "stats_memory_index.h"
#include "stats_time.h"
#include "version.h"
#include "instream_file.h"
#include "instream_deflate.h"
#include "instream_bz2.h"
#include "instream_buffer.h"
#include "instream_lzo.h"
#include "instream_scrub.h"
#include "stem.h"
#include "stemmer_factory.h"
#include "btree_iterator.h"
#include "unicode.h"
#include "pregens_writer.h"
#include "parser.h"
#include "parser_readability.h"
#include "readability_factory.h"
#include "memory.h"
#include "memory_index.h"
#include "memory_index_one.h"
#include "pregens_writer.h"
#include "directory_iterator_object.h"
#include "stem.h"
#include "stemmer_factory.h"
#include "compression_text_factory.h"


#ifdef _MSC_VER
	#include <windows.h>
	#define PATH_MAX MAX_PATH
#else
	#include <limits.h>
#endif

const char *ATIRE_indexer::EMPTY_DOCUMENT_CONTENT = "<error>EMPTYDOCUMENT</error>"; // [30]; //
const char *ATIRE_indexer::EMPTY_DOCUMENT_FILENAME = "EMPTY DOCUMEN TTITLE"; // [30]; //

const int  ATIRE_indexer::EMPTY_DOUCMENT_LENGTH = strlen(EMPTY_DOCUMENT_CONTENT); // 28; //

#if (defined(ANDROID) || defined(__ANDROID__))
	// NOTHING
#else
	// See the issue #6 on github
	// This might cause a program that includes the "-lantelope" library to crash

	// Only use the following code for indexer intialisation in where such as "index.c" 
	// after an indexer instance is actually created
	// static bool indexer_static_initialzed = ATIRE_indexer::initialize();

	// this part of code will be run implicitly even this is no indexer instance created
	// subsequently a crash could happen with empty pointer used for an instance initialisation 
#endif

/*
	ATIRE_INDEXER::ATIRE_INDEXER()
	------------------------------
*/
ATIRE_indexer::ATIRE_indexer()
{
pregen = NULL;

//#ifndef PARALLEL_INDEXING_DOCUMENTS
stemmer = NULL;
//#endif

parallel_indexing = 0;
docno = -1;
param_block = NULL;
input_files = NULL;
}

ATIRE_indexer::~ATIRE_indexer()
{
	cleanup();
}

/*
	ATIRE_INDEXER::USAGE()
 */
void ATIRE_indexer::usage() {
	ANT_indexer_param_block param_block;
	param_block.usage();
}

void ATIRE_indexer::cleanup() {
	if (memory_index) {
		delete memory_index;
		memory_index = NULL;
	}

	if (parser) {
		delete parser;
		parser = NULL;
	}

	if (readability) {
		delete readability;
		readability = NULL;
	}

	if (document_indexer) {
		delete document_indexer;
		document_indexer = NULL;
	}

	if (pregen) {
		delete pregen;
		pregen = NULL;
	}

	#ifndef PARALLEL_INDEXING_DOCUMENTS
		if (stemmer) {
			delete stemmer;
			stemmer = NULL;
		}
	#endif

	if (factory_text) {
		delete factory_text;
		factory_text = NULL;
	}

	if (param_block) {
		delete param_block;
		param_block = NULL;
	}
}

/*
	ATIRE_INDEXER::INITIALIZE()
	---------------------------
*/
bool ATIRE_indexer::initialize()
{
//	strcpy(EMPTY_DOCUMENT_CONTENT, "<ERROR>EMPTYDOCUMENT</ERROR>");
//	strcpy(EMPTY_DOCUMENT_FILENAME, "EMPTY DOCUMEN TTITLE");
}

/*

 */
void ATIRE_indexer::init(char *options)
{
static char *seperators = "+";
char **argv, **file_list;
char *token;
size_t total_length = (options ? strlen(options) : 0) + 7;
char *copy, *copy_start;

copy = copy_start = new char[total_length];

memset(copy, 0, sizeof(*copy) * total_length);

memcpy(copy, "index+", 6);
copy += 6;
if (options) {
	memcpy(copy, options, strlen(options));
	copy += strlen(options);
}
*copy = '\0';

argv = file_list = new char *[total_length];
int argc = 0;
token = strtok(copy_start, seperators);

#ifdef DEBUG
	fprintf(stderr, "Start indexing with options: %s\n", options);
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

init(argc, file_list);

delete [] copy_start;
delete [] file_list;
}

void ATIRE_indexer::init(int argc, char *argv[])
{
this->param_block = new ANT_indexer_param_block(argc, argv);
long first_param = this->param_block->parse();

if (first_param >= argc)
	exit(0);	// no files to index so terminate
input_files = argv + first_param;
input_files_count = argc - first_param;

init(*param_block);
}

void ATIRE_indexer::init(ANT_indexer_param_block& param_block)
{
docno = 0;
segmentation = param_block.segmentation;
document_compression_scheme = param_block.document_compression_scheme;

memory_index = new ANT_memory_index(param_block.index_filename);
#ifndef FILENAME_INDEX
	id_list.open(param_block.doclist_filename, "wbx");
#endif
memory_index->set_compression_scheme(param_block.compression_scheme);
memory_index->set_compression_validation(param_block.compression_validation);
memory_index->set_static_pruning(param_block.static_prune_point);
memory_index->set_term_culling(param_block.stop_word_removal, param_block.stop_word_df_threshold, param_block.stop_word_df_frequencies);
memory_index->set_quantization(param_block.quantization, param_block.quantization_automatic, param_block.quantization_bits);
memory_index->set_ranking_function(param_block.ranking_function, param_block.p1, param_block.p2, param_block.p3);
memory_index->set_inverted_index_mode(param_block.inversion_extras, param_block.puurula_length_g);

if (param_block.readability_measure == ANT_readability_factory::NONE
		|| param_block.readability_measure == ANT_readability_factory::TAG_WEIGHTING)
	parser = new ANT_parser(param_block.segmentation);
else
	parser = new ANT_parser_readability();

if (param_block.inversion_type == ANT_indexer_param_block::TOPSIG)
	document_indexer = new ANT_index_document_topsig(param_block.stop_word_removal, param_block.topsig_width, param_block.topsig_density, param_block.topsig_global_stats);
else
	document_indexer = new ANT_index_document(param_block.stop_word_removal);

if (param_block.stemmer != 0)
	{
	/*
		The user has asked for a stemmed index and so we create a stemmer
		and store the fact in the index (so that the search engine knows)
	*/
	ANT_string_pair squiggle_stemmed("~stemmer");
	memory_index->set_variable(&squiggle_stemmed, param_block.stemmer);
#ifndef PARALLEL_INDEXING_DOCUMENTS
	stemmer = ANT_stemmer_factory::get_core_stemmer(param_block.stemmer);
#endif
	}

readability = new ANT_readability_factory;
readability->set_measure(param_block.readability_measure);
readability->set_parser(parser);

if (param_block.document_compression_scheme != ANT_indexer_param_block::NONE)
	{
	factory_text = new ANT_compression_text_factory;
	factory_text->set_scheme(param_block.document_compression_scheme);
	}
else
	factory_text = NULL;

char pregen_filename[PATH_MAX + 1];

if (param_block.num_pregen_fields)
	{
	size_t pregen_prefix_len = strlen(param_block.index_filename) + 1;
	pregen = new ANT_pregens_writer();

	if (pregen_prefix_len < PATH_MAX)
		{
		strcpy(pregen_filename, param_block.index_filename);

		pregen_filename[pregen_prefix_len - 1] = '.';

		for (int i = 0; i < param_block.num_pregen_fields; i++)
			{
			ANT_indexer_param_block::pregen_field_spec & spec = param_block.pregens[i];

			if (pregen_prefix_len + strlen(spec.field_name) > PATH_MAX)
				fprintf(stderr, "Pathname too long to for pregen field '%s'\n", spec.field_name);
			else
				{
				strcpy(pregen_filename + pregen_prefix_len, spec.field_name);
				pregen->add_field(pregen_filename, spec.field_name, spec.type);
				}
			}
		}
	else
		{
		fprintf(stderr, "Index pathname too long to derive pregen filenames\n");
		//And we'll just keep going...
		}
	}
}

long long ATIRE_indexer::index_document(ANT_directory_iterator_object *current_file, char *doc_to_store)
{
long terms_in_document;
long skip_document = 0;
char *filename = current_file->filename;
char *file = current_file->file;
long long length = current_file->length;

docno++;

readability->set_current_file(current_file);

#ifdef PARALLEL_INDEXING_DOCUMENTS
	memory_index->add_indexed_document(current_file->index, docno);
	delete current_file->index;
	terms_in_document = current_file->terms;
#else
	terms_in_document = document_indexer->index_document(memory_index, stemmer, segmentation, readability, docno, file);
#endif

if (terms_in_document == 0)
	{
//			puts(current_file->filename);
	/*
		pretend we never saw the document
	*/
#ifndef WITH_EMPTY_DOCUMENT
	skip_document = 1;
	docno--;
#else

	/*
	 * actually even terms_in_document is zero, we should still count it because that we could depend on it for the unique document id
	 *
	 */
	length = EMPTY_DOUCMENT_LENGTH;
	filename = (char *)EMPTY_DOCUMENT_FILENAME;
	file = new char[length + 1];
	strncpy(file, EMPTY_DOCUMENT_CONTENT, length);
	file[length] = '\0';
	terms_in_document = document_indexer->index_document(memory_index, stemmer, segmentation, readability, docno, file);
	delete file;
#endif
	}


if (!skip_document)
	{
	if (pregen)
		pregen->process_document(docno - 1, filename);

	/*
		Store the document in the repository.
	*/
	if (document_compression_scheme != ANT_indexer_param_block::NONE)
		{
		if (doc_to_store)
			{
			file = doc_to_store;
			length = strlen(file);
			}

		/*
		   if parallel indexing macro is set and "-C" option is set too, text will be compressed when being retrieved
		   so we don't need to compress it again, and the following code is for the situation where parallel indexing is not set
		 */
		if (!parallel_indexing)
			{
//#ifndef PARALLEL_INDEXING
			unsigned long compressed_size;

			/*
			* the following code is copied from from the directory_iterator_compressor
			* it may be better to have it refactored to include a compressor in the base class (i.e. ANT_directory_iterator)
			*/
			current_file->compressed = new (std::nothrow) char [(size_t)(compressed_size = factory_text->space_needed_to_compress((unsigned long)length + 1))];		// +1 to include the '\0'
			if (factory_text->compress(current_file->compressed, &compressed_size, file, (unsigned long)(length + 1)) == NULL)
				exit(printf("Cannot compress document (name:%s)\n", filename));
			current_file->compressed_length = compressed_size;
			}
//#endif
		memory_index->add_to_document_repository(strip_space_inplace(filename), current_file->compressed, (long)current_file->compressed_length, (long)length);
		if (current_file->compressed)
			delete [] current_file->compressed;
		}
#ifdef FILENAME_INDEX
	else
		memory_index->add_to_document_repository(strip_space_inplace(filename));
#else
	//			puts(filename);
	id_list.puts(strip_space_inplace(filename));
#endif
	}
return docno;
}

void ATIRE_indexer::index_document(char *file_name, char *file, char *doc_to_store)
{
ANT_directory_iterator_object current_file;
current_file.file = file;
current_file.filename = file_name;
current_file.length = strlen(file);

index_document(&current_file, doc_to_store);
}

long ATIRE_indexer::finish()
{	
if (docno <= 0)
	return 0;

#ifndef FILENAME_INDEX
	id_list.close();
#endif
	if (pregen)
		{
		for (int i = 0; i < pregen->field_count; i++)
			if (pregen->fields[i]->doc_count == 0)
				fprintf(stderr, "Warning: Pregen field '%s' not found in any documents\n", pregen->fields[i]->field_name);
		pregen->close();
		}

	// write the index information into file, and store the serialisation status
	long ret = memory_index->serialise();

	// clean up, release resources
	cleanup();

	return ret;
}

/*
	REPORT()
	--------
*/
void report(long long doc, ANT_memory_indexer *index, ANT_stats_time *stats, long long bytes_indexed)
{
printf("%lld Documents (%lld bytes) in %lld bytes of memory in ", (long long)doc, (long long)bytes_indexed, (long long)index->get_memory_usage());
stats->print_elapsed_time();
}

/*
	ATIRE_INDEXER::INDEX()
	----------------------
*/
void ATIRE_indexer::index()
{
index(*param_block);
}

/*
	ATIRE_INDEXER::INDEX()
	----------------------
*/
void ATIRE_indexer::index(ANT_indexer_param_block& param_block)
{
if (!input_files) {
	puts("No files to index");
	exit(-1);
}

ANT_stats_time stats;
ANT_directory_iterator *source = NULL;
ANT_directory_iterator *disk = NULL;

long long doc, now, last_report;
long param;
ANT_memory file_buffer(1024 * 1024);

long long files_that_match;
long long bytes_indexed;
ANT_instream *file_stream = NULL, *decompressor = NULL, *instream_buffer = NULL, *scrubber = NULL;
ANT_directory_iterator *dir_scrubber = NULL;
ANT_directory_iterator_object file_object, *current_file;
//#ifdef PARALLEL_INDEXING
	ANT_directory_iterator_multiple *parallel_disk = NULL;
//#endif

if (param_block.logo)
	puts(ANT_version_string);				// print the version string is we parsed the parameters OK

doc = 0;
last_report = 0;

ANT_compression_text_factory *factory_text;
factory_text = get_compression_text_factory();

// #ifdef PARALLEL_INDEXING
if (param_block.parallel_indexing)
	{
	parallel_disk = new ANT_directory_iterator_multiple;
	enable_parallel_indexing();
	}
// #endif

/*
	The first parameter that is not a command line switch is the start of the list of files to index
*/
bytes_indexed = 0;

for (param = 0; param < input_files_count; param++)
	{
// #ifdef PARALLEL_INDEXING
// #else
	if (!param_block.parallel_indexing)
		{
		delete disk;
		delete file_stream;
		delete decompressor;
		delete instream_buffer;
		}
// #endif

	now = stats.start_timer();
	if (param_block.recursive == ANT_indexer_param_block::DIRECTORIES)
		{
		source = new ANT_directory_iterator_recursive(input_files[param], ANT_directory_iterator::READ_FILE);			// this dir and below
		if (strcmp(input_files[param] + strlen(input_files[param]) - 3, ".gz") == 0)
			source = new ANT_directory_iterator_deflate(source, ANT_directory_iterator_deflate::TEXT);			// recursive .gz files
		}
	else if (param_block.recursive == ANT_indexer_param_block::TAR)
		{
		file_stream = new ANT_instream_file(&file_buffer, input_files[param]);
		instream_buffer = new ANT_instream_buffer(&file_buffer, file_stream);
		source = new ANT_directory_iterator_tar(instream_buffer, ANT_directory_iterator::READ_FILE, ANT_directory_iterator_tar::NAME);
		}
	else if (param_block.recursive == ANT_indexer_param_block::TAR_BZ2)
		{
		file_stream = new ANT_instream_file(&file_buffer, input_files[param]);
		decompressor = new ANT_instream_bz2(&file_buffer, file_stream);
		instream_buffer = new ANT_instream_buffer(&file_buffer, decompressor);
		source = new ANT_directory_iterator_tar(instream_buffer, ANT_directory_iterator::READ_FILE, ANT_directory_iterator_tar::NAME);
		}
	else if (param_block.recursive == ANT_indexer_param_block::TAR_GZ)
		{
		file_stream = new ANT_instream_file(&file_buffer, input_files[param]);
		decompressor = new ANT_instream_deflate(&file_buffer, file_stream);
		instream_buffer = new ANT_instream_buffer(&file_buffer, decompressor);
		source = new ANT_directory_iterator_tar(instream_buffer, ANT_directory_iterator::READ_FILE, ANT_directory_iterator_tar::NAME);
		}
	else if (param_block.recursive == ANT_indexer_param_block::TAR_LZO)
		{
		file_stream = new ANT_instream_file(&file_buffer, input_files[param]);
		decompressor = new ANT_instream_lzo(&file_buffer, file_stream);
		instream_buffer = new ANT_instream_buffer(&file_buffer, decompressor);
		source = new ANT_directory_iterator_tar(instream_buffer, ANT_directory_iterator::READ_FILE, ANT_directory_iterator_tar::NAME);
		}
	else if (param_block.recursive == ANT_indexer_param_block::WARC_GZ)
		{
		file_stream = new ANT_instream_file(&file_buffer, input_files[param]);
		decompressor = new ANT_instream_deflate(&file_buffer, file_stream);
		instream_buffer = new ANT_instream_buffer(&file_buffer, decompressor);
		source = new ANT_directory_iterator_warc(instream_buffer, ANT_directory_iterator::READ_FILE);
		}
	else if (param_block.recursive == ANT_indexer_param_block::RECURSIVE_WARC_GZ)
		source = new ANT_directory_iterator_warc_gz_recursive(input_files[param], ANT_directory_iterator::READ_FILE, param_block.scrubbing);

#ifdef ANT_HAS_MYSQL
	else if (param_block.recursive == ANT_indexer_param_block::VBULLETIN)
		{
		source = new ANT_directory_iterator_mysql(input_files[param + 2], input_files[param], input_files[param + 1], input_files[param + 3], "select postid, username, title, pagetext from post", ANT_directory_iterator::READ_FILE);
		param += 3;
		}
	else if (param_block.recursive == ANT_indexer_param_block::PHPBB)
		{
		/* We replace special term markers in user text (Unicode character 80, which encodes to C2 80 in UTF-8)
		 * with the euro symbol (encoded as E2 82 AC in UTF-8)
		 */
		assert(SPECIAL_TERM_CHAR == 0x80);

		if (strcmp(input_files[param+4], "topics")==0)
			source = new ANT_directory_iterator_mysql(input_files[param + 2], input_files[param], input_files[param + 1], input_files[param + 3],
					"SELECT topic_id, CONCAT('<id>', t.topic_id, '</id><last_post_time>', topic_last_post_time, '</last_post_time><title>', "
							"REPLACE(topic_title, '\xC2\x80', '\xE2\x82\xAC'), '</title><author>', t.topic_first_poster_name, '</author><forum>',"
							"forum_name ,'</forum>'), "
						"CONCAT('\xC2\x80""forum-', t.forum_id), IF(t.topic_approved=0, '\xC2\x80""u', '\xC2\x80""a'), "
						"GROUP_CONCAT("
							"REPLACE(CONCAT_WS(' ', p.post_subject, p.post_text), '\xC2\x80', '\xE2\x82\xAC'), "
							"CONCAT('\xC2\x80poster-', p.poster_id) "
							"SEPARATOR ' '"
						") "
					"FROM phpbb_topics t "
					"LEFT JOIN phpbb_posts p ON t.topic_id = p.topic_id "
					"LEFT JOIN phpbb_forums f ON t.forum_id = f.forum_id "
					"WHERE p.post_approved = 1 AND t.topic_id > $start "
					"GROUP BY t.topic_id "
					"ORDER BY t.topic_id ASC ",
					ANT_directory_iterator::READ_FILE);
		else
			source = new ANT_directory_iterator_mysql(input_files[param + 2], input_files[param], input_files[param + 1], input_files[param + 3],
					"SELECT post_id, CONCAT('<id>', post_id, '</id><time>', post_time, '</time><subject>', REPLACE(post_subject, '\xC2\x80', '\xE2\x82\xAC'), "
							"'</subject><title>', REPLACE(topic_title, '\xC2\x80', '\xE2\x82\xAC'), '</title><author>', u.username, '</author><forum>',"
							"forum_name ,'</forum>'), "
						"REPLACE(post_subject, '\xC2\x80', '\xE2\x82\xAC'), "
						"REPLACE(post_text, '\xC2\x80', '\xE2\x82\xAC'), "
						"IF(post_approved=0, '\xC2\x80""u', '\xC2\x80""a'), CONCAT('\xC2\x80poster-', poster_id), "
						"CONCAT('\xC2\x80""forum-', p.forum_id), CONCAT('\xC2\x80""topic-', topic_id) "
					"FROM phpbb_posts p "
					"LEFT JOIN phpbb_forums f USING (forum_id) "
					"LEFT JOIN phpbb_topics t USING (topic_id) "
					"LEFT JOIN phpbb_users u ON p.poster_id = u.user_id "
					"WHERE p.post_id > $start "
					"ORDER BY post_id ASC ",
					ANT_directory_iterator::READ_FILE);

		static_cast<ANT_directory_iterator_mysql*>(source)->set_paging_mode(FETCH_RANGE_START_LIMIT);

		param += 4;
		}
	else if (param_block.recursive == ANT_indexer_param_block::MYSQL)
		{
		source = new ANT_directory_iterator_mysql(input_files[param + 2], input_files[param], input_files[param + 1], input_files[param + 3], input_files[param + 4], ANT_directory_iterator::READ_FILE);
		param += 4;
		}
#else
	else if (param_block.recursive == ANT_indexer_param_block::VBULLETIN
			|| param_block.recursive == ANT_indexer_param_block::PHPBB
			|| param_block.recursive == ANT_indexer_param_block::MYSQL)
		{
		fprintf(stderr, "You tried to index documents from MySQL but this indexer was not built with MySQL support\n");
		exit(-1);
		}
#endif

	else if (param_block.recursive == ANT_indexer_param_block::TREC)
		{
		file_stream = new ANT_instream_file(&file_buffer, input_files[param]);
		if (param_block.scrubbing)
			scrubber = new ANT_instream_scrub(&file_buffer, file_stream, param_block.scrubbing);
		else
			scrubber = file_stream;
		ANT_directory_iterator_file_buffered *buffered_file_iterator = new ANT_directory_iterator_file_buffered(scrubber, ANT_directory_iterator::READ_FILE);
		if (param_block.doc_tag != NULL && param_block.docno_tag != NULL)
			buffered_file_iterator->set_tags(param_block.doc_tag, param_block.docno_tag);
		source = buffered_file_iterator;
//		source = new ANT_directory_iterator_file(ANT_disk::read_entire_file(input_files[param]), ANT_directory_iterator::READ_FILE);
		}
	else if (param_block.recursive == ANT_indexer_param_block::RECURSIVE_TREC)
		{
		source = new ANT_directory_iterator_recursive(input_files[param], ANT_directory_iterator::READ_FILE);
		if (strcmp(input_files[param] + strlen(input_files[param]) - 3, ".gz") == 0)
			source = new ANT_directory_iterator_deflate(source);		// recursive .gz files
		if (param_block.scrubbing)
			dir_scrubber = new ANT_directory_iterator_scrub(source, param_block.scrubbing, ANT_directory_iterator::READ_FILE);
		source = new ANT_directory_iterator_file(dir_scrubber == NULL ? source : dir_scrubber, ANT_directory_iterator::READ_FILE);
		}
	else if (param_block.recursive == ANT_indexer_param_block::CSV)
		source = new ANT_directory_iterator_csv(ANT_disk::read_entire_file(input_files[param]), ANT_directory_iterator::READ_FILE);
	else if (param_block.recursive == ANT_indexer_param_block::TSV)
		{
		file_stream = new ANT_instream_file(&file_buffer, input_files[param]);
		decompressor = new ANT_instream_deflate(&file_buffer, file_stream);
		scrubber = new ANT_instream_scrub(&file_buffer, decompressor, param_block.scrubbing);
		instream_buffer = new ANT_instream_buffer(&file_buffer, scrubber);
		source = new ANT_directory_iterator_tsv(instream_buffer, ANT_directory_iterator::READ_FILE);
		}
	else if (param_block.recursive == ANT_indexer_param_block::PKZIP)
		source = new ANT_directory_iterator_pkzip(input_files[param], ANT_directory_iterator::READ_FILE);
	else
		{
		if (ANT_disk::is_directory(input_files[param]))
			source = new ANT_directory_iterator_recursive(input_files[param], ANT_directory_iterator::READ_FILE);
		else
			source = new ANT_directory_iterator(input_files[param], ANT_directory_iterator::READ_FILE);					// current directory
		}

	if (param_block.filter_filename != NULL)
		source = new ANT_directory_iterator_filter(source, param_block.filter_filename, param_block.filter_method, ANT_directory_iterator_filter::READ_FILE);

	if (param_block.mime_filter)
		source = new ANT_directory_iterator_mime_filter(source, ANT_directory_iterator::READ_FILE);
	
	if (param_block.spam_filename != NULL)
		source = new ANT_directory_iterator_filter_spam(source, param_block.spam_filename, param_block.spam_threshold, ANT_directory_iterator::READ_FILE);
	
	/*
		We may already had to have created a directory iterator scrubber at some other point in the chain, here's looking at you RECURSIVE_TREC,
		so don't do it again unnecessarily
	*/
	if (param_block.scrubbing && dir_scrubber == NULL && scrubber == NULL)
		source = new ANT_directory_iterator_scrub(source, param_block.scrubbing, ANT_directory_iterator::READ_FILE);

	stats.add_disk_input_time(stats.stop_timer(now));

// #ifdef PARALLEL_INDEXING
	if (param_block.parallel_indexing)
		parallel_disk->add_iterator(source);
	}

if (param_block.parallel_indexing)
	{
	disk = parallel_disk;
	if (factory_text != NULL)
		disk = new ANT_directory_iterator_compressor(disk, 8, factory_text, ANT_directory_iterator::READ_FILE);

	#ifdef PARALLEL_INDEXING_DOCUMENTS
		disk = new ANT_directory_iterator_preindex(disk, param_block.segmentation, param_block.readability_measure, param_block.stemmer, get_document_indexer(), get_index(), 8, ANT_directory_iterator::READ_FILE);
	#endif

	files_that_match = 0;

	now = stats.start_timer();
	current_file = disk->first(&file_object);
	stats.add_disk_input_time(stats.stop_timer(now));
	}

	{
// #else
	if (!param_block.parallel_indexing)
		{
		disk = source;
		files_that_match = 0;

		now = stats.start_timer();
		current_file = disk->first(&file_object);
		stats.add_disk_input_time(stats.stop_timer(now));
		}
// #endif

	while (current_file != NULL)
		{
		if (current_file->file != NULL)
			{
			/*
				How much data do we have?
			*/
			files_that_match++;
			bytes_indexed += current_file->length;
	
			/*
				Index, this call returns the number of terms we found in the document
			*/
			now = stats.start_timer();
			//printf("INDEX:%s\n", current_file->filename);

			doc = index_document(current_file);
			stats.add_indexing_time(stats.stop_timer(now));

			/*
				Report
			*/
			if (doc % param_block.reporting_frequency == 0 && doc != last_report)
				report(last_report = doc, get_index(), &stats, bytes_indexed);

			delete [] current_file->file;
			delete [] current_file->filename;
			}
#ifdef WITH_EMPTY_DOCUMENT
		else
			{
				if (current_file->filename)
					delete [] current_file->filename;
				/*
				static const char *EMPTY_DOCUMENT_CONTENT = "<error>EMPTYDOCUMENT</error>";
				static const int  EMPTY_DOUCMENT_LENGTH = strlen(EMPTY_DOCUMENT_CONTENT);
				static const char *EMPTY_DOCUMENT_FILENAME = "EMPTY DOCUMEN TTITLE";
				*/
				files_that_match++;
				bytes_indexed += ATIRE_indexer::EMPTY_DOUCMENT_LENGTH;

				current_file->file = (char *)ATIRE_indexer::EMPTY_DOCUMENT_CONTENT;
				current_file->filename = (char *)ATIRE_indexer::EMPTY_DOCUMENT_FILENAME;
				doc = index_document(current_file);
			}
#endif

		/*
			Get the next file
		*/
		now = stats.start_timer();
		current_file = disk->next(&file_object);
		stats.add_disk_input_time(stats.stop_timer(now));
		}

#ifdef NEVER
	if (files_that_match == 0)
		printf("Warning: '%s' does not match any files\n", input_files[param]);
#endif
	}

if (param_block.reporting_frequency != LLONG_MAX && doc != last_report)
	report(doc, get_index(), &stats, bytes_indexed);

if (doc == 0)
	puts("No documents indexed (check your file list)");
else
	{
	now = stats.start_timer();
	stats.add_disk_output_time(stats.stop_timer(now));
	get_index()->text_render(param_block.statistics);
	}

if (param_block.statistics & ANT_indexer_param_block::STAT_TIME)
	{
	printf("\nTIMINGS\n-------\n");
	stats.text_render();
	}

/**
 * Wail until stats gets all the information first then do the memory clean-up
 */
if (doc > 0) {
	finish();
	doc = 0;
}

delete disk;
delete file_stream;
delete decompressor;
delete instream_buffer;

}
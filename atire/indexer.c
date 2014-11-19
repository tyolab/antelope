/*
	INDEXER.C
	---------
*/

#include "indexer.h"
#include "index_document.h"
#include "indexer_param_block.h"
#include "index_document_topsig.h"
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

ANT_indexer::ANT_indexer()
{
pregen = NULL;
}

ANT_indexer::~ANT_indexer()
{
delete index;
delete parser;
delete readability;
delete document_indexer;
delete pregen;

#ifndef PARALLEL_INDEXING_DOCUMENTS
	delete stemmer;
#endif

if (factory_text)
	delete factory_text;
}

void ANT_indexer::init(char *options)
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

void ANT_indexer::init(int argc, char *argv[])
{
ANT_indexer_param_block param_block(argc, argv);
param_block.parse();

init(param_block);
}

void ANT_indexer::init(ANT_indexer_param_block& param_block)
{
docno = 0;
segmentation = param_block.segmentation;
document_compression_scheme = param_block.document_compression_scheme;

index = new ANT_memory_index(param_block.index_filename);
#ifndef FILENAME_INDEX
	id_list.open(param_block.doclist_filename, "wbx");
#endif
index->set_compression_scheme(param_block.compression_scheme);
index->set_compression_validation(param_block.compression_validation);
index->set_static_pruning(param_block.static_prune_point);
index->set_term_culling(param_block.stop_word_removal, param_block.stop_word_df_threshold, param_block.stop_word_df_frequencies);
index->set_quantization(param_block.quantization, param_block.quantization_automatic, param_block.quantization_bits);
index->set_ranking_function(param_block.ranking_function, param_block.p1, param_block.p2, param_block.p3);
index->set_inverted_index_mode(param_block.inversion_extras, param_block.puurula_length_g);

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
	index->set_variable(&squiggle_stemmed, param_block.stemmer);
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

void ANT_indexer::index_document(ANT_directory_iterator_object *current_file, long long *doc)
{
long terms_in_document;

docno++;

readability->set_current_file(current_file);

#ifdef PARALLEL_INDEXING_DOCUMENTS
	index->add_indexed_document(current_file->index, docno);
	delete current_file->index;
	terms_in_document = current_file->terms;
#else
	terms_in_document = document_indexer->index_document(index, stemmer, segmentation, readability, docno, current_file->file);
#endif

/*
 * actually even terms_in_document is zero, we should still count it because that we cound depend on it for the unique document id
 *
 */

if (terms_in_document == 0)
	{
//			puts(current_file->filename);
	/*
		pretend we never saw the document
	*/
	docno--;
	}
else
	{
	if (pregen)
		pregen->process_document(docno - 1, current_file->filename);

	/*
		Store the document in the repository.
	*/
	if (document_compression_scheme != ANT_indexer_param_block::NONE)
		{
		/*
		   if parallel indexing macro is set and "-C" option is set too, text will be compressed when being retrieved
		   so we don't need to compress it again, and the following code is for the situation where parallel indexing is not set
		 */
#ifndef PARALLEL_INDEXING
	unsigned long compressed_size;

	/*
	 * the following code is copied from from the directory_iterator_compressor
	 * it may be better to have it refactored to include a compressor in the base class (i.e. ANT_directory_iterator)
	 */
	current_file->compressed = new (std::nothrow) char [(size_t)(compressed_size = factory_text->space_needed_to_compress((unsigned long)current_file->length + 1))];		// +1 to include the '\0'
	if (factory_text->compress(current_file->compressed, &compressed_size, current_file->file, (unsigned long)(current_file->length + 1)) == NULL)
		exit(printf("Cannot compress document (name:%s)\n", current_file->filename));
	current_file->compressed_length = compressed_size;
#endif
	index->add_to_document_repository(strip_space_inplace(current_file->filename), current_file->compressed, (long)current_file->compressed_length, (long)current_file->length);
	if (current_file->compressed)
		delete [] current_file->compressed;
	}
#ifdef FILENAME_INDEX
	else
			index->add_to_document_repository(strip_space_inplace(current_file->filename));
#else
	//			puts(current_file->filename);
	id_list.puts(strip_space_inplace(current_file->filename));
#endif
	}

	if (doc)
		(*doc) = docno;
}

void ANT_indexer::index_document(char *file_name, char *file)
{
ANT_directory_iterator_object current_file;
current_file.file = file;
current_file.filename = file_name;
current_file.length = strlen(file);

index_document(&current_file, NULL);
}

void ANT_indexer::finalize()
{
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

	index->serialise();
}

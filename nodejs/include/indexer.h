/*
	INDEXER.H
	---------
*/

#ifndef INDEXER_H_
#define INDEXER_H_

#include "file.h"

class ANT_memory_index;
class ANT_directory_iterator_object;
class ANT_readability_factory;
class ANT_parser;
class ANT_index_document;
class ANT_pregens_writer;
class ANT_indexer_param_block;
class ANT_compression_text_factory;
class ANT_indexer_param_block;

#ifndef PARALLEL_INDEXING_DOCUMENTS
class ANT_stem;
#endif

/*
	class ANT_INDEXER
	-----------------
*/
class ATIRE_indexer {
public:
	static const char *EMPTY_DOCUMENT_CONTENT;
	static const char *EMPTY_DOCUMENT_FILENAME;
	static const int  EMPTY_DOUCMENT_LENGTH;

private:
	long long 						docno;

	ANT_memory_index 				*memory_index;
#ifndef FILENAME_INDEX
	ANT_file id_list;
#endif
	ANT_index_document 				*document_indexer;
	ANT_parser *parser;
	ANT_readability_factory 		*readability;
//	ANT_indexer_param_block 		*param_block_ptr;
	ANT_compression_text_factory 	*factory_text;
	ANT_pregens_writer 				*pregen;
#ifndef PARALLEL_INDEXING_DOCUMENTS
	ANT_stem *stemmer;
#endif

	long 							segmentation;
	long 							document_compression_scheme;

	long 							parallel_indexing;

	ANT_indexer_param_block 		*param_block;
	long							first_param;
	char 							**input_files;
	long							input_files_count;

public:
	ATIRE_indexer();
	virtual ~ATIRE_indexer();

	void usage();

	void init(char *options);
	void init(int argc, char *argv[]);

	long finish();

	long long index_document(ANT_directory_iterator_object *current_file, char *doc_to_store = 0x0);
	void index_document(char *file_name, char *file, char *doc_to_store = 0x0);
	void index();

	void enable_parallel_indexing() { parallel_indexing = 1; }

	static bool initialize();

private:
	void init(ANT_indexer_param_block& param_block);
	void index(ANT_indexer_param_block& param_block);
	void cleanup();

	ANT_memory_index *get_index() { return memory_index; }
	ANT_compression_text_factory *get_compression_text_factory() { return factory_text; }
	ANT_index_document *get_document_indexer() { return document_indexer; }
};

#endif /* INDEXER_H_ */

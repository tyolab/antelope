/*
	TEST_MEMORY_ENGINE_OWNERSHIP.CPP
	--------------------------------
	NOTE ON INDEXING PATH: this build is compiled with PARALLEL_INDEXING_DOCUMENTS
	(GNUmakefile's default USE_PARALLEL_INDEXING := 1), which changes
	ATIRE_indexer::index_document(ANT_directory_iterator_object*, char*) to read a
	pre-built object->index (an ANT_memory_index_one*) instead of parsing the
	document itself.  The convenience overload
	ATIRE_indexer::index_document(char *file_name, char *file, char *doc_to_store)
	(atire/indexer.cpp ~line 506) constructs its ANT_directory_iterator_object on
	the stack WITHOUT populating ->index, so under this build configuration it
	dereferences an indeterminate pointer and segfaults -- confirmed with gdb
	(crash in ANT_memory_index_one_node::ANT_memory_index_one_node via
	add_indexed_document_node).  This is a pre-existing bug in that convenience
	wrapper, unrelated to the ownership hooks under test here, so this test
	drives the (working, public) object-based index_document() overload directly,
	performing the same "pre-index one document" step that
	ANT_directory_iterator_preindex::work_one() (source/directory_iterator_preindex.cpp)
	does for the parallel-indexing worker threads.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../atire/indexer.h"
#include "../atire/atire_api.h"
#include "memory_index.h"
#include "memory_index_one.h"
#include "memory.h"
#include "index_document.h"
#include "readability_factory.h"
#include "parser.h"
#include "directory_iterator_object.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

/*
	INDEX_ONE_DOCUMENT()
	--------------------
	Mirrors ANT_directory_iterator_preindex::work_one(): build a per-document
	ANT_memory_index_one, index the raw text into it, then hand it to the
	indexer's (public, object-based) index_document() which merges it into the
	live ANT_memory_index and appends to the doc_list.
*/
static void index_one_document(ATIRE_indexer *indexer, char *file_name, char *file)
{
ANT_directory_iterator_object current_file;
ANT_readability_factory readability;
ANT_parser parser(ANT_parser::SHOULD_SEGMENT);
ANT_index_document document_indexer(/*stop_word_removal=*/0);

current_file.filename = file_name;
current_file.file = file;
current_file.length = (long long)strlen(file);
current_file.compressed = NULL;
current_file.compressed_length = 0;

readability.set_measure(ANT_readability_factory::NONE);
readability.set_parser(&parser);
readability.set_current_file(&current_file);

current_file.index = new ANT_memory_index_one(new ANT_memory(1024 * 1024), indexer->get_index());
current_file.terms = (long)document_indexer.index_document(current_file.index, /*stemmer=*/NULL, ANT_parser::SHOULD_SEGMENT, &readability, /*doc_id=*/1, file);

indexer->index_document(&current_file, /*doc_to_store=*/(char *)NULL);
}

int main(void)
{
long long count;
char query[64];

ATIRE_indexer *indexer = new ATIRE_indexer();
indexer->init((char *)"atire_test -nologo -findex /tmp/unused_task5.aspt -fdoclist /tmp/unused_task5_doclist.aspt");
index_one_document(indexer, (char *)"doc-1", (char *)"aardvark zebra");
index_one_document(indexer, (char *)"doc-2", (char *)"zebra quokka");

/*
	First wrapper: non-owning.  get_index(), not release_index() -- the
	indexer keeps its pointer and can continue indexing afterwards.
*/
ATIRE_API *engine_one = new ATIRE_API();
CHECK(engine_one->open_from_memory_index(indexer->get_index(), indexer->get_doc_list(&count), count, /*take_ownership =*/ 0) == 0);
strcpy(query, "aardvark");
CHECK(engine_one->search(query, 10) == 1);
delete engine_one;			// must NOT free the memory index

/*
	Keep indexing into the same index, then wrap again: both old and new
	documents must be searchable.
*/
index_one_document(indexer, (char *)"doc-3", (char *)"aardvark wombat");
ATIRE_API *engine_two = new ATIRE_API();
CHECK(engine_two->open_from_memory_index(indexer->get_index(), indexer->get_doc_list(&count), count, 0) == 0);
strcpy(query, "aardvark");
CHECK(engine_two->search(query, 10) == 2);
strcpy(query, "wombat");
CHECK(engine_two->search(query, 10) == 1);
delete engine_two;

delete indexer;
printf("PASSED\n");
return 0;
}

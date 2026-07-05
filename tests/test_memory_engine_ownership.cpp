/*
	TEST_MEMORY_ENGINE_OWNERSHIP.CPP
	--------------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../atire/indexer.h"
#include "../atire/atire_api.h"
#include "memory_index.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
long long count;
char query[64];

/*
	The document (and query) buffers must be writable: ANT_parser normalises
	tokens in place, so passing string literals (.rodata) segfaults.
*/
char name_one[] = "doc-1", doc_one[] = "<DOC>aardvark zebra</DOC>";
char name_two[] = "doc-2", doc_two[] = "<DOC>zebra quokka</DOC>";
char name_three[] = "doc-3", doc_three[] = "<DOC>aardvark wombat</DOC>";

/*
	ATIRE_indexer::init(char *options) tokenises on '+', not spaces; a
	space-separated options string is silently ignored and the indexer falls
	back to its compiled-in defaults ("index.aspt" in the current working
	directory -- i.e. a stray file in the repo root when run from there).
*/
ATIRE_indexer *indexer = new ATIRE_indexer();
indexer->init((char *)"-nologo+-findex+/tmp/unused_task5.aspt+-fdoclist+/tmp/unused_task5_doclist.aspt");
indexer->index_document(name_one, doc_one);
indexer->index_document(name_two, doc_two);

/*
	The wrapper's constructor forces quantization_bits = 8 on the shared
	index for searching; its destructor must restore the original value or a
	later serialise() by the indexer would skip the automatic bit-count
	computation (which requires -1) and write a mis-quantized on-disk index.
*/
long long original_qbits = indexer->get_index()->get_quantization_bits();

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
indexer->index_document(name_three, doc_three);
ATIRE_API *engine_two = new ATIRE_API();
CHECK(engine_two->open_from_memory_index(indexer->get_index(), indexer->get_doc_list(&count), count, 0) == 0);
strcpy(query, "aardvark");
CHECK(engine_two->search(query, 10) == 2);
strcpy(query, "wombat");
CHECK(engine_two->search(query, 10) == 1);
delete engine_two;

/*
	Both wrappers gone: the shared index's quantization_bits must be back to
	its pre-wrapper value.
*/
CHECK(indexer->get_index()->get_quantization_bits() == original_qbits);

delete indexer;
printf("PASSED\n");
return 0;
}

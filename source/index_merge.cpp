/*
	INDEX_MERGE.CPP
	---------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "index_merge.h"
#include "index_tombstones.h"
#include "btree.h"
#include "btree_head_node.h"
#include "btree_iterator.h"
#include "compression_factory.h"
#include "file.h"
#include "impact_header.h"
#include "maths.h"
#include "memory.h"
#include "memory_index.h"
#include "memory_index_hash_node.h"
#include "search_engine.h"
#include "search_engine_btree_leaf.h"
#include "stats_memory_index.h"
#include "string_pair.h"
#include "version.h"

/*
	ANT_DOCID_RENUMBERER::ANT_DOCID_RENUMBERER()
	--------------------------------------------
	Live documents are numbered densely: within a segment in ascending old
	docid order, segments in the order given (which is manifest order).
*/
ANT_docid_renumberer::ANT_docid_renumberer(ANT_index_tombstones **tombstones, long long *document_counts, long long segments)
{
long long segment, docid, next = 0;

segment_count = segments;
documents = new long long[segment_count];
new_docid = new long long *[segment_count];
for (segment = 0; segment < segment_count; segment++)
	{
	documents[segment] = document_counts[segment];
	new_docid[segment] = new long long[documents[segment]];
	for (docid = 0; docid < documents[segment]; docid++)
		if (tombstones[segment]->is_deleted(docid))
			new_docid[segment][docid] = -1;
		else
			new_docid[segment][docid] = next++;
	}
live_documents = next;
}

/*
	ANT_DOCID_RENUMBERER::~ANT_DOCID_RENUMBERER()
	---------------------------------------------
*/
ANT_docid_renumberer::~ANT_docid_renumberer()
{
long long segment;

for (segment = 0; segment < segment_count; segment++)
	delete [] new_docid[segment];
delete [] new_docid;
delete [] documents;
}

/*
	ANT_DOCID_RENUMBERER::LIVE_IN_SEGMENT()
	---------------------------------------
*/
long long ANT_docid_renumberer::live_in_segment(long long segment)
{
long long docid, live = 0;

for (docid = 0; docid < documents[segment]; docid++)
	if (new_docid[segment][docid] >= 0)
		live++;
return live;
}

/*
	=====================================================================
	ANT_INDEX_MERGER
	---------------
	Faithful in-process adaptation of atire_merge.cpp's merge_index() and its
	free-function helpers (write_postings, write_impact_header_postings,
	write_variable, write_node, find_end_of_node), specialized to Phase 1
	segments and with tombstone filtering + docid renumbering injected at the
	quantum-decode step.  Only the IMPACT_HEADER + FILENAME_INDEX +
	SPECIAL_COMPRESSION build paths are implemented (the only ones compiled).
	=====================================================================
*/

/*
	ANT_INDEX_MERGER::ANT_INDEX_MERGER()
	------------------------------------
	postings_list and compressed_impact_header_buffer are reusable scratch owned
	by the instance (in atire_merge.cpp they were a file-scope global and a
	function-local static respectively); as members they carry no cross-call
	statics yet avoid reallocation on every merge().
*/
ANT_index_merger::ANT_index_merger()
{
postings_list_size = 2;
postings_list = new unsigned char[postings_list_size + ANT_COMPRESSION_FACTORY_END_PADDING];

header_buffer_size = 1 + ANT_impact_header::INFO_SIZE + (ANT_impact_header::NUM_OF_QUANTUMS * 3 * sizeof(ANT_compressable_integer));
compressed_impact_header_buffer = new unsigned char[header_buffer_size];

factory = new ANT_compression_factory;			// defaults to variable-byte, matching the writer/reader

longest_postings = 0;
longest_term = 0;
highest_df = 0;
terms_so_far = 0;
renumberer = NULL;
node_memory = NULL;
memory_stats = NULL;
}

/*
	ANT_INDEX_MERGER::~ANT_INDEX_MERGER()
	-------------------------------------
*/
ANT_index_merger::~ANT_index_merger()
{
delete [] postings_list;
delete [] compressed_impact_header_buffer;
delete factory;
delete renumberer;
delete memory_stats;
delete node_memory;
}

/*
	ANT_INDEX_MERGER::GROW_POSTINGS_BUFFER()
	----------------------------------------
	Grow the shared postings compression buffer by the atire_merge growth factor
	(1.6), preserving a caller's write pointer into it if one is supplied.
*/
void ANT_index_merger::grow_postings_buffer(unsigned char **postings_ptr)
{
unsigned char *new_postings_list;
long long new_size = (long long)(postings_list_size * 1.6);

new_postings_list = new unsigned char[new_size + ANT_COMPRESSION_FACTORY_END_PADDING];
memcpy(new_postings_list, postings_list, (size_t)(postings_list_size * sizeof(*postings_list)));

if (postings_ptr != NULL)
	*postings_ptr = new_postings_list + (*postings_ptr - postings_list);

delete [] postings_list;
postings_list = new_postings_list;
postings_list_size = new_size;
}

/*
	ANT_INDEX_MERGER::FIND_END_OF_NODE()
	------------------------------------
	Duplicate of atire_merge.cpp's find_end_of_node (itself a duplicate of
	ANT_memory_index::find_end_of_node).
*/
ANT_memory_index_hash_node **ANT_index_merger::find_end_of_node(ANT_memory_index_hash_node **start)
{
ANT_memory_index_hash_node **current;

current = start;
if ((*current)->string.length() < B_TREE_PREFIX_SIZE)
	current++;
else
	while (*current != NULL)
		{
		if ((*current)->string.length() < B_TREE_PREFIX_SIZE)
			break;
		if ((*current)->string.true_strncmp(&(*start)->string, B_TREE_PREFIX_SIZE) != 0)
			break;
		current++;
		}
return current;
}

/*
	ANT_INDEX_MERGER::WRITE_NODE()
	------------------------------
	Duplicate of atire_merge.cpp's write_node.
*/
ANT_memory_index_hash_node **ANT_index_merger::write_node(ANT_file *file, ANT_memory_index_hash_node **start)
{
uint8_t zero = 0;
uint64_t eight_byte;
uint32_t four_byte, string_pos;
uint32_t terms_in_node, current_node_head_length;
ANT_memory_index_hash_node **current, **end;

end = find_end_of_node(start);

four_byte = terms_in_node = (uint32_t)(end - start);
file->write((unsigned char *)&terms_in_node, sizeof(terms_in_node));

current_node_head_length = (*start)->string.length() > B_TREE_PREFIX_SIZE ? B_TREE_PREFIX_SIZE : (uint32_t)(*start)->string.length();
string_pos = (uint32_t)(end - start) * ANT_btree_iterator::LEAF_SIZE + 4;
for (current = start; current < end; current++)
	{
	four_byte = (uint32_t)(*current)->collection_frequency;
	file->write((unsigned char *)&four_byte, sizeof(four_byte));

	four_byte = (uint32_t)(*current)->document_frequency;
	file->write((unsigned char *)&four_byte, sizeof(four_byte));

	eight_byte = (uint64_t)((*current)->in_disk.docids_pos_on_disk);
	file->write((unsigned char *)&eight_byte, sizeof(eight_byte));

	four_byte = (uint32_t)((*current)->in_disk.impacted_length);
	file->write((unsigned char *)&four_byte, sizeof(four_byte));

	four_byte = (uint32_t)((*current)->in_disk.end_pos_on_disk - (*current)->in_disk.docids_pos_on_disk);
	file->write((unsigned char *)&four_byte, sizeof(four_byte));

	four_byte = (uint32_t)string_pos;
	file->write((unsigned char *)&four_byte, sizeof(four_byte));

	string_pos += (uint32_t)(*current)->string.length() + 1 - current_node_head_length;
	}

for (current = start; current < end; current++)
	{
	file->write((unsigned char *)((*current)->string.string() + current_node_head_length), (uint32_t)((*current)->string.length()) - current_node_head_length);
	file->write(&zero, 1);
	}

return end;
}

/*
	ANT_INDEX_MERGER::WRITE_POSTINGS()
	----------------------------------
	Adapted from atire_merge.cpp's write_postings.  The static-prune-point
	truncation is dropped (Phase 1 segments are unpruned and, under IMPACT_HEADER,
	this path is only ever reached for '~' variables and the df<=2 special form).
*/
ANT_memory_index_hash_node *ANT_index_merger::write_postings(char *term, ANT_compressable_integer *raw, ANT_file *index, ANT_search_engine_btree_leaf *leaf, long long output_documents)
{
ANT_memory_index_hash_node *node;
uint64_t current_disk_position;
long long len = 0;

ANT_string_pair *t = new ANT_string_pair(term); node = new (memory_stats->postings_memory) ANT_memory_index_hash_node(memory_stats->postings_memory, memory_stats->postings_memory, t, memory_stats); delete t;

node->collection_frequency = leaf->local_collection_frequency;
node->document_frequency = leaf->local_document_frequency;

#ifdef SPECIAL_COMPRESSION
if (node->document_frequency <= 2)
	{
	/*
		Squiggle variables are given to us as an array of values but we need to
		put them out impact ordered, so here we impact order them under a 'tf' of 1
	*/
	if (node->string[0] == '~')
		{
		raw[5] = 0;
		raw[4] = raw[3] = 0;
		raw[2] = raw[1];
		raw[1] = raw[0];
		raw[0] = 1;
		}

	node->in_disk.docids_pos_on_disk = ((long long)raw[1]) << 32 | raw[0];

	if (node->document_frequency == 2)
		{
		node->in_disk.impacted_length = (raw[2] == 0 ? raw[4] : raw[2]);
		node->in_disk.end_pos_on_disk = (raw[2] == 0 ? raw[3] : raw[0]) + node->in_disk.docids_pos_on_disk;
		}
	else
		node->in_disk.impacted_length = node->in_disk.end_pos_on_disk = 0;
	}
else
#endif
	{
	while ((len = factory->compress(postings_list, postings_list_size, raw, leaf->impacted_length)) == 1)
		grow_postings_buffer(NULL);

	current_disk_position = index->tell();
	index->write(postings_list, len);

	node->in_disk.docids_pos_on_disk = current_disk_position;
	node->in_disk.impacted_length = leaf->impacted_length;
	node->in_disk.end_pos_on_disk = index->tell();
	}

longest_postings = ANT_max(len, longest_postings);

return node;
}

/*
	ANT_INDEX_MERGER::WRITE_IMPACT_HEADER_POSTINGS()
	------------------------------------------------
	Adapted verbatim from atire_merge.cpp's write_impact_header_postings; the
	static header buffer is now a member.  The leaf passed in already carries the
	post-filter (surviving) df/cf, so the df<=2 special form is entered correctly
	for terms that become rare only after tombstoning.
*/
ANT_memory_index_hash_node *ANT_index_merger::write_impact_header_postings(char *term, ANT_compressable_integer *header, ANT_compressable_integer quantum_count, ANT_compressable_integer *raw, ANT_file *index, ANT_search_engine_btree_leaf *leaf, long long output_documents)
{
unsigned char *postings_ptr = postings_list;
unsigned char *compressed_header_ptr = compressed_impact_header_buffer + ANT_impact_header::INFO_SIZE;

long long len = 0;
uint64_t current_disk_position;
ANT_memory_index_hash_node *node;

ANT_compressable_integer *impact_value_start = header;
ANT_compressable_integer *document_count_start = header + quantum_count;
ANT_compressable_integer *impact_offset_start = header + (2 * quantum_count);

ANT_compressable_integer *impact_value_pointer = impact_value_start;
ANT_compressable_integer *document_count_pointer = document_count_start;
ANT_compressable_integer *impact_offset_pointer = impact_offset_start;

#ifdef SPECIAL_COMPRESSION
/*
	Because the fiddling here would screw up '~' terms (handled in
	write_postings) we ignore them this time.  Turn the impact header format
	into an impact-ordered format rather than writing two versions of
	write_postings.
*/
if (leaf->local_document_frequency <= 2 && *term != '~')
	{
	ANT_compressable_integer doc_one = raw[*impact_offset_start];
	ANT_compressable_integer doc_two;
	if (quantum_count == 2)
		doc_two = raw[*(impact_offset_start + 1)];
	else
		doc_two = raw[*impact_offset_start + 1];
	ANT_compressable_integer impact_one = *impact_value_start;
	ANT_compressable_integer impact_two = *(impact_value_start + 1);

	leaf->impacted_length = 3;
	if (quantum_count == 2)
		{
		raw[5] = 0;
		raw[4] = doc_two;
		raw[3] = impact_two;
		raw[2] = 0;
		leaf->impacted_length += 3;
		}
	else if (leaf->local_document_frequency == 2)
		{
		raw[3] = 0;
		raw[2] = doc_two;
		leaf->impacted_length++;
		}
	else
		raw[2] = 0;
	raw[1] = doc_one;
	raw[0] = impact_one;

	return write_postings(term, raw, index, leaf, output_documents);
	}
#endif

if (*term == '~')
	return write_postings(term, raw, index, leaf, output_documents);

ANT_string_pair *t = new ANT_string_pair(term); node = new (memory_stats->postings_memory) ANT_memory_index_hash_node(memory_stats->postings_memory, memory_stats->postings_memory, t, memory_stats); delete t;

node->collection_frequency = leaf->local_collection_frequency;
node->document_frequency = leaf->local_document_frequency;

/*
	Compress each quantum's postings
*/
for (ANT_compressable_integer i = 0; i < quantum_count; impact_value_pointer++, document_count_pointer++, impact_offset_pointer++, i++)
	{
	while ((len = factory->compress(postings_ptr, postings_list_size - (postings_ptr - postings_list), raw + *impact_offset_pointer, *document_count_pointer)) == 1)
		grow_postings_buffer(&postings_ptr);
	*impact_offset_pointer = (ANT_compressable_integer)(postings_ptr - postings_list);		// convert pointer to offset
	postings_ptr += len;
	}

/*
	Compress the header
*/
len = factory->compress(compressed_header_ptr, header_buffer_size, header, quantum_count * 3);

((uint64_t *)compressed_impact_header_buffer)[0] = 0;
((uint64_t *)compressed_impact_header_buffer)[1] = 0;
((uint32_t *)compressed_impact_header_buffer)[4] = quantum_count;
((uint32_t *)compressed_impact_header_buffer)[5] = (uint32_t)(ANT_impact_header::INFO_SIZE + len);

compressed_header_ptr += len;

/*
	Write the header then the postings to disk
*/
current_disk_position = index->tell();
index->write(compressed_impact_header_buffer, compressed_header_ptr - compressed_impact_header_buffer);
index->write(postings_list, postings_ptr - postings_list);

node->in_disk.docids_pos_on_disk = current_disk_position;
node->in_disk.impacted_length = leaf->impacted_length;
node->in_disk.end_pos_on_disk = index->tell();

longest_postings = ANT_max(longest_postings, (long long)(postings_ptr - postings_list));

return node;
}

/*
	ANT_INDEX_MERGER::WRITE_VARIABLE()
	----------------------------------
	Adapted from atire_merge.cpp's write_variable.  raw[6] (not [2]) leaves room
	for write_postings to fiddle the value into impact order.
*/
ANT_memory_index_hash_node *ANT_index_merger::write_variable(const char *name, long long value, ANT_file *index, ANT_search_engine_btree_leaf *leaf, long long output_documents)
{
ANT_compressable_integer raw[6];

raw[0] = ((unsigned long long)value) >> 32;
raw[1] = ((unsigned long long)value) & 0xFFFFFFFF;

leaf->impacted_length = leaf->local_collection_frequency = leaf->local_document_frequency = 2;

return write_postings((char *)name, raw, index, leaf, output_documents);
}

/*
	ANT_INDEX_MERGER::MERGE()
	-------------------------
	The in-process specialization of merge_index().  Inputs arrive as already-open
	ANT_search_engine* (Phase 1 disk segments), each with its tombstones.  On any
	failure the partial output is removed and 1 returned; on success 0.
*/
long ANT_index_merger::merge(ANT_search_engine **engines, ANT_index_tombstones **tombstones, long long number_engines, const char *output_filename)
{
long long engine, document, offset;
long long combined_docs = 0, raw_combined_docs = 0, maximum_terms = 0;
long long stemmer, global_trimpoint = 0, static_prune_point;
long long document_filenames_start, document_filenames_finish;
long long filename_offset = 0, filename_offset_sum = 0;
long long longest_document = 0;
long long surviving_df, surviving_cf;
uint64_t current_disk_position;
char file_header[] = "ATIRE Search Engine Index File\n\0\0";
long result = 1;					// pessimistic: assume failure until the footer is written
ANT_memory_index_hash_node *p, **here;

/*
	Everything that cleanup frees is NULL until allocated, so a mid-setup goto
	is safe.
*/
long long *document_counts = NULL;
ANT_btree_iterator **iterators = NULL;
char **terms = NULL;
ANT_search_engine_btree_leaf **leaves = NULL;
ANT_compressable_integer **raw = NULL;
ANT_memory_index_hash_node **term_list = NULL;
long long *filename_index_offsets = NULL;
ANT_compressable_integer *decompress_buffer = NULL;
ANT_compressable_integer **impact_headers = NULL;
ANT_compressable_integer *quantum_counts = NULL;
ANT_compressable_integer *postings_begin = NULL;
ANT_compressable_integer *quantums_processed = NULL;
long *should_process = NULL;
long *strcmp_results = NULL;
ANT_btree_head_node *header = NULL;
ANT_file *index = NULL;

/*
	Reset per-merge state (this instance may drive many sequential merges)
*/
terms_so_far = 0;
longest_postings = 0;
longest_term = 0;
highest_df = 0;

if (number_engines < 1)
	return 1;

/*
	Quantized inputs cannot be merged by this code path
*/
for (engine = 0; engine < number_engines; engine++)
	if (engines[engine]->quantized())
		return 1;

/*
	Build the renumbering table and the output/raw document counts.
	  combined_docs      = live documents in the output (drives the output count)
	  raw_combined_docs  = summed input document counts (drives buffer sizes: a
	                       single input quantum can hold up to a whole input's docs)
*/
document_counts = new long long[number_engines];
for (engine = 0; engine < number_engines; engine++)
	{
	document_counts[engine] = engines[engine]->document_count();
	raw_combined_docs += document_counts[engine];
	maximum_terms += engines[engine]->get_unique_term_count();
	}
renumberer = new ANT_docid_renumberer(tombstones, document_counts, number_engines);
combined_docs = renumberer->total_live_documents();

/*
	Node allocations for the output dictionary live here
*/
node_memory = new ANT_memory;
memory_stats = new ANT_stats_memory_index(node_memory, node_memory);

iterators = new ANT_btree_iterator *[number_engines];
terms = new char *[number_engines];
leaves = new ANT_search_engine_btree_leaf *[number_engines + 1];
raw = new ANT_compressable_integer *[number_engines + 1];
for (engine = 0; engine < number_engines; engine++)
	{
	iterators[engine] = new ANT_btree_iterator(engines[engine]);
	leaves[engine] = new ANT_search_engine_btree_leaf;
	}
leaves[number_engines] = new ANT_search_engine_btree_leaf;
raw[number_engines] = new ANT_compressable_integer[510 + raw_combined_docs];

term_list = new ANT_memory_index_hash_node *[maximum_terms + 1];

decompress_buffer = new ANT_compressable_integer[raw_combined_docs + ANT_COMPRESSION_FACTORY_END_PADDING];
impact_headers = new ANT_compressable_integer *[number_engines + 1];
for (engine = 0; engine <= number_engines; engine++)
	impact_headers[engine] = new ANT_compressable_integer[ANT_impact_header::NUM_OF_QUANTUMS * 3];
quantum_counts = new ANT_compressable_integer[number_engines + 1];
postings_begin = new ANT_compressable_integer[number_engines + 1];
quantums_processed = new ANT_compressable_integer[number_engines];
should_process = new long[number_engines];
strcmp_results = new long[number_engines];
filename_index_offsets = new long long[combined_docs > 0 ? combined_docs : 1];

/*
	Open the output and write the file header
*/
index = new ANT_file;
if (index->open(output_filename, (char *)"w") == 0)
	goto cleanup;
index->write((unsigned char *)file_header, sizeof(file_header));

/*
	All inputs must have been stemmed identically
*/
stemmer = engines[0]->get_variable((char *)"~stemmer");
for (engine = 1; engine < number_engines; engine++)
	if (engines[engine]->get_variable((char *)"~stemmer") != stemmer)
		goto cleanup;					// differently stemmed: cannot merge

/*
	FILENAMES
	---------
	Concatenate the surviving documents' filenames (skipping tombstoned docids)
	in renumbered order -- which, because renumbering preserves (engine, docid)
	order, is exactly the natural loop order.
*/
document_filenames_start = index->tell();
for (engine = 0; engine < number_engines; engine++)
	{
	long long start = engines[engine]->get_variable((char *)"~documentfilenamesstart");
	long long end = engines[engine]->get_variable((char *)"~documentfilenamesfinish");
	unsigned long buf_len = 0;
	char *doc_buf = (char *)malloc((size_t)(end - start));
	char **doc_filenames = engines[engine]->get_document_filenames(doc_buf, &buf_len);

	for (document = 0; document < document_counts[engine]; document++)
		{
		if (renumberer->renumber(engine, document) < 0)
			continue;					// tombstoned: dropped
		filename_index_offsets[filename_offset++] = filename_offset_sum;
		filename_offset_sum += strlen(doc_filenames[document]) + 1;
		index->write((unsigned char *)doc_filenames[document], strlen(doc_filenames[document]) + 1);
		}
	free(doc_buf);
	free(doc_filenames);
	}
document_filenames_finish = index->tell();

if ((p = write_variable("~documentfilenamesstart", document_filenames_start, index, leaves[number_engines], combined_docs)) != NULL)
	term_list[terms_so_far++] = p;
if ((p = write_variable("~documentfilenamesfinish", document_filenames_finish, index, leaves[number_engines], combined_docs)) != NULL)
	term_list[terms_so_far++] = p;

/*
	The filename index: one offset per surviving document, then the total.
*/
document_filenames_start = index->tell();
index->write((unsigned char *)filename_index_offsets, sizeof(*filename_index_offsets) * combined_docs);
index->write((unsigned char *)&filename_offset_sum, sizeof(filename_offset_sum));
document_filenames_finish = index->tell();

if ((p = write_variable("~documentfilenamesindexstart", document_filenames_start, index, leaves[number_engines], combined_docs)) != NULL)
	term_list[terms_so_far++] = p;
if ((p = write_variable("~documentfilenamesindexfinish", document_filenames_finish, index, leaves[number_engines], combined_docs)) != NULL)
	term_list[terms_so_far++] = p;

/*
	~LENGTH
	-------
	The reader derives the document count from this term's df, so it must equal
	combined_docs.  Values are (real length + 1), matching what the writer stores
	and what get_document_lengths()'s consumers expect.  ~documentlongest is
	derived from this filtered vector's max.
*/
offset = 0;
for (engine = 0; engine < number_engines; engine++)
	{
	double dummy;
	ANT_compressable_integer *lengths = engines[engine]->get_document_lengths(&dummy);
	for (document = 0; document < document_counts[engine]; document++)
		{
		if (renumberer->renumber(engine, document) < 0)
			continue;
		raw[number_engines][offset] = lengths[document] + 1;
		if (raw[number_engines][offset] > longest_document)
			longest_document = raw[number_engines][offset];
		offset++;
		}
	}
leaves[number_engines]->impacted_length = offset;
leaves[number_engines]->local_collection_frequency = offset;
leaves[number_engines]->local_document_frequency = offset;
if ((p = write_postings((char *)"~length", raw[number_engines], index, leaves[number_engines], combined_docs)) != NULL)
	term_list[terms_so_far++] = p;

if ((p = write_variable("~documentlongest", longest_document, index, leaves[number_engines], combined_docs)) != NULL)
	term_list[terms_so_far++] = p;

/*
	~TRIMPOINT / ~STEMMER
	---------------------
	Faithful to merge_index(): an unpruned Phase 1 segment reports ~trimpoint 0
	(the reader treats 0 as "no trim"), so global_trimpoint stays 0,
	static_prune_point stays LONG_MAX, and ~trimpoint is not written -- exactly
	what a freshly serialised unpruned segment carries.  ~stemmer (0 here) is
	likewise only written when non-zero.
*/
for (engine = 0; engine < number_engines; engine++)
	global_trimpoint += engines[engine]->get_variable((char *)"~trimpoint");
static_prune_point = global_trimpoint ? global_trimpoint : LONG_MAX;
if (static_prune_point != LONG_MAX)
	if ((p = write_variable("~trimpoint", static_prune_point, index, leaves[number_engines], combined_docs)) != NULL)
		term_list[terms_so_far++] = p;

if (stemmer)
	if ((p = write_variable("~stemmer", stemmer, index, leaves[number_engines], combined_docs)) != NULL)
		term_list[terms_so_far++] = p;

/*
	MAIN TERM WALK
	--------------
	N-way lexicographic lock-step over the inputs' B-tree iterators.
*/
	{
	char *next_term_to_process = NULL;
	long should_continue = false;

	for (engine = 0; engine < number_engines; engine++)
		{
		terms[engine] = iterators[engine]->first(NULL);
		should_continue = should_continue || terms[engine] != NULL;
		}

	while (should_continue)
		{
		/*
			Smallest term across the inputs
		*/
		for (engine = 0; engine < number_engines; engine++)
			if (terms[engine])
				{
				next_term_to_process = terms[engine];
				break;
				}
		for (; engine < number_engines; engine++)
			if (terms[engine] && strcmp(terms[engine], next_term_to_process) < 0)
				next_term_to_process = terms[engine];

		for (engine = 0; engine < number_engines; engine++)
			strcmp_results[engine] = terms[engine] ? strcmp(next_term_to_process, terms[engine]) : 1;

		/*
			'~' terms were all emitted above; skip them here.
		*/
		if (*next_term_to_process != '~')
			{
			ANT_compressable_integer *current, *current_impact_header;
			ANT_compressable_integer number_quantums_used, quantum;
			unsigned int current_tf;
			long process_this_tf, previous_docid;
			long number_documents;

			/*
				Preload each contributing input's raw postings block and impact
				header (get_postings returns a pointer into the in-memory index;
				the destination we pass is ignored for INDEX_IN_MEMORY segments).
			*/
			for (engine = 0; engine < number_engines; engine++)
				if (strcmp_results[engine] == 0)
					{
					iterators[engine]->get_postings_details(leaves[engine]);
					raw[engine] = (ANT_compressable_integer *)engines[engine]->get_postings(leaves[engine], (unsigned char *)raw[number_engines]);
					}
			for (engine = 0; engine < number_engines; engine++)
				if (strcmp_results[engine] == 0)
					{
					quantum_counts[engine] = (ANT_compressable_integer)((uint32_t *)raw[engine])[4];
					postings_begin[engine] = (ANT_compressable_integer)((uint32_t *)raw[engine])[5];
					quantums_processed[engine] = 0;
					factory->decompress(impact_headers[engine], (unsigned char *)raw[engine] + ANT_impact_header::INFO_SIZE, quantum_counts[engine] * 3);
					}

			current = raw[number_engines];
			current_impact_header = impact_headers[number_engines];
			memset(current_impact_header, 0, sizeof(*current_impact_header) * ANT_impact_header::NUM_OF_QUANTUMS * 3);
			number_quantums_used = 0;
			surviving_df = 0;
			surviving_cf = 0;

			for (current_tf = 255; current_tf > 0; current_tf--)
				{
				long long quantum_start, survivors_this_quantum;

				process_this_tf = false;
				for (engine = 0; engine < number_engines; engine++)
					{
					should_process[engine] = false;
					if (strcmp_results[engine] == 0 && quantums_processed[engine] < quantum_counts[engine] && impact_headers[engine][quantums_processed[engine]] == current_tf)
						should_process[engine] = process_this_tf = true;
					}

				if (!process_this_tf)
					continue;

				quantum_start = current - raw[number_engines];
				survivors_this_quantum = 0;
				/*
					The reader reconstructs docids per quantum starting from -1
					(docid = -1; docid += *current), so the stored values live in
					the (docid + 1) domain: seed previous_docid at -1 so the first
					survivor is encoded as new_docid + 1 and the rest as deltas.
				*/
				previous_docid = -1;

				for (engine = 0; engine < number_engines; engine++)
					if (should_process[engine])
						{
						long long old_docid, new_docid;
						long impact_offset;

						number_documents = impact_headers[engine][quantums_processed[engine] + quantum_counts[engine]];
						impact_offset = impact_headers[engine][quantums_processed[engine] + (quantum_counts[engine] * 2)];
						factory->decompress(decompress_buffer, (unsigned char *)raw[engine] + postings_begin[engine] + impact_offset, number_documents);

						/*
							Rebuild absolute old docids, renumber (dropping
							tombstones), and delta-encode survivors against the
							quantum's running previous_docid (renumbering supplies
							the cross-engine shift that 'offset' did in the source).
						*/
						/*
							The input quantum is delta-encoded in the same
							(docid + 1) domain, so seed old_docid at -1 to recover
							absolute 0-based old docids for the renumberer.
						*/
						old_docid = -1;
						for (document = 0; document < number_documents; document++)
							{
							old_docid += decompress_buffer[document];
							new_docid = renumberer->renumber(engine, old_docid);
							if (new_docid < 0)
								continue;
							*current++ = (ANT_compressable_integer)(new_docid - previous_docid);
							previous_docid = new_docid;
							survivors_this_quantum++;
							}
						quantums_processed[engine]++;
						}

				/*
					Commit the merged quantum only if something survived; an
					empty quantum contributes no header entry and no postings.
				*/
				if (survivors_this_quantum > 0)
					{
					*current_impact_header = current_tf;
					current_impact_header[ANT_impact_header::NUM_OF_QUANTUMS] = (ANT_compressable_integer)survivors_this_quantum;
					current_impact_header[ANT_impact_header::NUM_OF_QUANTUMS * 2] = (ANT_compressable_integer)quantum_start;
					current_impact_header++;
					number_quantums_used++;
					surviving_df += survivors_this_quantum;					// each doc appears in exactly one quantum per term
					surviving_cf += (long long)current_tf * survivors_this_quantum;	// impact value == tf for a non-quantized index
					}
				}

			/*
				Make the three impact-header sections contiguous with stride
				number_quantums_used (they were written at stride NUM_OF_QUANTUMS).
			*/
			for (quantum = 0; quantum < number_quantums_used; quantum++)
				impact_headers[number_engines][quantum + number_quantums_used] = impact_headers[number_engines][quantum + ANT_impact_header::NUM_OF_QUANTUMS];
			for (quantum = 0; quantum < number_quantums_used; quantum++)
				impact_headers[number_engines][quantum + 2 * number_quantums_used] = impact_headers[number_engines][quantum + 2 * ANT_impact_header::NUM_OF_QUANTUMS];

			/*
				A term with no surviving documents is dropped entirely.
			*/
			if (surviving_df > 0)
				{
				leaves[number_engines]->local_document_frequency = surviving_df;
				leaves[number_engines]->local_collection_frequency = surviving_cf;
				leaves[number_engines]->impacted_length = current - raw[number_engines];

				if ((p = write_impact_header_postings(next_term_to_process, impact_headers[number_engines], number_quantums_used, raw[number_engines], index, leaves[number_engines], combined_docs)) != NULL)
					{
					term_list[terms_so_far++] = p;

					if (terms_so_far == maximum_terms - 1)
						{
						ANT_memory_index_hash_node **bigger = new ANT_memory_index_hash_node *[maximum_terms * 2];
						memcpy(bigger, term_list, (size_t)(terms_so_far * sizeof(*term_list)));
						delete [] term_list;
						term_list = bigger;
						maximum_terms *= 2;
						}
					}
				}
			}

		/*
			Advance the inputs that matched this term
		*/
		for (engine = 0; engine < number_engines; engine++)
			if (strcmp_results[engine] == 0)
				terms[engine] = iterators[engine]->next();

		should_continue = false;
		for (engine = 0; engine < number_engines; engine++)
			should_continue = should_continue || terms[engine];
		}
	}

/*
	DICTIONARY + FOOTER
	-------------------
	(atire_merge.cpp:1063-1135)
*/
	{
	long btree_root_size = 0;
	ANT_btree_head_node *current_header, *last_header;
	uint64_t terms_in_root, eight_byte;
	uint32_t four_byte;
	uint8_t zero = 0;

	term_list[terms_so_far] = NULL;
	qsort(term_list, terms_so_far, sizeof(*term_list), ANT_memory_index_hash_node::term_compare);

	for (here = term_list; *here != NULL; here = find_end_of_node(here))
		btree_root_size++;

	current_header = header = new ANT_btree_head_node[btree_root_size];
	here = term_list;
	while (*here != NULL)
		{
		current_header->disk_pos = index->tell();
		current_header->node = *here;
		current_header++;
		here = write_node(index, here);
		}
	last_header = current_header;
	terms_in_root = last_header - header;

	current_disk_position = index->tell();
	index->write((unsigned char *)&terms_in_root, sizeof(terms_in_root));

	for (current_header = header; current_header < last_header; current_header++)
		{
		index->write((unsigned char *)current_header->node->string.string(), current_header->node->string.length() > B_TREE_PREFIX_SIZE ? B_TREE_PREFIX_SIZE : current_header->node->string.length());
		index->write(&zero, sizeof(zero));
		eight_byte = current_header->disk_pos;
		index->write((unsigned char *)&eight_byte, sizeof(eight_byte));
		}

	index->write((unsigned char *)&current_disk_position, sizeof(current_disk_position));
	index->write((unsigned char *)&longest_term, sizeof(longest_term));

	four_byte = (uint32_t)terms_so_far;
	index->write((unsigned char *)&four_byte, sizeof(four_byte));

	four_byte = (uint32_t)longest_postings;
	four_byte += 3 * sizeof(*impact_headers[number_engines]) * ANT_impact_header::NUM_OF_QUANTUMS + ANT_impact_header::INFO_SIZE;
	index->write((unsigned char *)&four_byte, sizeof(four_byte));

	index->write((unsigned char *)&highest_df, sizeof(highest_df));
	eight_byte = 0;
	index->write((unsigned char *)&eight_byte, sizeof(eight_byte));
	eight_byte = ANT_file_signature_index;
	index->write((unsigned char *)&eight_byte, sizeof(eight_byte));
	four_byte = (uint32_t)ANT_version;
	index->write((unsigned char *)&four_byte, sizeof(four_byte));
	four_byte = (uint32_t)ANT_file_signature;
	index->write((unsigned char *)&four_byte, sizeof(four_byte));

	index->close();
	}

result = 0;						// success

cleanup:
if (iterators != NULL)
	for (engine = 0; engine < number_engines; engine++)
		delete iterators[engine];
if (leaves != NULL)
	{
	for (engine = 0; engine < number_engines; engine++)
		delete leaves[engine];
	delete leaves[number_engines];
	}
if (impact_headers != NULL)
	{
	for (engine = 0; engine <= number_engines; engine++)
		delete [] impact_headers[engine];
	delete [] impact_headers;
	}
if (raw != NULL)
	delete [] raw[number_engines];

delete [] iterators;
delete [] terms;
delete [] leaves;
delete [] raw;
delete [] term_list;
delete [] document_counts;
delete [] filename_index_offsets;
delete [] decompress_buffer;
delete [] quantum_counts;
delete [] postings_begin;
delete [] quantums_processed;
delete [] should_process;
delete [] strcmp_results;
delete [] header;
delete index;

delete memory_stats;
memory_stats = NULL;
delete node_memory;
node_memory = NULL;
delete renumberer;
renumberer = NULL;

if (result != 0)
	remove(output_filename);

return result;
}

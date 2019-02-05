LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# Enable PIE manually. Will get reset on $(CLEAR_VARS). This
# is what enabling PIE translates to behind the scenes.
LOCAL_CFLAGS := -g -fPIC
LOCAL_LDFLAGS += 

LOCAL_CPP_EXTENSION := .c

ATIRE_DIR := $(LOCAL_PATH)/../../atire
SRC_DIR := $(LOCAL_PATH)/../../source
INCLUDE := include

EXTRA_MINUS_D = -DANT_HAS_ZLIB 

MINUS_D = $(EXTRA_MINUS_D) -DHASHER=1 -DHEADER_NUM=1 
MINUS_D += -DSPECIAL_COMPRESSION=1
MINUS_D += -DTWO_D_ACCUMULATORS
MINUS_D += -DTOP_K_READ_AND_DECOMPRESSOR

# No parallel indexing with macro it has be set in params
#MINUS_D += -DPARALLEL_INDEXING
#MINUS_D += -DPARALLEL_INDEXING_DOCUMENTS

MINUS_D += -DANT_ACCUMULATOR_T="double"
MINUS_D += -DANT_PREGEN_T="unsigned long long"
MINUS_D += -DNOMINMAX

# Do not use IMPACT_HEADER as the old index file is created without it
#MINUS_D += -DIMPACT_HEADER

# NO DOCLIST
MINUS_D += -DFILENAME_INDEX

# USE IT AS LIBRARY WITH API INTERFACE
MINUS_D += -DATIRE_API

CORE_SOURCES =  \
	$(SRC_DIR)/arithmetic_model_bigram.c \
	$(SRC_DIR)/arithmetic_model_tables.c \
	$(SRC_DIR)/arithmetic_model_unigram.c \
	$(SRC_DIR)/assessment.c \
	$(SRC_DIR)/assessment_factory.c \
	$(SRC_DIR)/assessment_INEX.c \
	$(SRC_DIR)/assessment_TREC.c \
	$(SRC_DIR)/barrier.c \
	$(SRC_DIR)/bitstream.c \
	$(SRC_DIR)/bitstring.c \
	$(SRC_DIR)/bitstring_iterator.c \
	$(SRC_DIR)/btree_iterator.c \
	$(SRC_DIR)/channel_file.c \
	$(SRC_DIR)/channel_inex.c \
	$(SRC_DIR)/channel_socket.c \
	$(SRC_DIR)/channel_memory.c \
	$(SRC_DIR)/channel_trec.c \
	$(SRC_DIR)/compress_carryover12.c \
	$(SRC_DIR)/compress_elias_delta.c \
	$(SRC_DIR)/compress_elias_gamma.c \
	$(SRC_DIR)/compress_four_integer_variable_byte.c \
	$(SRC_DIR)/compress_golomb.c \
	$(SRC_DIR)/compression_factory.c \
	$(SRC_DIR)/compression_text_factory.c \
	$(SRC_DIR)/compress_none.c \
	$(SRC_DIR)/compress_relative10.c \
	$(SRC_DIR)/compress_sigma.c \
	$(SRC_DIR)/compress_simple16.c \
	$(SRC_DIR)/compress_simple16_packed.c \
	$(SRC_DIR)/compress_simple8b.c \
	$(SRC_DIR)/compress_simple8b_packed.c \
	$(SRC_DIR)/compress_simple9.c \
	$(SRC_DIR)/compress_simple9_packed.c \
	$(SRC_DIR)/compress_text_bz2.c \
	$(SRC_DIR)/compress_text_deflate.c \
	$(SRC_DIR)/compress_text_none.c \
	$(SRC_DIR)/compress_text_snappy.c \
	$(SRC_DIR)/compress_variable_byte.c \
	$(SRC_DIR)/critical_section.c \
	$(SRC_DIR)/ctypes.c \
	$(SRC_DIR)/directory_iterator.c \
	$(SRC_DIR)/directory_iterator_compressor.c \
	$(SRC_DIR)/directory_iterator_csv.c \
	$(SRC_DIR)/directory_iterator_deflate.c \
	$(SRC_DIR)/directory_iterator_file_buffered.c \
	$(SRC_DIR)/directory_iterator_file.c \
	$(SRC_DIR)/directory_iterator_filter.c \
	$(SRC_DIR)/directory_iterator_filter_spam.c \
	$(SRC_DIR)/directory_iterator_internals.c \
	$(SRC_DIR)/directory_iterator_mime_filter.c \
	$(SRC_DIR)/directory_iterator_multiple.c \
	$(SRC_DIR)/directory_iterator_mysql.c \
	$(SRC_DIR)/directory_iterator_pkzip.c \
	$(SRC_DIR)/directory_iterator_preindex.c \
	$(SRC_DIR)/directory_iterator_preindex_internals.c \
	$(SRC_DIR)/directory_iterator_recursive.c \
	$(SRC_DIR)/directory_iterator_scrub.c \
	$(SRC_DIR)/directory_iterator_tar.c \
	$(SRC_DIR)/directory_iterator_tsv.c \
	$(SRC_DIR)/directory_iterator_warc.c \
	$(SRC_DIR)/directory_iterator_warc_gz_recursive.c \
	$(SRC_DIR)/disk.c \
	$(SRC_DIR)/error.c \
	$(SRC_DIR)/evaluation_binary_preference.c \
	$(SRC_DIR)/evaluation.c \
	$(SRC_DIR)/evaluation_discounted_cumulative_gain.c \
	$(SRC_DIR)/evaluation_expected_reciprocal_rank.c \
	$(SRC_DIR)/evaluation_intent_aware_expected_reciprocal_rank.c \
	$(SRC_DIR)/evaluation_intent_aware_mean_average_precision.c \
	$(SRC_DIR)/evaluation_intent_aware_normalised_discounted_cumulative_gain.c \
	$(SRC_DIR)/evaluation_intent_aware_precision_at_n.c \
	$(SRC_DIR)/evaluation_mean_average_generalised_precision_document.c \
	$(SRC_DIR)/evaluation_mean_average_precision.c \
	$(SRC_DIR)/evaluation_normalised_discounted_cumulative_gain.c \
	$(SRC_DIR)/evaluation_precision_at_n.c \
	$(SRC_DIR)/evaluation_rank_effectiveness.c \
	$(SRC_DIR)/evaluation_success_at_n.c \
	$(SRC_DIR)/evaluator.c \
	$(SRC_DIR)/event.c \
	$(SRC_DIR)/file.c \
	$(SRC_DIR)/file_internals.c \
	$(SRC_DIR)/file_memory.c \
	$(SRC_DIR)/focus_article.c \
	$(SRC_DIR)/focus.c \
	$(SRC_DIR)/focus_lowest_tag.c \
	$(SRC_DIR)/focus_results_list.c \
	$(SRC_DIR)/hash_header.c \
	$(SRC_DIR)/hash_header_collapse.c \
	$(SRC_DIR)/hash_header_experimental.c \
	$(SRC_DIR)/hash_matt.c \
	$(SRC_DIR)/hash_random.c \
	$(SRC_DIR)/index_document.c \
	$(SRC_DIR)/index_document_topsig.c \
	$(SRC_DIR)/index_document_topsig_signature.c \
	$(SRC_DIR)/instream_buffer.c \
	$(SRC_DIR)/instream_bz2.c \
	$(SRC_DIR)/instream_deflate.c \
	$(SRC_DIR)/instream_file.c \
	$(SRC_DIR)/instream_file_star.c \
	$(SRC_DIR)/instream_lzo.c \
	$(SRC_DIR)/instream_memory.c \
	$(SRC_DIR)/instream_pkzip.c \
	$(SRC_DIR)/instream_scrub.c \
	$(SRC_DIR)/maths.c \
	$(SRC_DIR)/memory.c \
	$(SRC_DIR)/memory_index.c \
	$(SRC_DIR)/memory_indexer.c \
	$(SRC_DIR)/memory_index_filename_index.c \
	$(SRC_DIR)/memory_index_hash_node.c \
	$(SRC_DIR)/memory_index_one.c \
	$(SRC_DIR)/mersenne_twister.c \
	$(SRC_DIR)/NEXI_ant.c \
	$(SRC_DIR)/nexi.c \
	$(SRC_DIR)/NEXI_term_ant.c \
	$(SRC_DIR)/NEXI_term.c \
	$(SRC_DIR)/NEXI_term_iterator.c \
	$(SRC_DIR)/numbers.c \
	$(SRC_DIR)/parser.c \
	$(SRC_DIR)/parser_readability.c \
	$(SRC_DIR)/plugin_manager.c \
	$(SRC_DIR)/postings_piece.c \
	$(SRC_DIR)/pregen.c \
	$(SRC_DIR)/pregen_kendall_tau.c \
	$(SRC_DIR)/pregens_writer.c \
	$(SRC_DIR)/pregen_writer.c \
	$(SRC_DIR)/pregen_writer_exact_integers.c \
	$(SRC_DIR)/pregen_writer_exact_strings.c \
	$(SRC_DIR)/pregen_writer_normal.c \
	$(SRC_DIR)/query_boolean.c \
	$(SRC_DIR)/query.c \
	$(SRC_DIR)/query_parse_tree.c \
	$(SRC_DIR)/ranking_function_bm25adpt.c \
	$(SRC_DIR)/ranking_function_bm25.c \
	$(SRC_DIR)/ranking_function_bm25l.c \
	$(SRC_DIR)/ranking_function_bm25plus.c \
	$(SRC_DIR)/ranking_function_bm25t.c \
	$(SRC_DIR)/ranking_function_bose_einstein.c \
	$(SRC_DIR)/ranking_function.c \
	$(SRC_DIR)/ranking_function_dfi.c \
	$(SRC_DIR)/ranking_function_dfi_idf.c \
	$(SRC_DIR)/ranking_function_dfiw.c \
	$(SRC_DIR)/ranking_function_dfiw_idf.c \
	$(SRC_DIR)/ranking_function_dfree.c \
	$(SRC_DIR)/ranking_function_divergence.c \
	$(SRC_DIR)/ranking_function_dlh13.c \
	$(SRC_DIR)/ranking_function_docid.c \
	$(SRC_DIR)/ranking_function_dph.c \
	$(SRC_DIR)/ranking_function_factory.c \
	$(SRC_DIR)/ranking_function_impact.c \
	$(SRC_DIR)/ranking_function_inner_product.c \
	$(SRC_DIR)/ranking_function_kbtfidf.c \
	$(SRC_DIR)/ranking_function_lmd.c \
	$(SRC_DIR)/ranking_function_lmds.c \
	$(SRC_DIR)/ranking_function_lmjm.c \
	$(SRC_DIR)/ranking_function_pregen.c \
	$(SRC_DIR)/ranking_function_puurula.c \
	$(SRC_DIR)/ranking_function_puurula_idf.c \
	$(SRC_DIR)/ranking_function_readability.c \
	$(SRC_DIR)/ranking_function_term_count.c \
	$(SRC_DIR)/ranking_function_tflodop.c \
	$(SRC_DIR)/ranking_function_topsig_negative.c \
	$(SRC_DIR)/ranking_function_topsig_positive.c \
	$(SRC_DIR)/readability.c \
	$(SRC_DIR)/readability_dale_chall.c \
	$(SRC_DIR)/readability_factory.c \
	$(SRC_DIR)/readability_tag_weighting.c \
	$(SRC_DIR)/relevance_feedback_blind_kl.c \
	$(SRC_DIR)/relevance_feedback_blind_kl_rm.c \
	$(SRC_DIR)/relevance_feedback.c \
	$(SRC_DIR)/relevance_feedback_factory.c \
	$(SRC_DIR)/relevance_feedback_topsig.c \
	$(SRC_DIR)/relevant_document.c \
	$(SRC_DIR)/relevant_subtopic.c \
	$(SRC_DIR)/relevant_topic.c \
	$(SRC_DIR)/search_engine_accumulator.c \
	$(SRC_DIR)/search_engine.c \
	$(SRC_DIR)/search_engine_forum.c \
	$(SRC_DIR)/search_engine_forum_INEX_bep.c \
	$(SRC_DIR)/search_engine_forum_INEX.c \
	$(SRC_DIR)/search_engine_forum_INEX_efficiency.c \
	$(SRC_DIR)/search_engine_forum_INEX_focus.c \
	$(SRC_DIR)/search_engine_forum_TREC.c \
	$(SRC_DIR)/search_engine_memory_index.c \
	$(SRC_DIR)/search_engine_readability.c \
	$(SRC_DIR)/search_engine_result.c \
	$(SRC_DIR)/search_engine_result_id_iterator.c \
	$(SRC_DIR)/search_engine_result_doc_iterator.c \
	$(SRC_DIR)/search_engine_result_iterator.c \
	$(SRC_DIR)/semaphores.c \
	$(SRC_DIR)/snippet_beginning.c \
	$(SRC_DIR)/snippet_best_tag.c \
	$(SRC_DIR)/snippet.c \
	$(SRC_DIR)/snippet_factory.c \
	$(SRC_DIR)/snippet_tag.c \
	$(SRC_DIR)/snippet_tficf.c \
	$(SRC_DIR)/snippet_word_cloud.c \
	$(SRC_DIR)/sockets.c \
	$(SRC_DIR)/stats.c \
	$(SRC_DIR)/stats_memory_index.c \
	$(SRC_DIR)/stats_search_engine.c \
	$(SRC_DIR)/stats_time.c \
	$(SRC_DIR)/stem_krovetz.c \
	$(SRC_DIR)/stemmer.c \
	$(SRC_DIR)/stemmer_factory.c \
	$(SRC_DIR)/stemmer_term_similarity.c \
	$(SRC_DIR)/stemmer_term_similarity_threshold.c \
	$(SRC_DIR)/stemmer_term_similarity_weighted.c \
	$(SRC_DIR)/stem_otago.c \
	$(SRC_DIR)/stem_otago_v2.c \
	$(SRC_DIR)/stem_paice_husk.c \
	$(SRC_DIR)/stem_porter.c \
	$(SRC_DIR)/stem_s.c \
	$(SRC_DIR)/stem_snowball.c \
	$(SRC_DIR)/stop_word.c \
	$(SRC_DIR)/str.c \
	$(SRC_DIR)/term_divergence_kl.c \
	$(SRC_DIR)/thesaurus.c \
	$(SRC_DIR)/thesaurus_relationship.c \
	$(SRC_DIR)/thesaurus_wordnet.c \
	$(SRC_DIR)/threads.c \
	$(SRC_DIR)/unicode.c \
	$(SRC_DIR)/unicode_tables.c \
	$(SRC_DIR)/version.c 

OTHER_SOURCES := glob.c antelope_helper.c

LOCAL_MODULE    := antelope_core

LOCAL_SRC_FILES := $(OTHER_SOURCES) \
				$(CORE_SOURCES) 
				
LOCAL_LDLIBS    := -llog -lz

LOCAL_CFLAGS    += -DONE_PARSER -D_CRT_SECURE_NO_WARNINGS -DANT_WITHOUT_STL \
		-DATIRE_MOBILE -DATIRE_JNI $(MINUS_D) \
		-I ./include
		
#LOCAL_C_INCLUDES := ./include $(SRC_DIR)

	
include $(BUILD_STATIC_LIBRARY)

# the api lib, which will depend on and include the core one
#

include $(CLEAR_VARS)
LOCAL_CFLAGS := -fPIC
LOCAL_LDFLAGS += 

LOCAL_CPP_EXTENSION := .c

API_SOURCES = $(ATIRE_DIR)/ant_param_block.c \
			$(ATIRE_DIR)/atire_api.c \
			$(ATIRE_DIR)/atire_api_result.c \
			$(ATIRE_DIR)/atire_api_server.c \
			$(ATIRE_DIR)/atire_api_remote.c
			
INDEX_SOURCES =	$(ATIRE_DIR)/indexer.c \
			 $(ATIRE_DIR)/indexer_param_block.c \
			 $(ATIRE_DIR)/indexer_param_block_rank.c \
			 $(ATIRE_DIR)/indexer_param_block_topsig.c \
			 $(ATIRE_DIR)/indexer_param_block_pregen.c \
			 $(ATIRE_DIR)/indexer_param_block_stem.c
			 
JNI_SOURCES = ./antelope_wrap.c

LOCAL_MODULE    := antelope_jni
LOCAL_SRC_FILES := \
			$(JNI_SOURCES) \
			$(API_SOURCES) \
			$(INDEX_SOURCES)
			
LOCAL_STATIC_LIBRARIES := antelope_core

LOCAL_LDLIBS += -llog -lz -ldl -lc

LOCAL_CFLAGS    := -g -Wno-write-strings  -DATIRE_LIBRARY -DONE_PARSER \
		-D_CRT_SECURE_NO_WARNINGS -DATIRE_MOBILE -DATIRE_JNI \
		$(MINUS_D)  \
		-I $(SRC_DIR) -I ./include -I $(ATIRE_DIR)
		
LOCAL_C_INCLUDES += $(SRC_DIR)

include $(BUILD_SHARED_LIBRARY)
# include $(CLEAR_VARS)

# LOCAL_PATH := $(call my-dir)
# ATIRE_DIR := $(LOCAL_PATH)/../../atire
# SRC_DIR := $(LOCAL_PATH)/../../source
# INCLUDE := include

# LOCAL_CFLAGS := -g -fPIC
# LOCAL_LDFLAGS += 
# LOCAL_CPP_EXTENSION := .c

# LOCAL_MODULE := antelope
# LOCAL_SRC_FILES := $(JNI_SOURCES) $(ATIRE_DIR)/antelope.c
# LOCAL_CPPFLAGS := -std=c++11 -Wall         
# LOCAL_LDLIBS := -llog -lz -ldl
# LOCAL_STATIC_LIBRARIES := antelope_jni
# LOCAL_CFLAGS := -g -Wno-write-strings  -DATIRE_LIBRARY -DONE_PARSER \
# 		-D_CRT_SECURE_NO_WARNINGS -DATIRE_MOBILE -DATIRE_JNI \
# 		$(MINUS_D)  \
# 		-I $(SRC_DIR) -I ./include -I $(ATIRE_DIR)

# include $(BUILD_EXECUTABLE) 
# include $(CLEAR_VARS)

# LOCAL_CFLAGS := -g -fPIC
# LOCAL_LDFLAGS += 
# #LOCAL_CPP_EXTENSION := .c

# LOCAL_MODULE := antelope-dict
# LOCAL_SRC_FILES := $(ATIRE_DIR)/atire_dictionary.c
# #LOCAL_CPPFLAGS := -std=gnu++0x -Wall         
# LOCAL_LDLIBS += -llog -lz -ldl
# LOCAL_SHARED_LIBRARIES += antelope_jni
# LOCAL_CFLAGS += -I ./include -I $(ATIRE_DIR) -I $(SRC_DIR)

# include $(BUILD_EXECUTABLE)  

# include $(CLEAR_VARS)
# LOCAL_CFLAGS := -g -fPIC
# LOCAL_LDFLAGS += 
# #LOCAL_CPP_EXTENSION := .c

# LOCAL_MODULE := index
# LOCAL_SRC_FILES := $(ATIRE_DIR)/index.c
# #LOCAL_CPPFLAGS := -std=gnu++0x -Wall         
# LOCAL_LDLIBS += -llog -lz -ldl
# LOCAL_STATIC_LIBRARIES := antelope_core
# LOCAL_SHARED_LIBRARIES += antelope_jni
# LOCAL_CFLAGS += $(MINUS_D) -I ./include -I $(ATIRE_DIR) -I $(SRC_DIR)

# include $(BUILD_EXECUTABLE)  

AM_CPPFLAGS = $(AM_CFLAGS)
AM_LDFLAGS = @LIBS@

CC = g++

ATIRE_DIR = $(srcdir)/../../atire
SOURCE_DIR = $(srcdir)/../../source

AM_CPPFLAGS += -fPIC -std=c++0x -pedantic -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -I$(ATIRE_DIR) -I$(SOURCE_DIR)

MINUS_D = $(EXTRA_MINUS_D) -DHASHER=1 -DHEADER_NUM=1 
MINUS_D += -DSPECIAL_COMPRESSION=1
MINUS_D += -DTWO_D_ACCUMULATORS
MINUS_D += -DTOP_K_READ_AND_DECOMPRESSOR
MINUS_D += -DPARALLEL_INDEXING
MINUS_D += -DPARALLEL_INDEXING_DOCUMENTS
MINUS_D += -DANT_ACCUMULATOR_T="double"
MINUS_D += -DANT_PREGEN_T="unsigned long long"
MINUS_D += -DNOMINMAX
MINUS_D += -DIMPACT_HEADER
MINUS_D += -DFILENAME_INDEX

lib_LTLIBRARIES = libatire_jni.la

libatire_jni_la_SOURCES = atire_wrap.c atire_helper.c $(top_srcdir)/atire/atire_atire.c $(top_srcdir)/atire/index.c
libatire_jni_la_CFLAGS = -DATIRE_LIBRARY  -DONE_PARSER -D_CRT_SECURE_NO_WARNINGS -DHASHER=1 -DHEADER_HASHER=1 \
		-DSPECIAL_COMPRESSION=1 -DTOP_K_READ_AND_DECOMPRESSOR $(MINUS_D) -I$(top_srcdir)/source
libatire_jni_la_LDFLAGS = -fPIC -export-dynamic -shared  
# -Bsymbolic -export-dynamic
libatire_jni_la_DEPENDENCIES = ../../libatire_api.la  ../../libatire_ant_param.la ../../libatire_index_param.la

# other possible libraries
#  -L../../ -latire_index_param -L../../ -latire_index 
libatire_jni_la_LIBADD =   ../../libatire_api.la ../../libatire_ant_param.la ../../libatire_index_param.la 

noinst_PROGRAMS = index_test atire_test atire_command

index_test_SOURCES = test/index_test.c
index_test_CFLAGS = -I$(srcdir)/include
index_test_LDADD = -L. -latire_jni -L../../ -latire_core
index_test_LDFLAGS = -static -export-dynamic
index_test_DEPENDENCIES = libatire_jni.la ../../libatire_core.la

atire_test_SOURCES = test/atire_test.c
atire_test_CFLAGS = -I$(srcdir)/include
atire_test_LDADD = -L. -latire_jni -L../../ -latire_core -latire_api
atire_test_LDFLAGS = -static -export-dynamic
atire_test_DEPENDENCIES = libatire_jni.la ../../libatire_core.la ../../libatire_api.la

atire_command_SOURCES = test/atire_command.c \
						$(top_srcdir)/atire/atire_api_remote.c
atire_command_CFLAGS = -I$(srcdir)/include -DATIRE_JNI -I$(top_srcdir)/source
atire_command_LDADD = -L. -latire_jni -L../../ -latire_core -latire_api
atire_command_LDFLAGS = -static -export-dynamic
atire_command_DEPENDENCIES = libatire_jni.la ../../libatire_core.la ../../libatire_api.la
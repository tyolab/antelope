AM_CPPFLAGS = $(AM_CFLAGS)
AM_LDFLAGS = @LIBS@

CC = g++

AM_CPPFLAGS += -std=c++0x -pedantic -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux


lib_LTLIBRARIES = libatire_jni.la


libatire_jni_la_SOURCES = atire_wrap.c  $(top_srcdir)/atire/atire.c $(top_srcdir)/atire/index.c $(top_srcdir)/atire/ant_param_block.c
libatire_jni_la_CFLAGS = -DATIRE_LIBRARY  -DONE_PARSER -D_CRT_SECURE_NO_WARNINGS -DHASHER=1 -DHEADER_HASHER=1 \
		-DSPECIAL_COMPRESSION=1 -DTOP_K_READ_AND_DECOMPRESSOR -I$(top_srcdir)/source
libatire_jni_la_LDFLAGS = 
libatire_jni_la_DEPENDENCIES = ../../libatire_index_param.la ../../libatire_index_param.la ../../libatire_api.la

# other possible libraries
#  -L../../ -latire_index_param -L../../ -latire_index 
libatire_jni_la_LIBADD =  -L../../ -latire_index_param

noinst_PROGRAMS = index_test atire_test

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
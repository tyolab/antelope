#!/bin/bash

EXTRA_MINUS_D="-DANT_HAS_ZLIB"

MINUS_D="${EXTRA_MINUS_D} -DHASHER=1 -DHEADER_NUM=1 "
MINUS_D="${MINUS_D} -DSPECIAL_COMPRESSION=1"
MINUS_D="${MINUS_D} -DTWO_D_ACCUMULATORS"
MINUS_D="${MINUS_D} -DTOP_K_READ_AND_DECOMPRESSOR"

# NO PARALLEL INDEXING"
#MINUS_D="${MINUS_D} -DPARALLEL_INDEXING"
#MINUS_D="${MINUS_D} -DPARALLEL_INDEXING_DOCUMENTS"
MINUS_D="${MINUS_D} -DANT_ACCUMULATOR_T=\"double\""
MINUS_D="${MINUS_D} -DANT_PREGEN_T=\"unsigned long long\""
MINUS_D="${MINUS_D} -DNOMINMAX"
MINUS_D="${MINUS_D} -DIMPACT_HEADER"

# NO DOCLIST"
MINUS_D="${MINUS_D} -DFILENAME_INDEX"

export CFLAGS="-DBUILDING_NODE_EXTENSION -DATIRE_LIBRARY -DONE_PARSER \
		-D_CRT_SECURE_NO_WARNINGS -DATIRE_MOBILE -DATIRE_JNI \
		${MINUS_D} -I../source -I../atire"
		
export CXXFLAGS=${CFLAGS}

export CXX="g++ $CXXFLAGS"

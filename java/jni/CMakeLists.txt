# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.18.1)

# Declares and names the project.

project("antelope_jni")

#list(APPEND CMAKE_CXX_SOURCE_FILE_EXTENSIONS c)

file(GLOB APIS "atire/atire_api*.cpp" )
file(GLOB INDEXERS "atire/indexer*.cpp" )
file(GLOB SRCS "src/*.cpp" )

#set_source_files_properties(${APIS} PROPERTIES LANGUAGE CXX)
#set_source_files_properties(${SRCS} PROPERTIES LANGUAGE CXX)
#set_source_files_properties(atire/ant_param_block.c LANGUAGE CXX)
set_source_files_properties(glob.c PROPERTIES LANGUAGE C)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")

# set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_BUILD_TYPE Debug)

# similar to release but with debug symbols
# set(CMAKE_BUILD_TYPE RelWithDebInfo)

add_compile_definitions(
        ANT_HAS_ZLIB
        ATIRE_LIBRARY
        ONE_PARSER
        ATIRE_JNI
        ATIRE_MOBILE
        _CRT_SECURE_NO_WARNINGS
        ANT_WITHOUT_STL

        FILENAME_INDEX
        ANTELOPE_API
        HASHER=1
        HEADER_NUM=1
        SPECIAL_COMPRESSION=1
        TWO_D_ACCUMULATORS
        TOP_K_READ_AND_DECOMPRESSOR
        #ANT_ACCUMULATOR_T="double"
        #ANT_PREGEN_T="unsigned long long"
        NOMINMAX
        )

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.

add_library( # Sets the name of the library.
             antelope_jni

             # Sets the library as a shared library.
             SHARED

             # Provides a relative path to your source file(s).
             atire/ant_param_block.cpp
             antelope_wrap.cpp
             ${SRCS}
             ${INDEXERS}
             ${APIS}
             glob.cpp
        )

include_directories(
        atire
        src
        include
        .
        )

# Searches for a specified prebuilt library and stores the path as a
# variable. Because CMake includes system libraries in the search path by
# default, you only need to specify the name of the public NDK library
# you want to add. CMake verifies that the library exists before
# completing its build.

find_library( # Sets the name of the path variable.
              log-lib

              # Specifies the name of the NDK library that
              # you want CMake to locate.
              log
              dl
              z
              c
        )

# Specifies libraries CMake should link to your target library. You
# can link multiple libraries, such as libraries you define in this
# build script, prebuilt third-party libraries, or system libraries.

target_link_libraries( # Specifies the target library.
                       antelope_jni

                       # Links the target library to the log library
                       # included in the NDK.
                       ${log-lib}
                       dl
                       z
                       c
        )

target_compile_options(antelope_jni
        PUBLIC
        -std=c++11
        )
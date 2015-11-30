/*
 * atire_node.cpp
 *
 *  Created on: 11 May 2015
 *      Author: monfee
 */

#include <node.h>
#include <v8.h>

#include "../atire/indexer.h"

#include <string.h>

using namespace v8;

const char *ATIRE_indexer::EMPTY_DOCUMENT_CONTENT = "<ERROR>EMPTYDOCUMENT</ERROR>"; // [30]; //
const char *ATIRE_indexer::EMPTY_DOCUMENT_FILENAME = "EMPTY DOCUMEN TTITLE"; // [30]; //

const int  ATIRE_indexer::EMPTY_DOUCMENT_LENGTH = strlen(EMPTY_DOCUMENT_CONTENT); // 28; //

Handle<Value> Version(const Arguments& args) {
  HandleScope scope;
  return scope.Close(String::New("atire-node: 0.0.1"));
}

void init(Handle<Object> target) {
  target->Set(String::NewSymbol("version"),
      FunctionTemplate::New(Version)->GetFunction());
}

//NODE_MODULE(atire_api, init)



/*
 * atire_node.cpp
 *
 *  Created on: 11 May 2015
 *      Author: monfee
 */

#include <node.h>
#include <v8.h>

#include "../atire/indexer.h"

using namespace v8;

const char *ATIRE_indexer::EMPTY_DOCUMENT_CONTENT = 0;

Handle<Value> Method(const Arguments& args) {
  HandleScope scope;
  return scope.Close(String::New("world"));
}

void init(Handle<Object> target) {
  target->Set(String::NewSymbol("hello"),
      FunctionTemplate::New(Method)->GetFunction());
}

NODE_MODULE(hello, init)



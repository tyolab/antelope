/*
	SEGMENT_INDEX.CPP  (Node-API binding)
	-------------------------------------
	Node-API addon exposing ATIRE_segment_index to JavaScript.  See
	docs/superpowers/specs/2026-07-06-nodejs-segment-index-binding-design.md.

	Statically linked against lib/libantelope_engine.a -- no engine source is
	compiled here, and atire_segment_index.h is include-free, so the addon is
	immune to engine feature-define drift by construction.
*/
#include <napi.h>
#include "../../atire/atire_segment_index.h"

static Napi::Object Init(Napi::Env env, Napi::Object exports)
{
exports.Set("version", Napi::String::New(env, "1.0.0"));
return exports;
}

NODE_API_MODULE(antelope_segment, Init)

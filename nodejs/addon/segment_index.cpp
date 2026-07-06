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
#include <string.h>
#include "../../atire/atire_segment_index.h"

class SegmentIndexWrap : public Napi::ObjectWrap<SegmentIndexWrap>
{
public:
	enum State { CONSTRUCTED, OPEN, MAINTENANCE, CLOSED };

private:
	ATIRE_segment_index *engine;
	State state;
	/* options captured at construction, applied at open() */
	long long option_dimension;
	long option_metric;
	long long option_flush_threshold;	// -1 = leave engine default
	long option_merge_factor;			// -1 = default
	double option_tombstone_ratio;		// < 0 = default
	long option_auto_maintain;			// 0/1

	friend class MaintenanceWorker;		// Task 4

	/* guards; each returns false after throwing */
	bool require_open(Napi::Env env)
	{
	if (state == OPEN)
		return true;
	if (state == MAINTENANCE)
		Napi::Error::New(env, "maintenance in progress").ThrowAsJavaScriptException();
	else
		Napi::Error::New(env, "index is not open").ThrowAsJavaScriptException();
	return false;
	}

public:
	static Napi::Object Register(Napi::Env env, Napi::Object exports);
	SegmentIndexWrap(const Napi::CallbackInfo &info);
	~SegmentIndexWrap() { delete engine; }

	Napi::Value Open(const Napi::CallbackInfo &info);
	Napi::Value Close(const Napi::CallbackInfo &info);
	Napi::Value DocumentCount(const Napi::CallbackInfo &info);
	Napi::Value VectorDimension(const Napi::CallbackInfo &info);
	/* Task 3 */
	Napi::Value AddDocument(const Napi::CallbackInfo &info);
	Napi::Value UpdateDocument(const Napi::CallbackInfo &info);
	Napi::Value DeleteDocument(const Napi::CallbackInfo &info);
	Napi::Value Search(const Napi::CallbackInfo &info);
	Napi::Value SearchVector(const Napi::CallbackInfo &info);
	Napi::Value SearchHybrid(const Napi::CallbackInfo &info);
	/* Task 4 */
	Napi::Value Flush(const Napi::CallbackInfo &info);
	Napi::Value Maintain(const Napi::CallbackInfo &info);
};

/*
	SEGMENTINDEXWRAP::SEGMENTINDEXWRAP()
	-------------------------------------
	Parses the (optional) options object; validates dimension/metric eagerly
	so bad configuration fails at construction rather than at open().
*/
SegmentIndexWrap::SegmentIndexWrap(const Napi::CallbackInfo &info) : Napi::ObjectWrap<SegmentIndexWrap>(info)
{
Napi::Env env = info.Env();

engine = NULL;
state = CONSTRUCTED;
option_dimension = 0;
option_metric = ATIRE_segment_index::VECTOR_METRIC_DOT;
option_flush_threshold = -1;
option_merge_factor = -1;
option_tombstone_ratio = -1.0;
option_auto_maintain = 0;

if (info.Length() >= 1 && !info[0].IsUndefined() && !info[0].IsNull())
	{
	if (!info[0].IsObject())
		{
		Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
		return;
		}
	Napi::Object options = info[0].As<Napi::Object>();
	if (options.Has("dimension"))
		{
		option_dimension = options.Get("dimension").ToNumber().Int64Value();
		if (option_dimension < 1 || option_dimension > 65536)
			{
			Napi::TypeError::New(env, "dimension must be between 1 and 65536").ThrowAsJavaScriptException();
			return;
			}
		}
	if (options.Has("metric"))
		{
		std::string metric = options.Get("metric").ToString().Utf8Value();
		if (metric == "dot")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_DOT;
		else if (metric == "cosine")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_COSINE;
		else if (metric == "l2")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_L2;
		else
			{
			Napi::TypeError::New(env, "metric must be 'dot', 'cosine' or 'l2'").ThrowAsJavaScriptException();
			return;
			}
		}
	if (options.Has("flushThreshold"))
		option_flush_threshold = options.Get("flushThreshold").ToNumber().Int64Value();
	if (options.Has("mergeFactor"))
		option_merge_factor = (long)options.Get("mergeFactor").ToNumber().Int64Value();
	if (options.Has("tombstoneRatio"))
		option_tombstone_ratio = options.Get("tombstoneRatio").ToNumber().DoubleValue();
	if (options.Has("autoMaintain"))
		option_auto_maintain = options.Get("autoMaintain").ToBoolean().Value() ? 1 : 0;
	}
}

/*
	SEGMENTINDEXWRAP::OPEN()
	-------------------------
	Engine setup sequence: set_vector_config() BEFORE open(), then the other
	setters, then open() itself.  A failed open() leaves the instance
	reusable (fresh engine per attempt) rather than transitioning to CLOSED.
*/
Napi::Value SegmentIndexWrap::Open(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state == CLOSED)
	{
	Napi::Error::New(env, "index is closed").ThrowAsJavaScriptException();
	return env.Undefined();
	}
if (state != CONSTRUCTED)
	{
	Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is already open").ThrowAsJavaScriptException();
	return env.Undefined();
	}
if (info.Length() < 1 || !info[0].IsString())
	{
	Napi::TypeError::New(env, "open(directory) requires a string").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string directory = info[0].As<Napi::String>().Utf8Value();

engine = new ATIRE_segment_index();
if (option_dimension > 0 && engine->set_vector_config(option_dimension, option_metric) != 0)
	{
	delete engine;
	engine = NULL;
	Napi::Error::New(env, "invalid vector configuration").ThrowAsJavaScriptException();
	return env.Undefined();
	}
if (option_flush_threshold >= 0)
	engine->set_flush_threshold(option_flush_threshold);
if (option_merge_factor > 0)
	engine->set_merge_factor(option_merge_factor);
if (option_tombstone_ratio >= 0.0)
	engine->set_tombstone_compact_ratio(option_tombstone_ratio);
engine->set_auto_maintain(option_auto_maintain);

if (engine->open(directory.c_str()) != 0)
	{
	delete engine;
	engine = NULL;
	Napi::Error::New(env, "open failed: bad directory, corrupt index, or vector config mismatch").ThrowAsJavaScriptException();
	return env.Undefined();
	}
state = OPEN;
return env.Undefined();
}

/*
	SEGMENTINDEXWRAP::CLOSE()
	---------------------------
	Idempotent-ish: throws if maintenance is in flight; otherwise deletes
	the engine (safe when already NULL) and transitions to CLOSED for good.
*/
Napi::Value SegmentIndexWrap::Close(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state == MAINTENANCE)
	{
	Napi::Error::New(env, "maintenance in progress").ThrowAsJavaScriptException();
	return env.Undefined();
	}
delete engine;
engine = NULL;
state = CLOSED;
return env.Undefined();
}

/*
	SEGMENTINDEXWRAP::DOCUMENTCOUNT()
	-----------------------------------
*/
Napi::Value SegmentIndexWrap::DocumentCount(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
return Napi::Number::New(env, (double)engine->get_document_count());
}

/*
	SEGMENTINDEXWRAP::VECTORDIMENSION()
	-------------------------------------
*/
Napi::Value SegmentIndexWrap::VectorDimension(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
return Napi::Number::New(env, (double)engine->vector_dimension());
}

/* replaced in Task 3 */
Napi::Value SegmentIndexWrap::AddDocument(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }
/* replaced in Task 3 */
Napi::Value SegmentIndexWrap::UpdateDocument(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }
/* replaced in Task 3 */
Napi::Value SegmentIndexWrap::DeleteDocument(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }
/* replaced in Task 3 */
Napi::Value SegmentIndexWrap::Search(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }
/* replaced in Task 3 */
Napi::Value SegmentIndexWrap::SearchVector(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }
/* replaced in Task 3 */
Napi::Value SegmentIndexWrap::SearchHybrid(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }
/* replaced in Task 4 */
Napi::Value SegmentIndexWrap::Flush(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }
/* replaced in Task 4 */
Napi::Value SegmentIndexWrap::Maintain(const Napi::CallbackInfo &info) { Napi::Error::New(info.Env(), "not implemented").ThrowAsJavaScriptException(); return info.Env().Undefined(); }

/*
	SEGMENTINDEXWRAP::REGISTER()
	-------------------------------
	Defines the full method shape now (Task 3/4 methods stubbed above) so
	later tasks only swap implementations, never touch this list.
*/
Napi::Object SegmentIndexWrap::Register(Napi::Env env, Napi::Object exports)
{
Napi::Function ctor = DefineClass(env, "SegmentIndex", {
	InstanceMethod("open", &SegmentIndexWrap::Open),
	InstanceMethod("close", &SegmentIndexWrap::Close),
	InstanceMethod("documentCount", &SegmentIndexWrap::DocumentCount),
	InstanceMethod("vectorDimension", &SegmentIndexWrap::VectorDimension),
	InstanceMethod("addDocument", &SegmentIndexWrap::AddDocument),
	InstanceMethod("updateDocument", &SegmentIndexWrap::UpdateDocument),
	InstanceMethod("deleteDocument", &SegmentIndexWrap::DeleteDocument),
	InstanceMethod("search", &SegmentIndexWrap::Search),
	InstanceMethod("searchVector", &SegmentIndexWrap::SearchVector),
	InstanceMethod("searchHybrid", &SegmentIndexWrap::SearchHybrid),
	InstanceMethod("flush", &SegmentIndexWrap::Flush),
	InstanceMethod("maintain", &SegmentIndexWrap::Maintain),
});
exports.Set("SegmentIndex", ctor);
return exports;
}

static Napi::Object Init(Napi::Env env, Napi::Object exports)
{
exports.Set("version", Napi::String::New(env, "1.0.0"));
return SegmentIndexWrap::Register(env, exports);
}

NODE_API_MODULE(antelope_segment, Init)

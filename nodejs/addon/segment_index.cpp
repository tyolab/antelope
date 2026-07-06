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
	long option_durable;				// 0/1 -- set_durable() before open()
	long option_wal_fsync;				// 0/1 -- set_wal_fsync() before open()
	long option_global_stats;			// 0/1, default 1 -- set_global_stats(0) before open() when false
	long option_approx_bits;			// -1 = not requested; 0 = engine default (256); >0 = explicit
	long option_approx_multiplier;		// 0 = unset; >0 = set_candidate_multiplier()

	friend class MaintenanceWorker;		// async flush/maintain worker mutates state

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
	/* write path + synchronous searches */
	Napi::Value AddDocument(const Napi::CallbackInfo &info);
	Napi::Value UpdateDocument(const Napi::CallbackInfo &info);
	Napi::Value DeleteDocument(const Napi::CallbackInfo &info);
	Napi::Value Search(const Napi::CallbackInfo &info);
	Napi::Value SearchVector(const Napi::CallbackInfo &info);
	Napi::Value SearchHybrid(const Napi::CallbackInfo &info);
	Napi::Value SearchVectorApprox(const Napi::CallbackInfo &info);
	Napi::Value SearchHybridApprox(const Napi::CallbackInfo &info);
	/* async maintenance (AsyncWorker-backed, Promise-returning) */
	Napi::Value Flush(const Napi::CallbackInfo &info);
	Napi::Value Maintain(const Napi::CallbackInfo &info);
	Napi::Value BuildSignatures(const Napi::CallbackInfo &info);
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
option_durable = 0;
option_wal_fsync = 0;
option_global_stats = 1;
option_approx_bits = -1;			// not requested
option_approx_multiplier = 0;		// unset

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
	if (options.Has("durable"))
		option_durable = options.Get("durable").ToBoolean().Value() ? 1 : 0;
	if (options.Has("walFsync"))
		option_wal_fsync = options.Get("walFsync").ToBoolean().Value() ? 1 : 0;
	if (options.Has("globalStats"))
		option_global_stats = options.Get("globalStats").ToBoolean().Value() ? 1 : 0;
	if (options.Has("approximate") && options.Get("approximate").IsObject())
		{
		Napi::Object approx = options.Get("approximate").As<Napi::Object>();
		option_approx_bits = approx.Has("bits") ? (long)approx.Get("bits").As<Napi::Number>().Int64Value() : 0;	// 0 => engine default 256
		if (approx.Has("multiplier"))
			option_approx_multiplier = (long)approx.Get("multiplier").As<Napi::Number>().Int64Value();
		}
	}
}

/*
	SEGMENTINDEXWRAP::OPEN()
	-------------------------
	Engine setup sequence: set_vector_config() BEFORE open(), then the other
	setters, then open() itself.  A failed open() leaves the instance
	reusable (fresh engine per attempt) rather than transitioning to CLOSED.

	`durable` MUST be applied via set_durable() before open() -- like
	set_vector_config(), the engine only honors it pre-open (it decides
	whether open() creates/replays the WAL).  `walFsync` and `globalStats`
	are order-insensitive with respect to open() but are applied here too,
	before open(), for a single obvious setup sequence.
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
if (option_durable && engine->set_durable(1) != 0)
	{
	delete engine;
	engine = NULL;
	Napi::Error::New(env, "set_durable failed: index is already open").ThrowAsJavaScriptException();
	return env.Undefined();
	}
if (option_wal_fsync)
	engine->set_wal_fsync(1);
if (!option_global_stats)
	engine->set_global_stats(0);

if (engine->open(directory.c_str()) != 0)
	{
	delete engine;
	engine = NULL;
	Napi::Error::New(env, "open failed: bad directory, corrupt index, or vector config mismatch").ThrowAsJavaScriptException();
	return env.Undefined();
	}
/* approximate requires the index OPEN with a dimension; apply it here, and it
   is NON-FATAL if it fails (approximate simply stays off -- do NOT throw). */
if (option_approx_bits >= 0)
	{
	engine->set_approximate_config(option_approx_bits);		// non-fatal: approximate stays off on failure
	if (option_approx_multiplier > 0)
		engine->set_candidate_multiplier(option_approx_multiplier);
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

/*
	EXTRACT_VECTOR()
	----------------
	Accepts Float32Array (zero-copy pointer) or number[] (converted into
	scratch, which the caller owns).  Returns NULL and throws on anything
	else or on dimension mismatch.  *scratch is set non-NULL only when a
	conversion allocated; caller delete[]s it.
*/
static const float *extract_vector(Napi::Env env, Napi::Value value, long long dimension, float **scratch)
{
*scratch = NULL;
if (dimension < 1)
	{
	Napi::TypeError::New(env, "vectors are not enabled on this index").ThrowAsJavaScriptException();
	return NULL;
	}
if (value.IsTypedArray())
	{
	Napi::TypedArray typed = value.As<Napi::TypedArray>();
	if (typed.TypedArrayType() != napi_float32_array)
		{
		Napi::TypeError::New(env, "vector must be a Float32Array or number[]").ThrowAsJavaScriptException();
		return NULL;
		}
	if ((long long)typed.ElementLength() != dimension)
		{
		Napi::TypeError::New(env, "vector dimension mismatch").ThrowAsJavaScriptException();
		return NULL;
		}
	return static_cast<const float *>(typed.As<Napi::Float32Array>().Data());
	}
if (value.IsArray())
	{
	Napi::Array array = value.As<Napi::Array>();
	if ((long long)array.Length() != dimension)
		{
		Napi::TypeError::New(env, "vector dimension mismatch").ThrowAsJavaScriptException();
		return NULL;
		}
	*scratch = new float[dimension];
	for (long long which = 0; which < dimension; which++)
		(*scratch)[which] = (float)array.Get((uint32_t)which).ToNumber().DoubleValue();
	return *scratch;
	}
Napi::TypeError::New(env, "vector must be a Float32Array or number[]").ThrowAsJavaScriptException();
return NULL;
}

/*
	HITS_TO_ARRAY()
	---------------
	Deep-copies the engine's hit list (valid only until the next search)
	into a JS array of { key, score, generation, docid }.
*/
static Napi::Array hits_to_array(Napi::Env env, ATIRE_segment_index *engine, long long count)
{
Napi::Array result = Napi::Array::New(env, (size_t)count);
for (long long which = 0; which < count; which++)
	{
	ATIRE_segment_index::hit *hit = engine->get_hit(which);
	Napi::Object entry = Napi::Object::New(env);
	entry.Set("key", Napi::String::New(env, hit->filename == NULL ? "" : hit->filename));
	entry.Set("score", Napi::Number::New(env, hit->score));
	entry.Set("generation", Napi::Number::New(env, (double)hit->generation));
	entry.Set("docid", Napi::Number::New(env, (double)hit->docid));
	result.Set((uint32_t)which, entry);
	}
return result;
}

/*
	SEGMENTINDEXWRAP::ADDDOCUMENT()
	----------------------------------
	add_document(key, text[, vector]) -> { generation, docid }.  The vector
	argument is optional even on a vector-enabled index (lexical-only
	documents are allowed to co-exist with vector documents).  Both
	add_document() overloads take const char* / const float* -- no writable
	copies are needed here (unlike search()).
*/
Napi::Value SegmentIndexWrap::AddDocument(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
float *scratch = NULL;
const float *vector = NULL;

if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString())
	{
	Napi::TypeError::New(env, "addDocument(key, text[, vector])").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string key = info[0].As<Napi::String>().Utf8Value();
std::string text = info[1].As<Napi::String>().Utf8Value();
if (info.Length() >= 3 && !info[2].IsUndefined() && !info[2].IsNull())
	{
	vector = extract_vector(env, info[2], engine->vector_dimension(), &scratch);
	if (vector == NULL)
		return env.Undefined();		// TypeError already thrown
	if (option_metric == ATIRE_segment_index::VECTOR_METRIC_COSINE)
		{
		double norm = 0.0;
		for (long long which = 0; which < engine->vector_dimension(); which++)
			norm += (double)vector[which] * (double)vector[which];
		if (norm == 0.0)
			{
			delete [] scratch;
			Napi::Error::New(env, "zero vector is not valid under the cosine metric").ThrowAsJavaScriptException();
			return env.Undefined();
			}
		}
	}

long long handle = vector != NULL
	? engine->add_document(key.c_str(), text.c_str(), vector)
	: engine->add_document(key.c_str(), text.c_str());
delete [] scratch;

if (handle < 0)
	{
	Napi::Error::New(env, "document rejected: empty or unparseable text, or index is in a degraded read-only state").ThrowAsJavaScriptException();
	return env.Undefined();
	}
Napi::Object ref = Napi::Object::New(env);
ref.Set("generation", Napi::Number::New(env, (double)(handle >> 40)));
ref.Set("docid", Napi::Number::New(env, (double)(handle & ((1LL << 40) - 1))));
return ref;
}

/*
	SEGMENTINDEXWRAP::UPDATEDOCUMENT()
	-------------------------------------
	Upsert: identical to AddDocument except it calls update_document(), which
	replaces (tombstones + re-adds) any existing document under the same key.
*/
Napi::Value SegmentIndexWrap::UpdateDocument(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
float *scratch = NULL;
const float *vector = NULL;

if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString())
	{
	Napi::TypeError::New(env, "updateDocument(key, text[, vector])").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string key = info[0].As<Napi::String>().Utf8Value();
std::string text = info[1].As<Napi::String>().Utf8Value();
if (info.Length() >= 3 && !info[2].IsUndefined() && !info[2].IsNull())
	{
	vector = extract_vector(env, info[2], engine->vector_dimension(), &scratch);
	if (vector == NULL)
		return env.Undefined();		// TypeError already thrown
	if (option_metric == ATIRE_segment_index::VECTOR_METRIC_COSINE)
		{
		double norm = 0.0;
		for (long long which = 0; which < engine->vector_dimension(); which++)
			norm += (double)vector[which] * (double)vector[which];
		if (norm == 0.0)
			{
			delete [] scratch;
			Napi::Error::New(env, "zero vector is not valid under the cosine metric").ThrowAsJavaScriptException();
			return env.Undefined();
			}
		}
	}

long long handle = vector != NULL
	? engine->update_document(key.c_str(), text.c_str(), vector)
	: engine->update_document(key.c_str(), text.c_str());
delete [] scratch;

if (handle < 0)
	{
	Napi::Error::New(env, "document rejected: empty or unparseable text, or index is in a degraded read-only state").ThrowAsJavaScriptException();
	return env.Undefined();
	}
Napi::Object ref = Napi::Object::New(env);
ref.Set("generation", Napi::Number::New(env, (double)(handle >> 40)));
ref.Set("docid", Napi::Number::New(env, (double)(handle & ((1LL << 40) - 1))));
return ref;
}

/*
	SEGMENTINDEXWRAP::DELETEDOCUMENT()
	-------------------------------------
	delete_document()'s key parameter is const char* -- .c_str() is safe.
*/
Napi::Value SegmentIndexWrap::DeleteDocument(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 1 || !info[0].IsString())
	{
	Napi::TypeError::New(env, "deleteDocument(key)").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string key = info[0].As<Napi::String>().Utf8Value();
return Napi::Boolean::New(env, engine->delete_document(key.c_str()) == 0);
}

/*
	SEGMENTINDEXWRAP::SEARCH()
	-----------------------------
	search()'s query parameter is char* (non-const) -- the engine is free to
	mutate the buffer in place, so we pass a writable std::string copy's
	internal buffer (&mutable_query[0]), never .c_str() (which returns a
	const pointer and may point at shared/immutable storage).
*/
Napi::Value SegmentIndexWrap::Search(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber())
	{
	Napi::TypeError::New(env, "search(text, k)").ThrowAsJavaScriptException();
	return env.Undefined();
	}
std::string query = info[0].As<Napi::String>().Utf8Value();
long long top_k = info[1].As<Napi::Number>().Int64Value();
if (top_k < 1)
	return Napi::Array::New(env, 0);
std::string mutable_query = query;		// engine may modify the buffer
long long count = engine->search(&mutable_query[0], top_k);
return hits_to_array(env, engine, count);
}

/*
	SEGMENTINDEXWRAP::SEARCHVECTOR()
	------------------------------------
	Per spec: when vectors are disabled on this index (vector_dimension() ==
	0) this returns an empty array rather than throwing -- extract_vector()
	throws for that case, so it is handled explicitly BEFORE calling it, and
	extract_vector() is still used to validate type/dimension when vectors
	ARE enabled.  search_vector()'s query parameter is const float* -- no
	writable copy needed.
*/
Napi::Value SegmentIndexWrap::SearchVector(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[1].IsNumber())
	{
	Napi::TypeError::New(env, "searchVector(vector, k)").ThrowAsJavaScriptException();
	return env.Undefined();
	}
long long top_k = info[1].As<Napi::Number>().Int64Value();
long long dimension = engine->vector_dimension();
if (dimension < 1)
	return Napi::Array::New(env, 0);		// vectors not enabled: empty result, not a throw
if (top_k < 1)
	return Napi::Array::New(env, 0);

float *scratch = NULL;
const float *vector = extract_vector(env, info[0], dimension, &scratch);
if (vector == NULL)
	return env.Undefined();		// TypeError already thrown

long long count = engine->search_vector(vector, top_k);
delete [] scratch;
return hits_to_array(env, engine, count);
}

/*
	SEGMENTINDEXWRAP::SEARCHHYBRID()
	------------------------------------
	Both the text and vector arguments are independently optional
	(null/undefined => that side is absent).  When BOTH are absent, return []
	without calling the engine.  The text side uses the writable-copy
	pattern (search_hybrid()'s query_text parameter is char*, non-const,
	exactly like search()); the vector side is const float* like
	search_vector().
*/
Napi::Value SegmentIndexWrap::SearchHybrid(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 3 || !info[2].IsNumber())
	{
	Napi::TypeError::New(env, "searchHybrid(text, vector, k)").ThrowAsJavaScriptException();
	return env.Undefined();
	}

bool has_text = !info[0].IsUndefined() && !info[0].IsNull();
bool has_vector = !info[1].IsUndefined() && !info[1].IsNull();
long long top_k = info[2].As<Napi::Number>().Int64Value();

if (has_text && !info[0].IsString())
	{
	Napi::TypeError::New(env, "searchHybrid: text must be a string, null, or undefined").ThrowAsJavaScriptException();
	return env.Undefined();
	}

if (!has_text && !has_vector)
	return Napi::Array::New(env, 0);
if (top_k < 1)
	return Napi::Array::New(env, 0);

std::string mutable_query;
char *text_ptr = NULL;
if (has_text)
	{
	mutable_query = info[0].As<Napi::String>().Utf8Value();	// engine may modify the buffer
	text_ptr = &mutable_query[0];
	}

float *scratch = NULL;
const float *vector = NULL;
if (has_vector)
	{
	vector = extract_vector(env, info[1], engine->vector_dimension(), &scratch);
	if (vector == NULL)
		return env.Undefined();		// TypeError already thrown
	}

long long count = engine->search_hybrid(text_ptr, vector, top_k);
delete [] scratch;
return hits_to_array(env, engine, count);
}

/*
	SEGMENTINDEXWRAP::SEARCHVECTORAPPROX()
	------------------------------------------
	Approximate (Hamming-prefiltered) counterpart of SearchVector() -- mirrors
	it verbatim, changing ONLY the engine call to search_vector_approx(), which
	transparently falls back to exact results for L2/unconfigured indexes.
*/
Napi::Value SegmentIndexWrap::SearchVectorApprox(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[1].IsNumber())
	{
	Napi::TypeError::New(env, "searchVectorApprox(vector, k)").ThrowAsJavaScriptException();
	return env.Undefined();
	}
long long top_k = info[1].As<Napi::Number>().Int64Value();
long long dimension = engine->vector_dimension();
if (dimension < 1)
	return Napi::Array::New(env, 0);		// vectors not enabled: empty result, not a throw
if (top_k < 1)
	return Napi::Array::New(env, 0);

float *scratch = NULL;
const float *vector = extract_vector(env, info[0], dimension, &scratch);
if (vector == NULL)
	return env.Undefined();		// TypeError already thrown

long long count = engine->search_vector_approx(vector, top_k);
delete [] scratch;
return hits_to_array(env, engine, count);
}

/*
	SEGMENTINDEXWRAP::SEARCHHYBRIDAPPROX()
	------------------------------------------
	Approximate counterpart of SearchHybrid() -- mirrors it verbatim, changing
	ONLY the engine call to search_hybrid_approx(), whose vector leg is
	signature-prefiltered (transparent exact fallback for L2/unconfigured).
*/
Napi::Value SegmentIndexWrap::SearchHybridApprox(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 3 || !info[2].IsNumber())
	{
	Napi::TypeError::New(env, "searchHybridApprox(text, vector, k)").ThrowAsJavaScriptException();
	return env.Undefined();
	}

bool has_text = !info[0].IsUndefined() && !info[0].IsNull();
bool has_vector = !info[1].IsUndefined() && !info[1].IsNull();
long long top_k = info[2].As<Napi::Number>().Int64Value();

if (has_text && !info[0].IsString())
	{
	Napi::TypeError::New(env, "searchHybridApprox: text must be a string, null, or undefined").ThrowAsJavaScriptException();
	return env.Undefined();
	}

if (!has_text && !has_vector)
	return Napi::Array::New(env, 0);
if (top_k < 1)
	return Napi::Array::New(env, 0);

std::string mutable_query;
char *text_ptr = NULL;
if (has_text)
	{
	mutable_query = info[0].As<Napi::String>().Utf8Value();	// engine may modify the buffer
	text_ptr = &mutable_query[0];
	}

float *scratch = NULL;
const float *vector = NULL;
if (has_vector)
	{
	vector = extract_vector(env, info[1], engine->vector_dimension(), &scratch);
	if (vector == NULL)
		return env.Undefined();		// TypeError already thrown
	}

long long count = engine->search_hybrid_approx(text_ptr, vector, top_k);
delete [] scratch;
return hits_to_array(env, engine, count);
}

/*
	class MAINTENANCE_WORKER
	------------------------
	Runs flush() or maintain() off the event loop.  Holds a reference to the
	wrapper so GC cannot finalize the engine mid-operation; restores OPEN and
	settles the Promise on completion.
*/
class MaintenanceWorker : public Napi::AsyncWorker
{
public:
	enum Operation { FLUSH, MAINTAIN, BUILD };

private:
	SegmentIndexWrap *wrapper;
	Napi::ObjectReference self;
	Napi::Promise::Deferred deferred;
	Operation operation;
	long result;

public:
	MaintenanceWorker(Napi::Env env, SegmentIndexWrap *wrapper, Napi::Object js_object, Operation operation)
		: Napi::AsyncWorker(env), wrapper(wrapper), self(Napi::Persistent(js_object)),
		  deferred(Napi::Promise::Deferred::New(env)), operation(operation), result(1) {}

	Napi::Promise Promise() { return deferred.Promise(); }

	void Execute()		/* worker thread: NO JS access */
	{
	switch (operation)
		{
		case FLUSH:		result = wrapper->engine->flush(); break;
		case MAINTAIN:	result = wrapper->engine->maintain(); break;
		case BUILD:		result = wrapper->engine->build_signatures(); break;
		}
	}

	void OnOK()
	{
	wrapper->state = SegmentIndexWrap::OPEN;
	if (result == 0)
		deferred.Resolve(Env().Undefined());
	else
		deferred.Reject(Napi::Error::New(Env(), operation == FLUSH ? "flush failed; index degraded to read-only" : operation == BUILD ? "build_signatures failed" : "maintain failed").Value());
	}

	void OnError(const Napi::Error &error)
	{
	wrapper->state = SegmentIndexWrap::OPEN;
	deferred.Reject(error.Value());
	}
};

/*
	SEGMENTINDEXWRAP::FLUSH()
	----------------------------
	Promise-returning methods NEVER throw synchronously: if the index is not
	OPEN (already CLOSED, still CONSTRUCTED, or MAINTENANCE already in
	flight), the busy/error condition is reported as a REJECTED promise, not
	a thrown exception.  Otherwise the state flips to MAINTENANCE
	synchronously (before this call returns) so that any engine call made
	before the AsyncWorker settles hits the busy-guard in require_open().
*/
Napi::Value SegmentIndexWrap::Flush(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state != OPEN)
	{
	Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
	deferred.Reject(Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is not open").Value());
	return deferred.Promise();
	}
state = MAINTENANCE;
MaintenanceWorker *worker = new MaintenanceWorker(env, this, info.This().As<Napi::Object>(), MaintenanceWorker::FLUSH);
worker->Queue();
return worker->Promise();
}

/*
	SEGMENTINDEXWRAP::MAINTAIN()
	--------------------------------
	Identical to Flush() except for the MaintenanceWorker::MAINTAIN
	operation tag -- see Flush()'s banner comment for the busy-guard and
	never-throw-synchronously rules, which apply identically here.
*/
Napi::Value SegmentIndexWrap::Maintain(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state != OPEN)
	{
	Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
	deferred.Reject(Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is not open").Value());
	return deferred.Promise();
	}
state = MAINTENANCE;
MaintenanceWorker *worker = new MaintenanceWorker(env, this, info.This().As<Napi::Object>(), MaintenanceWorker::MAINTAIN);
worker->Queue();
return worker->Promise();
}

/*
	SEGMENTINDEXWRAP::BUILDSIGNATURES()
	--------------------------------------
	Idempotent backfill of the approximate signature (.vsig) sidecars for
	existing segments.  Identical to Maintain() except for the
	MaintenanceWorker::BUILD operation tag -- see Flush()'s banner comment for
	the busy-guard and never-throw-synchronously rules, which apply here too.
*/
Napi::Value SegmentIndexWrap::BuildSignatures(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state != OPEN)
	{
	Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
	deferred.Reject(Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is not open").Value());
	return deferred.Promise();
	}
state = MAINTENANCE;
MaintenanceWorker *worker = new MaintenanceWorker(env, this, info.This().As<Napi::Object>(), MaintenanceWorker::BUILD);
worker->Queue();
return worker->Promise();
}

/*
	SEGMENTINDEXWRAP::REGISTER()
	-------------------------------
	Defines the complete method shape of the class in one place; the
	implementations live above.
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
	InstanceMethod("searchVectorApprox", &SegmentIndexWrap::SearchVectorApprox),
	InstanceMethod("searchHybridApprox", &SegmentIndexWrap::SearchHybridApprox),
	InstanceMethod("flush", &SegmentIndexWrap::Flush),
	InstanceMethod("maintain", &SegmentIndexWrap::Maintain),
	InstanceMethod("buildSignatures", &SegmentIndexWrap::BuildSignatures),
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

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
#include <string>
#include <vector>
#include "../../atire/atire_segment_index.h"
#include "../../source/attribute_store.h"
#include "../../source/filter.h"

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
	long option_hnsw_M;					// -1 = not requested; 0 = engine default (16); >0 = explicit
	long option_hnsw_ef_construction;	// 0 = engine default (200); >0 = explicit
	long option_hnsw_ef_search;			// 0 = unset; >0 = set_ef_search()
	long option_quantize;				// ATIRE_segment_index::QUANTIZE_OFF/REPLACE/EXACT
	long long option_rerank_dim;		// 0 = off
	long option_rerank_quant;			// ATIRE_segment_index::RERANK_QUANT_FLOAT/INT8
	ANT_attribute_schema option_attributes;	// attribute filter schema captured at construction
	bool option_has_attributes;			// true when options.attributes was supplied

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
	Napi::Value SearchVectorHnsw(const Napi::CallbackInfo &info);
	Napi::Value SearchHybridHnsw(const Napi::CallbackInfo &info);
	Napi::Value SearchRerank(const Napi::CallbackInfo &info);
	/* async maintenance (AsyncWorker-backed, Promise-returning) */
	Napi::Value Flush(const Napi::CallbackInfo &info);
	Napi::Value Maintain(const Napi::CallbackInfo &info);
	Napi::Value BuildSignatures(const Napi::CallbackInfo &info);
	Napi::Value BuildHnsw(const Napi::CallbackInfo &info);
	Napi::Value BuildQuantized(const Napi::CallbackInfo &info);
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
option_hnsw_M = -1;					// not requested
option_hnsw_ef_construction = 0;	// engine default
option_hnsw_ef_search = 0;			// unset
option_quantize = ATIRE_segment_index::QUANTIZE_OFF;
option_rerank_dim = 0;				// off
option_rerank_quant = ATIRE_segment_index::RERANK_QUANT_INT8;
option_has_attributes = false;

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
	if (options.Has("hnsw") && options.Get("hnsw").IsObject())
		{
		Napi::Object h = options.Get("hnsw").As<Napi::Object>();
		option_hnsw_M = h.Has("M") ? (long)h.Get("M").As<Napi::Number>().Int64Value() : 0;	// 0 => engine default 16
		if (h.Has("efConstruction"))
			option_hnsw_ef_construction = (long)h.Get("efConstruction").As<Napi::Number>().Int64Value();
		if (h.Has("efSearch"))
			option_hnsw_ef_search = (long)h.Get("efSearch").As<Napi::Number>().Int64Value();
		}
	if (options.Has("quantize"))
		{
		Napi::Value qv = options.Get("quantize");
		std::string mode;
		if (qv.IsString())
			mode = qv.ToString().Utf8Value();
		else if (qv.IsObject())
			{
			Napi::Object qo = qv.As<Napi::Object>();
			mode = qo.Has("mode") ? qo.Get("mode").ToString().Utf8Value() : std::string("replace");
			}
		else
			{
			Napi::TypeError::New(env, "quantize must be 'int8'/'replace'/'exact' or { mode }").ThrowAsJavaScriptException();
			return;
			}
		if (mode == "int8" || mode == "replace")
			option_quantize = ATIRE_segment_index::QUANTIZE_REPLACE;
		else if (mode == "exact")
			option_quantize = ATIRE_segment_index::QUANTIZE_EXACT;
		else
			{
			Napi::TypeError::New(env, "quantize mode must be 'int8', 'replace', or 'exact'").ThrowAsJavaScriptException();
			return;
			}
		}
	if (options.Has("rerank") && options.Get("rerank").IsObject())
		{
		Napi::Object r = options.Get("rerank").As<Napi::Object>();
		option_rerank_dim = r.Has("dimension") ? (long long)r.Get("dimension").As<Napi::Number>().Int64Value() : 0;
		if (r.Has("quantize"))
			{
			std::string q = r.Get("quantize").ToString().Utf8Value();
			option_rerank_quant = (q == "float") ? ATIRE_segment_index::RERANK_QUANT_FLOAT : ATIRE_segment_index::RERANK_QUANT_INT8;
			}
		}
	if (options.Has("attributes") && !options.Get("attributes").IsUndefined() && !options.Get("attributes").IsNull())
		{
		Napi::Value attrs_val = options.Get("attributes");
		if (!attrs_val.IsObject())
			{
			Napi::TypeError::New(env, "attributes must be an object { field: '<type>' }").ThrowAsJavaScriptException();
			return;
			}
		Napi::Object attrs = attrs_val.As<Napi::Object>();
		Napi::Array keys = attrs.GetPropertyNames();
		for (uint32_t k = 0; k < keys.Length(); k++)
			{
			std::string name = keys.Get(k).ToString().Utf8Value();
			std::string spec = attrs.Get(name).ToString().Utf8Value();
			int multi = 0;
			if (spec.size() >= 2 && spec.compare(spec.size() - 2, 2, "[]") == 0)
				{
				multi = 1;
				spec.erase(spec.size() - 2);
				}
			int type;
			if (spec == "int64")
				type = ANT_attribute_schema::TYPE_INT64;
			else if (spec == "string")
				type = ANT_attribute_schema::TYPE_STRING;
			else if (spec == "bool")
				type = ANT_attribute_schema::TYPE_BOOL;
			else
				{
				Napi::TypeError::New(env, "attribute type must be 'int64', 'string', 'bool' (with optional '[]')").ThrowAsJavaScriptException();
				return;
				}
			if (option_attributes.add_field(name.c_str(), type, multi) != 0)
				{
				Napi::TypeError::New(env, "invalid attribute field (duplicate name, too many fields, name too long, or bool[] not allowed)").ThrowAsJavaScriptException();
				return;
				}
			}
		option_has_attributes = true;
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
/* HNSW likewise requires the index OPEN with a dimension; NON-FATAL if it
   fails (HNSW simply stays off -- do NOT throw). */
if (option_hnsw_M >= 0)
	{
	engine->set_hnsw_config(option_hnsw_M, option_hnsw_ef_construction);	// non-fatal: HNSW stays off on failure
	if (option_hnsw_ef_search > 0)
		engine->set_ef_search(option_hnsw_ef_search);
	}
/* Quantization is index-wide and must be set before the first flush; NON-FATAL
   if it fails (mode stays off -- e.g. a different mode already persisted). */
if (option_quantize != ATIRE_segment_index::QUANTIZE_OFF)
	engine->set_quantization(option_quantize);
/* Rerank (late-interaction / MaxSim) config is index-wide and must be set
   before the first flush; NON-FATAL if it fails (rerank simply stays off --
   e.g. a different dimension already persisted). */
if (option_rerank_dim > 0)
	engine->set_rerank_config(option_rerank_dim, option_rerank_quant);
/* Attribute filter schema is index-wide and immutable once set; apply it here
   after a successful open, before the first flush (mirrors the rerank
   placement).  Non-fatal if unsupported -- filtering simply stays off. */
if (option_has_attributes)
	engine->set_attributes_config(option_attributes);
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
	EXTRACT_MULTIVECTORS()
	-----------------------
	Accepts a JS Array of per-row Float32Array | number[] (a ragged-friendly
	shape at the JS level, but every row must have exactly `dimension`
	elements) and flattens it into one row-major float buffer of
	num_vectors * dimension floats, allocated into *scratch (always
	allocated when the row count is > 0; caller delete[]s it).  Returns NULL
	and throws (TypeError) on any row of the wrong type or length.  An empty
	input array yields *num_vectors = 0 and a NULL return without throwing.
*/
static const float *extract_multivectors(Napi::Env env, Napi::Value value, long long dimension, long long *num_vectors, float **scratch)
{
*scratch = NULL;
*num_vectors = 0;
if (!value.IsArray())
	{
	Napi::TypeError::New(env, "multiVectors must be an array of Float32Array/number[]").ThrowAsJavaScriptException();
	return NULL;
	}
Napi::Array rows = value.As<Napi::Array>();
long long count = (long long)rows.Length();
if (count == 0)
	return NULL;
float *buffer = new float[count * dimension];
for (long long row = 0; row < count; row++)
	{
	Napi::Value row_value = rows.Get((uint32_t)row);
	if (row_value.IsTypedArray())
		{
		Napi::TypedArray typed = row_value.As<Napi::TypedArray>();
		if (typed.TypedArrayType() != napi_float32_array || (long long)typed.ElementLength() != dimension)
			{
			delete [] buffer;
			Napi::TypeError::New(env, "multiVectors row dimension mismatch").ThrowAsJavaScriptException();
			return NULL;
			}
		const float *row_data = static_cast<const float *>(typed.As<Napi::Float32Array>().Data());
		memcpy(buffer + row * dimension, row_data, (size_t)dimension * sizeof(float));
		}
	else if (row_value.IsArray())
		{
		Napi::Array row_array = row_value.As<Napi::Array>();
		if ((long long)row_array.Length() != dimension)
			{
			delete [] buffer;
			Napi::TypeError::New(env, "multiVectors row dimension mismatch").ThrowAsJavaScriptException();
			return NULL;
			}
		for (long long which = 0; which < dimension; which++)
			buffer[row * dimension + which] = (float)row_array.Get((uint32_t)which).ToNumber().DoubleValue();
		}
	else
		{
		delete [] buffer;
		Napi::TypeError::New(env, "multiVectors row must be a Float32Array or number[]").ThrowAsJavaScriptException();
		return NULL;
		}
	}
*scratch = buffer;
*num_vectors = count;
return buffer;
}

/*
	JSON_NODE_TO_FILTER()
	----------------------
	Recursively translates a JS predicate node into an UNBUILT ANT_filter tree.
	The node must be an object carrying exactly one operator key
	(and/or/not/eq/in/range).  On malformed input a TypeError is thrown and
	NULL is returned; every already-allocated sub-tree is freed on the failure
	path so no filter node leaks.
*/
static ANT_filter *json_node_to_filter(Napi::Env env, Napi::Value v, const ANT_attribute_schema *schema)
{
if (!v.IsObject() || v.IsArray())
	{
	Napi::TypeError::New(env, "malformed filter: node must be an object").ThrowAsJavaScriptException();
	return NULL;
	}
Napi::Object node = v.As<Napi::Object>();
Napi::Array node_keys = node.GetPropertyNames();
if (node_keys.Length() != 1)
	{
	Napi::TypeError::New(env, "malformed filter: node must have exactly one operator key").ThrowAsJavaScriptException();
	return NULL;
	}
std::string op = node_keys.Get(0u).ToString().Utf8Value();
Napi::Value operand = node.Get(op);

if (op == "and" || op == "or")
	{
	if (!operand.IsArray())
		{
		Napi::TypeError::New(env, "malformed filter: 'and'/'or' value must be an array").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Array arr = operand.As<Napi::Array>();
	int n = (int)arr.Length();
	std::vector<ANT_filter *> children;
	children.reserve(n > 0 ? n : 1);
	for (int i = 0; i < n; i++)
		{
		ANT_filter *child = json_node_to_filter(env, arr.Get((uint32_t)i), schema);
		if (child == NULL)
			{
			for (size_t j = 0; j < children.size(); j++)
				delete children[j];
			return NULL;		// exception already pending
			}
		children.push_back(child);
		}
	ANT_filter **raw = children.empty() ? (ANT_filter **)NULL : &children[0];
	return (op == "and") ? ANT_filter::and_list(raw, n) : ANT_filter::or_list(raw, n);
	}

if (op == "not")
	{
	ANT_filter *child = json_node_to_filter(env, operand, schema);
	if (child == NULL)
		return NULL;		// exception already pending
	return ANT_filter::not_(child);
	}

if (op == "eq")
	{
	if (!operand.IsObject() || operand.IsArray())
		{
		Napi::TypeError::New(env, "malformed filter: 'eq' value must be { field: value }").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Object spec = operand.As<Napi::Object>();
	std::string field = spec.GetPropertyNames().Get(0u).ToString().Utf8Value();
	long fi = schema->field_index(field.c_str());
	if (fi < 0)
		{
		Napi::TypeError::New(env, "filter references unknown field").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Value value = spec.Get(field);
	switch (schema->type(fi))
		{
		case ANT_attribute_schema::TYPE_INT64:
			if (!value.IsNumber())
				{
				Napi::TypeError::New(env, "eq on int64 field requires a number").ThrowAsJavaScriptException();
				return NULL;
				}
			return ANT_filter::eq_int(field.c_str(), value.As<Napi::Number>().Int64Value());
		case ANT_attribute_schema::TYPE_STRING:
			if (!value.IsString())
				{
				Napi::TypeError::New(env, "eq on string field requires a string").ThrowAsJavaScriptException();
				return NULL;
				}
			return ANT_filter::eq_string(field.c_str(), value.As<Napi::String>().Utf8Value().c_str());
		case ANT_attribute_schema::TYPE_BOOL:
			if (!value.IsBoolean())
				{
				Napi::TypeError::New(env, "eq on bool field requires a boolean").ThrowAsJavaScriptException();
				return NULL;
				}
			return ANT_filter::eq_bool(field.c_str(), value.As<Napi::Boolean>().Value() ? 1 : 0);
		}
	Napi::TypeError::New(env, "eq on unsupported field type").ThrowAsJavaScriptException();
	return NULL;
	}

if (op == "in")
	{
	if (!operand.IsObject() || operand.IsArray())
		{
		Napi::TypeError::New(env, "malformed filter: 'in' value must be { field: [values] }").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Object spec = operand.As<Napi::Object>();
	std::string field = spec.GetPropertyNames().Get(0u).ToString().Utf8Value();
	long fi = schema->field_index(field.c_str());
	if (fi < 0)
		{
		Napi::TypeError::New(env, "filter references unknown field").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Value list_val = spec.Get(field);
	if (!list_val.IsArray())
		{
		Napi::TypeError::New(env, "'in' value must be an array").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Array list = list_val.As<Napi::Array>();
	int n = (int)list.Length();
	if (schema->type(fi) == ANT_attribute_schema::TYPE_INT64)
		{
		std::vector<long long> vals;
		vals.reserve(n > 0 ? n : 1);
		for (int i = 0; i < n; i++)
			{
			Napi::Value e = list.Get((uint32_t)i);
			if (!e.IsNumber())
				{
				Napi::TypeError::New(env, "'in' on int64 field requires numbers").ThrowAsJavaScriptException();
				return NULL;
				}
			vals.push_back(e.As<Napi::Number>().Int64Value());
			}
		return ANT_filter::in_int(field.c_str(), vals.empty() ? (const long long *)NULL : &vals[0], n);
		}
	if (schema->type(fi) == ANT_attribute_schema::TYPE_STRING)
		{
		std::vector<std::string> holder;
		holder.reserve(n > 0 ? n : 1);
		for (int i = 0; i < n; i++)
			{
			Napi::Value e = list.Get((uint32_t)i);
			if (!e.IsString())
				{
				Napi::TypeError::New(env, "'in' on string field requires strings").ThrowAsJavaScriptException();
				return NULL;
				}
			holder.push_back(e.As<Napi::String>().Utf8Value());
			}
		std::vector<const char *> ptrs;
		ptrs.reserve(n > 0 ? n : 1);
		for (int i = 0; i < n; i++)
			ptrs.push_back(holder[i].c_str());
		return ANT_filter::in_string(field.c_str(), ptrs.empty() ? (const char *const *)NULL : &ptrs[0], n);
		}
	Napi::TypeError::New(env, "'in' not supported on bool field").ThrowAsJavaScriptException();
	return NULL;
	}

if (op == "range")
	{
	if (!operand.IsObject() || operand.IsArray())
		{
		Napi::TypeError::New(env, "malformed filter: 'range' value must be { field: { gte/gt/lte/lt } }").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Object spec = operand.As<Napi::Object>();
	std::string field = spec.GetPropertyNames().Get(0u).ToString().Utf8Value();
	long fi = schema->field_index(field.c_str());
	if (fi < 0)
		{
		Napi::TypeError::New(env, "filter references unknown field").ThrowAsJavaScriptException();
		return NULL;
		}
	if (schema->type(fi) != ANT_attribute_schema::TYPE_INT64)
		{
		Napi::TypeError::New(env, "range only supported on int64 field").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Value bounds_val = spec.Get(field);
	if (!bounds_val.IsObject() || bounds_val.IsArray())
		{
		Napi::TypeError::New(env, "range bounds must be an object { gte/gt/lte/lt }").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Object bounds = bounds_val.As<Napi::Object>();
	bool has_gte = bounds.Has("gte"), has_gt = bounds.Has("gt");
	bool has_lte = bounds.Has("lte"), has_lt = bounds.Has("lt");
	if (has_gte && has_gt)
		{
		Napi::TypeError::New(env, "range: specify at most one of 'gte'/'gt'").ThrowAsJavaScriptException();
		return NULL;
		}
	if (has_lte && has_lt)
		{
		Napi::TypeError::New(env, "range: specify at most one of 'lte'/'lt'").ThrowAsJavaScriptException();
		return NULL;
		}
	if (!has_gte && !has_gt && !has_lte && !has_lt)
		{
		Napi::TypeError::New(env, "range: at least one bound (gte/gt/lte/lt) is required").ThrowAsJavaScriptException();
		return NULL;
		}
	int has_lo = (has_gte || has_gt) ? 1 : 0;
	int has_hi = (has_lte || has_lt) ? 1 : 0;
	int lo_incl = has_gte ? 1 : 0;
	int hi_incl = has_lte ? 1 : 0;
	long long lo = 0, hi = 0;
	if (has_gte)
		lo = bounds.Get("gte").As<Napi::Number>().Int64Value();
	else if (has_gt)
		lo = bounds.Get("gt").As<Napi::Number>().Int64Value();
	if (has_lte)
		hi = bounds.Get("lte").As<Napi::Number>().Int64Value();
	else if (has_lt)
		hi = bounds.Get("lt").As<Napi::Number>().Int64Value();
	return ANT_filter::range_int(field.c_str(), lo, has_lo, hi, has_hi, lo_incl, hi_incl);
	}

Napi::TypeError::New(env, "malformed filter: unknown operator").ThrowAsJavaScriptException();
return NULL;
}

/*
	PARSE_FILTER_OPTION()
	----------------------
	Top-level filter-option handler.  undefined/null => NULL (unfiltered).  A
	present filter REQUIRES a configured attribute schema; the predicate is
	translated then build()-checked against the schema (a field/type mismatch
	throws).  Returns a built, ready-to-pass ANT_filter* (caller deletes it),
	or NULL with an exception pending on any error.
*/
static ANT_filter *parse_filter_option(Napi::Env env, Napi::Value filterVal, ATIRE_segment_index *engine)
{
if (filterVal.IsUndefined() || filterVal.IsNull())
	return NULL;
if (!engine->attributes_configured())
	{
	Napi::TypeError::New(env, "filter requires an attributes schema").ThrowAsJavaScriptException();
	return NULL;
	}
ANT_filter *f = json_node_to_filter(env, filterVal, engine->attribute_schema());
if (f == NULL)
	return NULL;		// exception already pending
if (f->build(engine->attribute_schema()) != 0)
	{
	delete f;
	Napi::TypeError::New(env, "filter type/field mismatch").ThrowAsJavaScriptException();
	return NULL;
	}
return f;
}

/*
	BUILD_ATTRIBUTE_SET()
	----------------------
	Translates an add/update options object { attributes?, payload? } into a
	heap ANT_attribute_set (caller deletes it).  Returns NULL WITHOUT an
	exception when there is nothing to ingest (no options object, or neither
	attributes nor payload, or attributes not configured on the index) so the
	caller falls back to the existing lean overloads.  On malformed input a
	TypeError is thrown and NULL is returned -- the caller distinguishes the
	two via env.IsExceptionPending().
*/
static ANT_attribute_set *build_attribute_set(Napi::Env env, Napi::Value optsVal, ATIRE_segment_index *engine)
{
if (!optsVal.IsObject() || optsVal.IsArray())
	return NULL;
Napi::Object opts = optsVal.As<Napi::Object>();
bool has_attrs = opts.Has("attributes") && !opts.Get("attributes").IsUndefined() && !opts.Get("attributes").IsNull();
bool has_payload = opts.Has("payload") && !opts.Get("payload").IsUndefined() && !opts.Get("payload").IsNull();
if ((!has_attrs && !has_payload) || !engine->attributes_configured())
	return NULL;

const ANT_attribute_schema *schema = engine->attribute_schema();
ANT_attribute_set *set = new ANT_attribute_set(schema);

if (has_attrs)
	{
	Napi::Value attrs_val = opts.Get("attributes");
	if (!attrs_val.IsObject() || attrs_val.IsArray())
		{
		delete set;
		Napi::TypeError::New(env, "attributes must be an object { field: value | [values] }").ThrowAsJavaScriptException();
		return NULL;
		}
	Napi::Object attrs = attrs_val.As<Napi::Object>();
	Napi::Array keys = attrs.GetPropertyNames();
	for (uint32_t k = 0; k < keys.Length(); k++)
		{
		std::string field = keys.Get(k).ToString().Utf8Value();
		long fi = schema->field_index(field.c_str());
		if (fi < 0)
			{
			delete set;
			Napi::TypeError::New(env, "attributes reference an unknown field").ThrowAsJavaScriptException();
			return NULL;
			}
		int type = schema->type(fi);
		Napi::Value value = attrs.Get(field);
		if (value.IsArray())
			{
			if (type == ANT_attribute_schema::TYPE_BOOL)
				{
				delete set;
				Napi::TypeError::New(env, "bool attribute cannot be multi-valued").ThrowAsJavaScriptException();
				return NULL;
				}
			Napi::Array list = value.As<Napi::Array>();
			for (uint32_t i = 0; i < list.Length(); i++)
				{
				Napi::Value e = list.Get(i);
				if (type == ANT_attribute_schema::TYPE_INT64)
					{
					if (!e.IsNumber())
						{
						delete set;
						Napi::TypeError::New(env, "int64 attribute values must be numbers").ThrowAsJavaScriptException();
						return NULL;
						}
					set->add_int(fi, e.As<Napi::Number>().Int64Value());
					}
				else	/* TYPE_STRING */
					{
					if (!e.IsString())
						{
						delete set;
						Napi::TypeError::New(env, "string attribute values must be strings").ThrowAsJavaScriptException();
						return NULL;
						}
					set->add_string(fi, e.As<Napi::String>().Utf8Value().c_str());
					}
				}
			}
		else
			{
			switch (type)
				{
				case ANT_attribute_schema::TYPE_INT64:
					if (!value.IsNumber())
						{
						delete set;
						Napi::TypeError::New(env, "int64 attribute requires a number").ThrowAsJavaScriptException();
						return NULL;
						}
					set->set_int(fi, value.As<Napi::Number>().Int64Value());
					break;
				case ANT_attribute_schema::TYPE_STRING:
					if (!value.IsString())
						{
						delete set;
						Napi::TypeError::New(env, "string attribute requires a string").ThrowAsJavaScriptException();
						return NULL;
						}
					set->set_string(fi, value.As<Napi::String>().Utf8Value().c_str());
					break;
				case ANT_attribute_schema::TYPE_BOOL:
					if (!value.IsBoolean())
						{
						delete set;
						Napi::TypeError::New(env, "bool attribute requires a boolean").ThrowAsJavaScriptException();
						return NULL;
						}
					set->set_bool(fi, value.As<Napi::Boolean>().Value() ? 1 : 0);
					break;
				}
			}
		}
	}

if (has_payload)
	{
	Napi::Value pv = opts.Get("payload");
	if (pv.IsBuffer())
		{
		Napi::Buffer<unsigned char> buf = pv.As<Napi::Buffer<unsigned char> >();
		set->set_payload(buf.Data(), (long long)buf.Length());
		}
	else if (pv.IsString())
		{
		std::string s = pv.As<Napi::String>().Utf8Value();
		set->set_payload(s.data(), (long long)s.size());
		}
	else
		{
		delete set;
		Napi::TypeError::New(env, "payload must be a Buffer or string").ThrowAsJavaScriptException();
		return NULL;
		}
	}

return set;
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
	if (hit->payload != NULL && hit->payload_length > 0)
		entry.Set("payload", Napi::Buffer<unsigned char>::Copy(env, hit->payload, (size_t)hit->payload_length));
	result.Set((uint32_t)which, entry);
	}
return result;
}

/*
	SEGMENTINDEXWRAP::ADDDOCUMENT()
	----------------------------------
	add_document(key, text[, vector[, multiVectors]]) -> { generation, docid }.
	The vector argument is optional even on a vector-enabled index (lexical-
	only documents are allowed to co-exist with vector documents); likewise
	multiVectors is optional even on a rerank-enabled index.  multiVectors is
	a JS Array of per-row Float32Array|number[], each row exactly
	rerank_dimension() long; it is flattened by extract_multivectors() into
	one row-major float buffer and passed to the 5-arg add_document()
	overload.  When absent/empty, the existing 3-arg/2-arg overloads are used
	unchanged.  All add_document() overloads take const char* / const float*
	-- no writable copies are needed here (unlike search()).
*/
Napi::Value SegmentIndexWrap::AddDocument(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
float *scratch = NULL;
const float *vector = NULL;
float *mv_scratch = NULL;
const float *multivector = NULL;
long long num_vectors = 0;

if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString())
	{
	Napi::TypeError::New(env, "addDocument(key, text[, vector[, multiVectors]])").ThrowAsJavaScriptException();
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
if (info.Length() >= 4 && !info[3].IsUndefined() && !info[3].IsNull())
	{
	multivector = extract_multivectors(env, info[3], engine->rerank_dimension(), &num_vectors, &mv_scratch);
	if (env.IsExceptionPending())
		{
		delete [] scratch;
		return env.Undefined();		// TypeError already thrown
		}
	}

ANT_attribute_set *attr_set = NULL;
if (info.Length() >= 5)
	{
	attr_set = build_attribute_set(env, info[4], engine);
	if (env.IsExceptionPending())
		{
		delete [] scratch;
		delete [] mv_scratch;
		return env.Undefined();		// TypeError already thrown
		}
	}

long long handle = attr_set != NULL
	? engine->add_document(key.c_str(), text.c_str(), vector, multivector, num_vectors, attr_set)
	: (num_vectors > 0
		? engine->add_document(key.c_str(), text.c_str(), vector, multivector, num_vectors)
		: (vector != NULL
			? engine->add_document(key.c_str(), text.c_str(), vector)
			: engine->add_document(key.c_str(), text.c_str())));
delete [] scratch;
delete [] mv_scratch;
delete attr_set;

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
float *mv_scratch = NULL;
const float *multivector = NULL;
long long num_vectors = 0;

if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString())
	{
	Napi::TypeError::New(env, "updateDocument(key, text[, vector[, multiVectors]])").ThrowAsJavaScriptException();
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
if (info.Length() >= 4 && !info[3].IsUndefined() && !info[3].IsNull())
	{
	multivector = extract_multivectors(env, info[3], engine->rerank_dimension(), &num_vectors, &mv_scratch);
	if (env.IsExceptionPending())
		{
		delete [] scratch;
		return env.Undefined();		// TypeError already thrown
		}
	}

ANT_attribute_set *attr_set = NULL;
if (info.Length() >= 5)
	{
	attr_set = build_attribute_set(env, info[4], engine);
	if (env.IsExceptionPending())
		{
		delete [] scratch;
		delete [] mv_scratch;
		return env.Undefined();		// TypeError already thrown
		}
	}

long long handle = attr_set != NULL
	? engine->update_document(key.c_str(), text.c_str(), vector, multivector, num_vectors, attr_set)
	: (num_vectors > 0
		? engine->update_document(key.c_str(), text.c_str(), vector, multivector, num_vectors)
		: (vector != NULL
			? engine->update_document(key.c_str(), text.c_str(), vector)
			: engine->update_document(key.c_str(), text.c_str())));
delete [] scratch;
delete [] mv_scratch;
delete attr_set;

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
Napi::Value filterVal = env.Undefined();
if (info.Length() >= 3 && info[2].IsObject())
	filterVal = info[2].As<Napi::Object>().Get("filter");
ANT_filter *filter = parse_filter_option(env, filterVal, engine);
if (env.IsExceptionPending())
	return env.Undefined();
long long count = (filter != NULL) ? engine->search(&mutable_query[0], top_k, filter) : engine->search(&mutable_query[0], top_k);
delete filter;
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

Napi::Value filterVal = env.Undefined();
if (info.Length() >= 3 && info[2].IsObject())
	filterVal = info[2].As<Napi::Object>().Get("filter");
ANT_filter *filter = parse_filter_option(env, filterVal, engine);
if (env.IsExceptionPending())
	{
	delete [] scratch;
	return env.Undefined();
	}
long long count = (filter != NULL) ? engine->search_vector(vector, top_k, filter) : engine->search_vector(vector, top_k);
delete filter;
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

Napi::Value filterVal = env.Undefined();
if (info.Length() >= 4 && info[3].IsObject())
	filterVal = info[3].As<Napi::Object>().Get("filter");
ANT_filter *filter = parse_filter_option(env, filterVal, engine);
if (env.IsExceptionPending())
	{
	delete [] scratch;
	return env.Undefined();
	}
long long count = (filter != NULL) ? engine->search_hybrid(text_ptr, vector, top_k, filter) : engine->search_hybrid(text_ptr, vector, top_k);
delete filter;
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

Napi::Value filterVal = env.Undefined();
if (info.Length() >= 3 && info[2].IsObject())
	filterVal = info[2].As<Napi::Object>().Get("filter");
ANT_filter *filter = parse_filter_option(env, filterVal, engine);
if (env.IsExceptionPending())
	{
	delete [] scratch;
	return env.Undefined();
	}
long long count = (filter != NULL) ? engine->search_vector_approx(vector, top_k, filter) : engine->search_vector_approx(vector, top_k);
delete filter;
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

Napi::Value filterVal = env.Undefined();
if (info.Length() >= 4 && info[3].IsObject())
	filterVal = info[3].As<Napi::Object>().Get("filter");
ANT_filter *filter = parse_filter_option(env, filterVal, engine);
if (env.IsExceptionPending())
	{
	delete [] scratch;
	return env.Undefined();
	}
long long count = (filter != NULL) ? engine->search_hybrid_approx(text_ptr, vector, top_k, filter) : engine->search_hybrid_approx(text_ptr, vector, top_k);
delete filter;
delete [] scratch;
return hits_to_array(env, engine, count);
}

/*
	SEGMENTINDEXWRAP::SEARCHVECTORHNSW()
	------------------------------------------
	HNSW graph counterpart of SearchVector() -- mirrors it verbatim, changing
	ONLY the engine call to search_vector_hnsw(), which transparently falls back
	to exact results for dot/unconfigured indexes.
*/
Napi::Value SegmentIndexWrap::SearchVectorHnsw(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 2 || !info[1].IsNumber())
	{
	Napi::TypeError::New(env, "searchVectorHnsw(vector, k)").ThrowAsJavaScriptException();
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

Napi::Value filterVal = env.Undefined();
if (info.Length() >= 3 && info[2].IsObject())
	filterVal = info[2].As<Napi::Object>().Get("filter");
ANT_filter *filter = parse_filter_option(env, filterVal, engine);
if (env.IsExceptionPending())
	{
	delete [] scratch;
	return env.Undefined();
	}
long long count = (filter != NULL) ? engine->search_vector_hnsw(vector, top_k, filter) : engine->search_vector_hnsw(vector, top_k);
delete filter;
delete [] scratch;
return hits_to_array(env, engine, count);
}

/*
	SEGMENTINDEXWRAP::SEARCHHYBRIDHNSW()
	------------------------------------------
	HNSW graph counterpart of SearchHybrid() -- mirrors it verbatim, changing
	ONLY the engine call to search_hybrid_hnsw(), whose vector leg is graph-
	searched (transparent exact fallback for dot/unconfigured).
*/
Napi::Value SegmentIndexWrap::SearchHybridHnsw(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();
if (info.Length() < 3 || !info[2].IsNumber())
	{
	Napi::TypeError::New(env, "searchHybridHnsw(text, vector, k)").ThrowAsJavaScriptException();
	return env.Undefined();
	}

bool has_text = !info[0].IsUndefined() && !info[0].IsNull();
bool has_vector = !info[1].IsUndefined() && !info[1].IsNull();
long long top_k = info[2].As<Napi::Number>().Int64Value();

if (has_text && !info[0].IsString())
	{
	Napi::TypeError::New(env, "searchHybridHnsw: text must be a string, null, or undefined").ThrowAsJavaScriptException();
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

Napi::Value filterVal = env.Undefined();
if (info.Length() >= 4 && info[3].IsObject())
	filterVal = info[3].As<Napi::Object>().Get("filter");
ANT_filter *filter = parse_filter_option(env, filterVal, engine);
if (env.IsExceptionPending())
	{
	delete [] scratch;
	return env.Undefined();
	}
long long count = (filter != NULL) ? engine->search_hybrid_hnsw(text_ptr, vector, top_k, filter) : engine->search_hybrid_hnsw(text_ptr, vector, top_k);
delete filter;
delete [] scratch;
return hits_to_array(env, engine, count);
}

/*
	SEGMENTINDEXWRAP::SEARCHRERANK()
	------------------------------------
	Late-interaction (MaxSim) rerank (V5): arg0 is the mandatory query
	multi-vector array (Array<Float32Array|number[]>, each row exactly
	rerank_dimension() long), flattened by extract_multivectors(); arg1 is an
	optional options object { text?, vector?, firstStageN?, topK? } selecting
	the stage-1 retrieval (lexical/vector/hybrid, whichever of text/vector are
	given) whose top firstStageN candidates get MaxSim-reranked down to topK.
	Per spec: when rerank is disabled on this index (rerank_dimension() == 0)
	this returns an empty array rather than throwing, exactly like
	SearchVector() does for vector_dimension() == 0.  search_rerank()'s
	query_text parameter is char* (non-const) like search()/search_hybrid(),
	so the writable-copy pattern is used again here; query_vector and
	query_multivector are const float* -- no writable copies needed for them.
*/
Napi::Value SegmentIndexWrap::SearchRerank(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();
if (!require_open(env))
	return env.Undefined();

long long rerank_dim = engine->rerank_dimension();
if (rerank_dim < 1)
	return Napi::Array::New(env, 0);		// rerank not enabled: empty result, not a throw

if (info.Length() < 1)
	{
	Napi::TypeError::New(env, "searchRerank(queryMultiVectors[, options])").ThrowAsJavaScriptException();
	return env.Undefined();
	}

float *mv_scratch = NULL;
long long num_query_vecs = 0;
const float *query_multivector = extract_multivectors(env, info[0], rerank_dim, &num_query_vecs, &mv_scratch);
if (env.IsExceptionPending())
	return env.Undefined();		// TypeError already thrown

Napi::Object options = (info.Length() >= 2 && info[1].IsObject()) ? info[1].As<Napi::Object>() : Napi::Object::New(env);

bool has_text = options.Has("text") && !options.Get("text").IsUndefined() && !options.Get("text").IsNull();
bool has_vector = options.Has("vector") && !options.Get("vector").IsUndefined() && !options.Get("vector").IsNull();

if (!has_text && !has_vector)
	{
	delete [] mv_scratch;
	Napi::TypeError::New(env, "searchRerank requires options.text and/or options.vector for the first stage").ThrowAsJavaScriptException();
	return env.Undefined();
	}

if (has_text && !options.Get("text").IsString())
	{
	delete [] mv_scratch;
	Napi::TypeError::New(env, "searchRerank: options.text must be a string").ThrowAsJavaScriptException();
	return env.Undefined();
	}

std::string mutable_query;
char *text_ptr = NULL;
if (has_text)
	{
	mutable_query = options.Get("text").As<Napi::String>().Utf8Value();	// engine may modify the buffer
	text_ptr = &mutable_query[0];
	}

float *scratch = NULL;
const float *vector = NULL;
if (has_vector)
	{
	vector = extract_vector(env, options.Get("vector"), engine->vector_dimension(), &scratch);
	if (vector == NULL)
		{
		delete [] mv_scratch;
		return env.Undefined();		// TypeError already thrown
		}
	}

long long first_stage_n = options.Has("firstStageN") ? options.Get("firstStageN").As<Napi::Number>().Int64Value() : 100;
long long top_k = options.Has("topK") ? options.Get("topK").As<Napi::Number>().Int64Value() : 10;
if (top_k < 1)
	{
	delete [] scratch;
	delete [] mv_scratch;
	return Napi::Array::New(env, 0);
	}

ANT_filter *filter = parse_filter_option(env, options.Get("filter"), engine);
if (env.IsExceptionPending())
	{
	delete [] scratch;
	delete [] mv_scratch;
	return env.Undefined();
	}
long long count = (filter != NULL)
	? engine->search_rerank(text_ptr, vector, query_multivector, num_query_vecs, first_stage_n, top_k, filter)
	: engine->search_rerank(text_ptr, vector, query_multivector, num_query_vecs, first_stage_n, top_k);
delete filter;
delete [] scratch;
delete [] mv_scratch;
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
	enum Operation { FLUSH, MAINTAIN, BUILD, BUILD_HNSW, BUILD_QUANTIZED };

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
		case BUILD_HNSW:	result = wrapper->engine->build_hnsw(); break;
		case BUILD_QUANTIZED:	result = wrapper->engine->build_quantized(); break;
		}
	}

	void OnOK()
	{
	wrapper->state = SegmentIndexWrap::OPEN;
	if (result == 0)
		deferred.Resolve(Env().Undefined());
	else
		deferred.Reject(Napi::Error::New(Env(), operation == FLUSH ? "flush failed; index degraded to read-only" : operation == BUILD ? "build_signatures failed" : operation == BUILD_HNSW ? "build_hnsw failed" : operation == BUILD_QUANTIZED ? "build_quantized failed" : "maintain failed").Value());
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
	SEGMENTINDEXWRAP::BUILDHNSW()
	--------------------------------------
	Idempotent backfill of the per-segment HNSW graph for existing segments.
	Identical to BuildSignatures() except for the MaintenanceWorker::BUILD_HNSW
	operation tag -- see Flush()'s banner comment for the busy-guard and
	never-throw-synchronously rules, which apply here too.
*/
Napi::Value SegmentIndexWrap::BuildHnsw(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state != OPEN)
	{
	Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
	deferred.Reject(Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is not open").Value());
	return deferred.Promise();
	}
state = MAINTENANCE;
MaintenanceWorker *worker = new MaintenanceWorker(env, this, info.This().As<Napi::Object>(), MaintenanceWorker::BUILD_HNSW);
worker->Queue();
return worker->Promise();
}

/*
	SEGMENTINDEXWRAP::BUILDQUANTIZED()
	--------------------------------------
	Idempotent backfill: rewrites float .vec disk segments as int8 .qvec
	(replace mode).  Identical to BuildHnsw() except for the
	MaintenanceWorker::BUILD_QUANTIZED operation tag -- see Flush()'s banner
	comment for the busy-guard and never-throw-synchronously rules, which
	apply here too.
*/
Napi::Value SegmentIndexWrap::BuildQuantized(const Napi::CallbackInfo &info)
{
Napi::Env env = info.Env();

if (state != OPEN)
	{
	Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
	deferred.Reject(Napi::Error::New(env, state == MAINTENANCE ? "maintenance in progress" : "index is not open").Value());
	return deferred.Promise();
	}
state = MAINTENANCE;
MaintenanceWorker *worker = new MaintenanceWorker(env, this, info.This().As<Napi::Object>(), MaintenanceWorker::BUILD_QUANTIZED);
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
	InstanceMethod("searchVectorHnsw", &SegmentIndexWrap::SearchVectorHnsw),
	InstanceMethod("searchHybridHnsw", &SegmentIndexWrap::SearchHybridHnsw),
	InstanceMethod("searchRerank", &SegmentIndexWrap::SearchRerank),
	InstanceMethod("flush", &SegmentIndexWrap::Flush),
	InstanceMethod("maintain", &SegmentIndexWrap::Maintain),
	InstanceMethod("buildSignatures", &SegmentIndexWrap::BuildSignatures),
	InstanceMethod("buildHnsw", &SegmentIndexWrap::BuildHnsw),
	InstanceMethod("buildQuantized", &SegmentIndexWrap::BuildQuantized),
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

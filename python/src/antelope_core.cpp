#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <stdexcept>
#include <string>
#include <vector>
#include "atire_segment_index.h"
#include "attribute_store.h"

namespace py = pybind11;

/*
	EXTRACT_VECTOR()
	------------------
	Uniform converter for a Python list/tuple, numpy array (any numeric
	dtype), or array.array into a std::vector<float>. Relies on pybind11's
	stl caster (enabled by <pybind11/stl.h>), which iterates any Python
	sequence of numbers -- correct for all three input shapes, though not
	a zero-copy fast path (a buffer-protocol fast path for numpy/array.array
	is a possible future optimization).
*/
static std::vector<float> extract_vector(py::handle v, long long dim)
{
if (dim < 1)
	throw py::type_error("vectors are not enabled on this index");
std::vector<float> out;
try
	{
	out = v.cast<std::vector<float>>();
	}
catch (const py::cast_error &)
	{
	throw py::type_error("vector must be a sequence of numbers (list/numpy/array)");
	}
if ((long long)out.size() != dim)
	throw py::value_error("vector dimension mismatch");
return out;
}

/*
	EXTRACT_MULTIVECTORS()
	------------------------
	Flattens a Python sequence of row-vectors (each of length dim) into one
	row-major std::vector<float> buffer, mirroring the Node addon's
	extract_multivectors() (nodejs/addon/segment_index.cpp ~line 469). Each
	row is validated via extract_vector() (length == dim). *num receives the
	row count. A None input yields an empty buffer and *num == 0.
*/
static std::vector<float> extract_multivectors(py::handle v, long long dim, long long *num)
{
*num = 0;
std::vector<float> flat;
if (v.is_none())
	return flat;
py::sequence rows;
try
	{
	rows = v.cast<py::sequence>();
	}
catch (const py::cast_error &)
	{
	throw py::type_error("multi_vectors must be a sequence of vectors");
	}
long long n = (long long)py::len(rows);
flat.reserve((size_t)(n * (dim > 0 ? dim : 1)));
for (long long i = 0; i < n; i++)
	{
	std::vector<float> row = extract_vector(rows[i], dim);   // validates each row length == dim
	flat.insert(flat.end(), row.begin(), row.end());
	}
*num = n;
return flat;
}

/*
	BUILD_ATTRIBUTE_SET()
	------------------------
	Converts the Python-facing `attributes` dict / `payload` bytes-or-str into
	a heap-allocated ANT_attribute_set the caller owns (deletes), or NULL when
	neither is given. Mirrors the Node addon's build_attribute_set()
	(nodejs/addon/segment_index.cpp ~line 794): every error path deletes the
	half-built set before throwing, so callers never have to clean up on the
	throwing paths.
*/
static ANT_attribute_set *build_attribute_set(ATIRE_segment_index *engine, py::handle attributes, py::handle payload)
{
bool has_attrs = !attributes.is_none();
bool has_payload = !payload.is_none();
if (!has_attrs && !has_payload)
	return NULL;
if (!engine->attributes_configured())
	throw py::value_error("attributes/payload given but this index has no attributes schema");

const ANT_attribute_schema *schema = engine->attribute_schema();
ANT_attribute_set *set = new ANT_attribute_set(schema);

if (has_attrs)
	{
	py::dict d;
	try
		{
		d = attributes.cast<py::dict>();
		}
	catch (const py::cast_error &)
		{
		delete set;
		throw py::type_error("attributes must be a dict");
		}
	for (auto item : d)
		{
		std::string name = py::str(item.first);
		long fi = schema->field_index(name.c_str());
		if (fi < 0)
			{
			delete set;
			throw py::type_error("unknown attribute field: " + name);
			}
		int type = schema->type(fi);
		py::handle val = item.second;
		bool is_seq = py::isinstance<py::list>(val) || py::isinstance<py::tuple>(val);
		if (is_seq)
			{
			if (type == ANT_attribute_schema::TYPE_BOOL)
				{
				delete set;
				throw py::type_error("bool field cannot be multi-valued: " + name);
				}
			for (auto e : py::reinterpret_borrow<py::sequence>(val))
				{
				if (type == ANT_attribute_schema::TYPE_INT64)
					{
					if (!py::isinstance<py::int_>(e))
						{
						delete set;
						throw py::type_error("int64 field needs int values: " + name);
						}
					set->add_int(fi, py::cast<long long>(e));
					}
				else	/* TYPE_STRING */
					{
					if (!py::isinstance<py::str>(e))
						{
						delete set;
						throw py::type_error("string field needs str values: " + name);
						}
					set->add_string(fi, py::cast<std::string>(e).c_str());
					}
				}
			}
		else
			{
			if (type == ANT_attribute_schema::TYPE_INT64)
				{
				if (!py::isinstance<py::int_>(val) || py::isinstance<py::bool_>(val))
					{
					delete set;
					throw py::type_error("int64 field needs an int: " + name);
					}
				set->set_int(fi, py::cast<long long>(val));
				}
			else if (type == ANT_attribute_schema::TYPE_STRING)
				{
				if (!py::isinstance<py::str>(val))
					{
					delete set;
					throw py::type_error("string field needs a str: " + name);
					}
				set->set_string(fi, py::cast<std::string>(val).c_str());
				}
			else	/* TYPE_BOOL */
				{
				if (!py::isinstance<py::bool_>(val))
					{
					delete set;
					throw py::type_error("bool field needs a bool: " + name);
					}
				set->set_bool(fi, py::cast<bool>(val) ? 1 : 0);
				}
			}
		}
	}
if (has_payload)
	{
	if (py::isinstance<py::bytes>(payload))
		{
		std::string s = py::cast<std::string>(payload);
		set->set_payload(s.data(), (long long)s.size());
		}
	else if (py::isinstance<py::str>(payload))
		{
		std::string s = py::cast<std::string>(payload);
		set->set_payload(s.data(), (long long)s.size());
		}
	else
		{
		delete set;
		throw py::type_error("payload must be bytes or str");
		}
	}
return set;
}

/*
	HIT
	----
	Plain-data result row returned by search(); mirrors hits_to_array in the
	Node addon (nodejs/addon/segment_index.cpp ~line 930).
*/
struct Hit
{
	std::string key;
	double score;
	long long generation;
	long long docid;
	py::object payload;   // py::bytes when present, else py::none()
};

/*
	PYSEGMENTINDEX
	---------------
	pybind11-facing wrapper around ATIRE_segment_index, mirroring the Node-API
	binding (nodejs/addon/segment_index.cpp): options are captured at
	construction (kwargs), validated eagerly where the Node ctor validates
	eagerly (dimension/metric, attribute schema), and applied to a freshly
	constructed engine in open(), in the exact order the Node Open() applies
	them (pre-open fatal setters, then engine->open(), then post-open
	non-fatal setters).
*/
struct PySegmentIndex
{
	ATIRE_segment_index *engine;

	/* options captured at construction, applied at open() -- same shape and
	   defaults as SegmentIndexWrap's option_* members in the Node binding. */
	long long option_dimension;
	long option_metric;
	long long option_flush_threshold;	// -1 = leave engine default
	long option_merge_factor;			// <= 0 = not requested
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
	ANT_attribute_schema option_attributes;
	bool option_has_attributes;

	/*
		PYSEGMENTINDEX::PYSEGMENTINDEX()
		----------------------------------
		Parses **kwargs into the option_* members; validates dimension/metric
		and the attribute schema eagerly (same eagerness as the Node ctor) so
		bad configuration fails at construction rather than at open().
		Unknown kwargs are silently ignored (forgiving, per the Node ctor's
		Options.Has()-gated style).
	*/
	explicit PySegmentIndex(py::kwargs kw)
	{
	engine = NULL;
	option_dimension = 0;
	option_metric = ATIRE_segment_index::VECTOR_METRIC_DOT;
	option_flush_threshold = -1;
	option_merge_factor = -1;
	option_tombstone_ratio = -1.0;
	option_auto_maintain = 0;
	option_durable = 0;
	option_wal_fsync = 0;
	option_global_stats = 1;
	option_approx_bits = -1;
	option_approx_multiplier = 0;
	option_hnsw_M = -1;
	option_hnsw_ef_construction = 0;
	option_hnsw_ef_search = 0;
	option_quantize = ATIRE_segment_index::QUANTIZE_OFF;
	option_rerank_dim = 0;
	option_rerank_quant = ATIRE_segment_index::RERANK_QUANT_INT8;
	option_has_attributes = false;

	if (kw.contains("dimension"))
		{
		option_dimension = kw["dimension"].cast<long long>();
		if (option_dimension < 1 || option_dimension > 65536)
			throw std::invalid_argument("dimension must be between 1 and 65536");
		}
	if (kw.contains("metric"))
		{
		std::string metric = kw["metric"].cast<std::string>();
		if (metric == "dot")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_DOT;
		else if (metric == "cosine")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_COSINE;
		else if (metric == "l2")
			option_metric = ATIRE_segment_index::VECTOR_METRIC_L2;
		else
			throw std::invalid_argument("metric must be 'dot', 'cosine' or 'l2'");
		}
	if (kw.contains("flush_threshold"))
		option_flush_threshold = kw["flush_threshold"].cast<long long>();
	if (kw.contains("merge_factor"))
		option_merge_factor = kw["merge_factor"].cast<long>();
	if (kw.contains("tombstone_ratio"))
		option_tombstone_ratio = kw["tombstone_ratio"].cast<double>();
	if (kw.contains("auto_maintain"))
		option_auto_maintain = kw["auto_maintain"].cast<bool>() ? 1 : 0;
	if (kw.contains("durable"))
		option_durable = kw["durable"].cast<bool>() ? 1 : 0;
	if (kw.contains("wal_fsync"))
		option_wal_fsync = kw["wal_fsync"].cast<bool>() ? 1 : 0;
	if (kw.contains("global_stats"))
		option_global_stats = kw["global_stats"].cast<bool>() ? 1 : 0;
	if (kw.contains("approximate") && !kw["approximate"].is_none())
		{
		py::dict approx = kw["approximate"].cast<py::dict>();
		option_approx_bits = approx.contains("bits") ? approx["bits"].cast<long>() : 0;	// 0 => engine default 256
		if (approx.contains("multiplier"))
			option_approx_multiplier = approx["multiplier"].cast<long>();
		}
	if (kw.contains("hnsw") && !kw["hnsw"].is_none())
		{
		py::dict h = kw["hnsw"].cast<py::dict>();
		option_hnsw_M = h.contains("M") ? h["M"].cast<long>() : 0;	// 0 => engine default 16
		if (h.contains("ef_construction"))
			option_hnsw_ef_construction = h["ef_construction"].cast<long>();
		if (h.contains("ef_search"))
			option_hnsw_ef_search = h["ef_search"].cast<long>();
		}
	if (kw.contains("quantize") && !kw["quantize"].is_none())
		{
		py::object qv = kw["quantize"];
		std::string mode;
		if (py::isinstance<py::str>(qv))
			mode = qv.cast<std::string>();
		else if (py::isinstance<py::dict>(qv))
			{
			py::dict qo = qv.cast<py::dict>();
			mode = qo.contains("mode") ? qo["mode"].cast<std::string>() : std::string("replace");
			}
		else
			throw std::invalid_argument("quantize must be 'int8'/'replace'/'exact' or { mode }");
		if (mode == "int8" || mode == "replace")
			option_quantize = ATIRE_segment_index::QUANTIZE_REPLACE;
		else if (mode == "exact")
			option_quantize = ATIRE_segment_index::QUANTIZE_EXACT;
		else
			throw std::invalid_argument("quantize mode must be 'int8', 'replace', or 'exact'");
		}
	if (kw.contains("rerank") && !kw["rerank"].is_none())
		{
		py::dict r = kw["rerank"].cast<py::dict>();
		option_rerank_dim = r.contains("dimension") ? r["dimension"].cast<long long>() : 0;
		if (r.contains("quantize"))
			{
			std::string q = r["quantize"].cast<std::string>();
			option_rerank_quant = (q == "float") ? ATIRE_segment_index::RERANK_QUANT_FLOAT : ATIRE_segment_index::RERANK_QUANT_INT8;
			}
		}
	if (kw.contains("attributes") && !kw["attributes"].is_none())
		{
		py::dict attrs = kw["attributes"].cast<py::dict>();
		for (auto item : attrs)
			{
			std::string name = py::str(item.first);
			std::string spec = py::str(item.second);
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
				throw std::invalid_argument("attribute type must be 'int64', 'string', 'bool' (with optional '[]')");
			if (option_attributes.add_field(name.c_str(), type, multi) != 0)
				throw std::invalid_argument("invalid attribute field (duplicate name, too many fields, name too long, or bool[] not allowed)");
			}
		option_has_attributes = true;
		}
	}

	~PySegmentIndex() { delete engine; }

	/*
		PYSEGMENTINDEX::REQUIRE_OPEN()
		--------------------------------
	*/
	void require_open()
	{
	if (engine == NULL)
		throw std::runtime_error("index is not open");
	}

	/*
		PYSEGMENTINDEX::OPEN()
		------------------------
		Ported verbatim (option-application order) from SegmentIndexWrap::Open()
		in the Node binding: pre-open setters are fatal on failure (the engine
		is torn down and the exception propagates), post-open setters are
		non-fatal (the corresponding feature simply stays off).
	*/
	void open(const std::string &directory)
	{
	if (engine != NULL)
		throw std::runtime_error("index is already open");

	engine = new ATIRE_segment_index();
	if (option_dimension > 0 && engine->set_vector_config(option_dimension, option_metric) != 0)
		{
		delete engine;
		engine = NULL;
		throw std::runtime_error("invalid vector configuration");
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
		throw std::runtime_error("set_durable failed: index is already open");
		}
	if (option_wal_fsync)
		engine->set_wal_fsync(1);
	if (!option_global_stats)
		engine->set_global_stats(0);

	{
	py::gil_scoped_release release;
	if (engine->open(directory.c_str()) != 0)
		{
		delete engine;
		engine = NULL;
		}
	}
	if (engine == NULL)
		throw std::runtime_error("open failed: bad directory, corrupt index, or vector config mismatch");

	/* post-open, all non-fatal */
	if (option_approx_bits >= 0)
		{
		engine->set_approximate_config(option_approx_bits);
		if (option_approx_multiplier > 0)
			engine->set_candidate_multiplier(option_approx_multiplier);
		}
	if (option_hnsw_M >= 0)
		{
		engine->set_hnsw_config(option_hnsw_M, option_hnsw_ef_construction);
		if (option_hnsw_ef_search > 0)
			engine->set_ef_search(option_hnsw_ef_search);
		}
	if (option_quantize != ATIRE_segment_index::QUANTIZE_OFF)
		engine->set_quantization(option_quantize);
	if (option_rerank_dim > 0)
		engine->set_rerank_config(option_rerank_dim, option_rerank_quant);
	if (option_has_attributes)
		engine->set_attributes_config(option_attributes);
	}

	/*
		PYSEGMENTINDEX::CLOSE()
		-------------------------
		Idempotent: safe to call when already closed (engine == NULL).
	*/
	void close()
	{
	delete engine;
	engine = NULL;
	}

	long long document_count()
	{
	require_open();
	return engine->get_document_count();
	}

	long long vector_dimension()
	{
	require_open();
	return engine->vector_dimension();
	}

	/*
		PYSEGMENTINDEX::HITS_TO_LIST()
		---------------------------------
		Builds the Python-visible list of Hit objects from the engine's
		current result buffer. Caller must hold the GIL (this constructs
		Python objects).
	*/
	py::list hits_to_list(long long count)
	{
	py::list out;
	for (long long i = 0; i < count; i++)
		{
		ATIRE_segment_index::hit *h = engine->get_hit(i);
		Hit hit;
		hit.key = (h->filename != NULL) ? h->filename : "";
		hit.score = h->score;
		hit.generation = h->generation;
		hit.docid = h->docid;
		hit.payload = (h->payload != NULL && h->payload_length > 0)
			? py::object(py::bytes(reinterpret_cast<const char*>(h->payload), (size_t)h->payload_length))
			: py::object(py::none());
		out.append(hit);
		}
	return out;
	}

	/*
		PYSEGMENTINDEX::ADD_DOCUMENT()
		---------------------------------
		Lexical-only when vector is None; otherwise converts vector via
		extract_vector(). When multi_vectors is also given, flattens it via
		extract_multivectors() and calls the 5-arg engine overload; otherwise
		calls the 3-arg (vector-only) or 2-arg (lexical-only) overload. Rejects
		zero vectors under the cosine metric before it reaches the engine
		(mirrors the Node AddDocument zero-vector rejection, segment_index.cpp
		~529-540). When `attributes` and/or `payload` are given,
		build_attribute_set() constructs an ANT_attribute_set and the 6-arg
		engine overload is used instead.
	*/
	py::dict add_document(const std::string &key, const std::string &text, py::object vector = py::none(), py::object multi_vectors = py::none(), py::object attributes = py::none(), py::object payload = py::none())
	{
	require_open();
	std::vector<float> vec;
	const float *vptr = NULL;
	if (!vector.is_none())
		{
		vec = extract_vector(vector, engine->vector_dimension());
		if (option_metric == ATIRE_segment_index::VECTOR_METRIC_COSINE)
			{
			double norm = 0.0;
			for (float f : vec)
				norm += (double)f * f;
			if (norm == 0.0)
				throw py::value_error("zero vector is not valid under the cosine metric");
			}
		vptr = vec.data();
		}
	ANT_attribute_set *set = build_attribute_set(engine, attributes, payload);	// NULL if neither given; throws (and self-frees) on error
	long long handle;
	if (set != NULL)
		{
		long long num_mv = 0;
		const float *mvptr = NULL;
		std::vector<float> mv;
		if (!multi_vectors.is_none())
			{
			mv = extract_multivectors(multi_vectors, engine->rerank_dimension(), &num_mv);
			mvptr = num_mv > 0 ? mv.data() : NULL;
			}
		handle = engine->add_document(key.c_str(), text.c_str(), vptr, mvptr, num_mv, set);
		delete set;
		}
	else if (!multi_vectors.is_none())
		{
		long long num_mv = 0;
		std::vector<float> mv = extract_multivectors(multi_vectors, engine->rerank_dimension(), &num_mv);
		handle = engine->add_document(key.c_str(), text.c_str(), vptr, num_mv > 0 ? mv.data() : NULL, num_mv);
		}
	else if (vptr != NULL)
		handle = engine->add_document(key.c_str(), text.c_str(), vptr);
	else
		handle = engine->add_document(key.c_str(), text.c_str());
	if (handle == -1)
		throw std::runtime_error("add_document failed (empty document or index not writable)");
	py::dict d;
	d["generation"] = handle >> 40;
	d["docid"] = handle & ((1LL << 40) - 1);
	return d;
	}

	/*
		PYSEGMENTINDEX::SEARCH()
		----------------------------
		Lexical search. The engine mutates the query buffer in place, so a
		writable copy is passed rather than the const std::string's data().
		The GIL is released only around the engine call.
	*/
	py::list search(const std::string &text, long long k)
	{
	require_open();
	if (k < 1)
		return py::list();
	std::string buf = text;
	long long count;
	{
	py::gil_scoped_release release;
	count = engine->search(&buf[0], k);
	}
	return hits_to_list(count);
	}

	/*
		PYSEGMENTINDEX::SEARCH_VECTOR()
		-----------------------------------
	*/
	py::list search_vector(py::object vector, long long k)
	{
	require_open();
	if (k < 1)
		return py::list();
	std::vector<float> vec = extract_vector(vector, engine->vector_dimension());
	long long count;
	{
	py::gil_scoped_release release;
	count = engine->search_vector(vec.data(), k);
	}
	return hits_to_list(count);
	}

	/*
		PYSEGMENTINDEX::SEARCH_HYBRID()
		-----------------------------------
		RRF fusion of lexical + vector top-k. Both the query text and the
		vector are converted to engine-owned buffers before the GIL is
		released, matching search()'s writable-copy pattern.
	*/
	py::list search_hybrid(const std::string &text, py::object vector, long long k)
	{
	require_open();
	if (k < 1)
		return py::list();
	std::string buf = text;
	std::vector<float> vec = extract_vector(vector, engine->vector_dimension());
	long long count;
	{
	py::gil_scoped_release release;
	count = engine->search_hybrid(&buf[0], vec.data(), k);
	}
	return hits_to_list(count);
	}

	/*
		PYSEGMENTINDEX::SEARCH_VECTOR_APPROX()
		-----------------------------------------
		Signature-prefiltered top-k (transparently falls back to exact when
		approximate search is unconfigured or the metric is incompatible).
	*/
	py::list search_vector_approx(py::object vector, long long k)
	{
	require_open();
	if (k < 1)
		return py::list();
	std::vector<float> vec = extract_vector(vector, engine->vector_dimension());
	long long count;
	{
	py::gil_scoped_release release;
	count = engine->search_vector_approx(vec.data(), k);
	}
	return hits_to_list(count);
	}

	/*
		PYSEGMENTINDEX::SEARCH_VECTOR_HNSW()
		---------------------------------------
	*/
	py::list search_vector_hnsw(py::object vector, long long k)
	{
	require_open();
	if (k < 1)
		return py::list();
	std::vector<float> vec = extract_vector(vector, engine->vector_dimension());
	long long count;
	{
	py::gil_scoped_release release;
	count = engine->search_vector_hnsw(vec.data(), k);
	}
	return hits_to_list(count);
	}

	/*
		PYSEGMENTINDEX::SEARCH_HYBRID_APPROX()
		-----------------------------------------
	*/
	py::list search_hybrid_approx(const std::string &text, py::object vector, long long k)
	{
	require_open();
	if (k < 1)
		return py::list();
	std::string buf = text;
	std::vector<float> vec = extract_vector(vector, engine->vector_dimension());
	long long count;
	{
	py::gil_scoped_release release;
	count = engine->search_hybrid_approx(&buf[0], vec.data(), k);
	}
	return hits_to_list(count);
	}

	/*
		PYSEGMENTINDEX::SEARCH_HYBRID_HNSW()
		---------------------------------------
	*/
	py::list search_hybrid_hnsw(const std::string &text, py::object vector, long long k)
	{
	require_open();
	if (k < 1)
		return py::list();
	std::string buf = text;
	std::vector<float> vec = extract_vector(vector, engine->vector_dimension());
	long long count;
	{
	py::gil_scoped_release release;
	count = engine->search_hybrid_hnsw(&buf[0], vec.data(), k);
	}
	return hits_to_list(count);
	}

	/*
		PYSEGMENTINDEX::FLUSH()
		--------------------------
		Minimal binding pulled forward from Task 9 (the mutation-surface task)
		because this task's tests need a way to force the in-memory segment to
		disk before the backfill builders (build_signatures/build_hnsw) and the
		approximate/HNSW search paths -- which operate on disk segments -- have
		anything to work with.
	*/
	void flush()
	{
	require_open();
	long rc;
	{
	py::gil_scoped_release release;
	rc = engine->flush();
	}
	if (rc != 0)
		throw std::runtime_error("flush failed");
	}

	/*
		PYSEGMENTINDEX::BUILD_SIGNATURES()
		--------------------------------------
		Idempotent backfill: writes a .vsig sidecar for every disk segment that
		has vectors but no valid signature sidecar yet.
	*/
	void build_signatures()
	{
	require_open();
	long rc;
	{
	py::gil_scoped_release release;
	rc = engine->build_signatures();
	}
	if (rc != 0)
		throw std::runtime_error("build_signatures failed");
	}

	/*
		PYSEGMENTINDEX::BUILD_HNSW()
		---------------------------------
		Idempotent backfill: builds the .hnsw graph sidecar for disk segments
		that have vectors but no valid HNSW sidecar yet.
	*/
	void build_hnsw()
	{
	require_open();
	long rc;
	{
	py::gil_scoped_release release;
	rc = engine->build_hnsw();
	}
	if (rc != 0)
		throw std::runtime_error("build_hnsw failed");
	}

	/*
		PYSEGMENTINDEX::SEARCH_RERANK()
		-----------------------------------
		Stage 1 (lexical/vector/hybrid, whichever of text/vector are given) ->
		MaxSim rerank of the top first_stage_n candidates over multi-vectors,
		publishing top_k. text and vector may each be None, but not both (the
		Node binding guards this same case to avoid a search(NULL) SIGSEGV in
		the engine). No filter parameter yet (Task 8).
	*/
	py::list search_rerank(py::object text, py::object vector, py::object query_multi_vectors, long long first_stage_n, long long k)
	{
	require_open();
	if (k < 1)
		return py::list();
	if (text.is_none() && vector.is_none())
		throw py::value_error("search_rerank requires text and/or vector");
	std::string buf;
	char *tptr = NULL;
	if (!text.is_none())
		{
		buf = text.cast<std::string>();
		tptr = &buf[0];
		}
	std::vector<float> vec;
	const float *vptr = NULL;
	if (!vector.is_none())
		{
		vec = extract_vector(vector, engine->vector_dimension());
		vptr = vec.data();
		}
	long long num_qv = 0;
	std::vector<float> qmv = extract_multivectors(query_multi_vectors, engine->rerank_dimension(), &num_qv);
	const float *qmvptr = num_qv > 0 ? qmv.data() : NULL;
	long long count;
	{
	py::gil_scoped_release release;
	count = engine->search_rerank(tptr, vptr, qmvptr, num_qv, first_stage_n, k);
	}
	return hits_to_list(count);
	}

	/*
		PYSEGMENTINDEX::BUILD_QUANTIZED()
		--------------------------------------
		Idempotent backfill: rewrites each float .vec disk segment as an int8
		.qvec (replace mode).
	*/
	void build_quantized()
	{
	require_open();
	long rc;
	{
	py::gil_scoped_release release;
	rc = engine->build_quantized();
	}
	if (rc != 0)
		throw std::runtime_error("build_quantized failed");
	}
};

PYBIND11_MODULE(_core, m)
{
	m.attr("__doc__") = "Antelope engine binding (pybind11)";
	m.def("_link_check", []() {
		ATIRE_segment_index ix;   // must construct+destruct -> archive symbols must link
		(void)ix;
		return true;
	});

	py::class_<Hit>(m, "Hit")
		.def_readonly("key", &Hit::key)
		.def_readonly("score", &Hit::score)
		.def_readonly("generation", &Hit::generation)
		.def_readonly("docid", &Hit::docid)
		.def_readonly("payload", &Hit::payload)
		.def("__repr__", [](const Hit &h){ return std::string("<Hit key=") + h.key + ">"; });

	py::class_<PySegmentIndex>(m, "SegmentIndex")
		.def(py::init([](py::kwargs kw) { return new PySegmentIndex(kw); }))
		.def("open", &PySegmentIndex::open, py::arg("directory"))
		.def("close", &PySegmentIndex::close)
		.def("document_count", &PySegmentIndex::document_count)
		.def("vector_dimension", &PySegmentIndex::vector_dimension)
		.def("add_document", &PySegmentIndex::add_document, py::arg("key"), py::arg("text"), py::arg("vector") = py::none(), py::arg("multi_vectors") = py::none(), py::arg("attributes") = py::none(), py::arg("payload") = py::none())
		.def("search", &PySegmentIndex::search, py::arg("text"), py::arg("k"))
		.def("search_vector", &PySegmentIndex::search_vector, py::arg("vector"), py::arg("k"))
		.def("search_hybrid", &PySegmentIndex::search_hybrid, py::arg("text"), py::arg("vector"), py::arg("k"))
		.def("search_vector_approx", &PySegmentIndex::search_vector_approx, py::arg("vector"), py::arg("k"))
		.def("search_vector_hnsw", &PySegmentIndex::search_vector_hnsw, py::arg("vector"), py::arg("k"))
		.def("search_hybrid_approx", &PySegmentIndex::search_hybrid_approx, py::arg("text"), py::arg("vector"), py::arg("k"))
		.def("search_hybrid_hnsw", &PySegmentIndex::search_hybrid_hnsw, py::arg("text"), py::arg("vector"), py::arg("k"))
		.def("flush", &PySegmentIndex::flush)
		.def("build_signatures", &PySegmentIndex::build_signatures)
		.def("build_hnsw", &PySegmentIndex::build_hnsw)
		.def("search_rerank", &PySegmentIndex::search_rerank, py::arg("text"), py::arg("vector"), py::arg("query_multi_vectors"), py::arg("first_stage_n"), py::arg("k"))
		.def("build_quantized", &PySegmentIndex::build_quantized)
		.def("__enter__", [](PySegmentIndex &s) -> PySegmentIndex & { return s; }, py::return_value_policy::reference)
		.def("__exit__", [](PySegmentIndex &s, py::object, py::object, py::object) { s.close(); return false; });
}

/*
	ATIRE_SEGMENT_INDEX_VECTOR.CPP
	------------------------------
	Per-document vectors, vector search, and hybrid (RRF) search.  Part of
	ATIRE_segment_index, whose implementation is split across
	atire_segment_index*.cpp by feature (see
	docs/superpowers/specs/2026-07-06-segment-index-file-split-design.md).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

#include "atire_segment_index.h"
#include "atire_api.h"
#include "indexer.h"
#include "../source/index_manifest.h"
#include "../source/index_keymap.h"
#include "../source/index_tombstones.h"
#include "../source/search_engine.h"
#include "../source/search_engine_result.h"
#include "../source/search_engine_accumulator.h"
#include "../source/version.h"
#include "../source/index_merge.h"
#include "../source/vector_store.h"
#include "../source/wal.h"
#include "../source/signature.h"
#include "../source/signature_store.h"

/*
	ATIRE_SEGMENT_INDEX::RESET_WRITER_VECTORS()
	--------------------------------------------
	Frees the memory-segment vector buffer and zeroes its bookkeeping.  Called
	from start_new_writer() (a fresh segment starts with a fresh, empty
	buffer -- docids are local to the segment) and from the destructor.  The
	vector.config state (dimension/metric) is index-wide, not per-segment, and
	is untouched here.
*/
void ATIRE_segment_index::reset_writer_vectors(void)
{
delete [] writer_vector_data;
delete [] writer_vector_presence;
writer_vector_data = NULL;
writer_vector_presence = NULL;
writer_vector_capacity = 0;
writer_vectors_present = 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_VECTOR_CONFIG()
	----------------------------------------
	Must be called before open().  The configuration is fixed for the life of
	the index; open() writes it to <dir>/vector.config on first use and
	verifies it against an existing file on every later use.
*/
long ATIRE_segment_index::set_vector_config(long long dimension, long metric)
{
if (directory != NULL)
	return 1;			// already open
if (dimension < 1 || dimension > 65536)
	return 1;
if (metric != VECTOR_METRIC_DOT && metric != VECTOR_METRIC_COSINE && metric != VECTOR_METRIC_L2)
	return 1;
pending_vector_dimension = dimension;
pending_vector_metric = metric;
vector_config_pending = 1;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_VECTOR_CONFIG()
	-----------------------------------------
	Reads <dir>/vector.config (two lines: dimension, metric).  Absent file
	leaves vectors disabled.  Garbage is treated as absent (defensive parsing
	per house convention).
*/
long ATIRE_segment_index::load_vector_config(void)
{
char filename[4096], line[64];
FILE *fp;
long long dimension = 0;
long metric = -1;

snprintf(filename, sizeof(filename), "%s/vector.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fgets(line, sizeof(line), fp) != NULL)
	dimension = atoll(line);
if (fgets(line, sizeof(line), fp) != NULL)
	metric = atol(line);
fclose(fp);
if (dimension < 1 || dimension > 65536 || metric < VECTOR_METRIC_DOT || metric > VECTOR_METRIC_L2)
	return 0;			// corrupt: treat as absent
vector_dimension_current = dimension;
vector_metric = metric;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_VECTOR_CONFIG()
	-----------------------------------------
*/
long ATIRE_segment_index::save_vector_config(void)
{
char filename[4096], temp_name[4200];
FILE *fp;

snprintf(filename, sizeof(filename), "%s/vector.config", directory);
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
fprintf(fp, "%lld\n%ld\n", vector_dimension_current, vector_metric);
fclose(fp);
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	return 1;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_SIGNATURE_CONFIG()
	--------------------------------------------
	Reads <dir>/signature.config (magic/version/bits/seed).  Absent => approximate
	stays unconfigured.  Garbage => treated as absent (defensive parse).
*/
long ATIRE_segment_index::load_signature_config(void)
{
char filename[4096];
FILE *fp;
unsigned long long magic, seed;
unsigned int version;
long long bits;

snprintf(filename, sizeof(filename), "%s/signature.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != 0x3130474953544E41ULL
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != 1u
	|| fread(&bits, sizeof(bits), 1, fp) != 1
	|| fread(&seed, sizeof(seed), 1, fp) != 1
	|| bits < 8 || bits > 65536 || (bits % 8) != 0)
	{ fclose(fp); return 0; }
fclose(fp);
signature_bits_current = bits;
signature_seed = seed;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_SIGNATURE_CONFIG()
	--------------------------------------------
	Atomic write (temp + rename) of the index-wide signature config.
*/
long ATIRE_segment_index::save_signature_config(void)
{
char filename[4096], temp_name[4200];
FILE *fp;
unsigned long long magic = 0x3130474953544E41ULL, seed = signature_seed;
unsigned int version = 1u;
long long bits = signature_bits_current;

snprintf(filename, sizeof(filename), "%s/signature.config", directory);
if (snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fp = fopen(temp_name, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&bits, sizeof(bits), 1, fp) != 1 || fwrite(&seed, sizeof(seed), 1, fp) != 1)
	{ fclose(fp); remove(temp_name); return 1; }
fclose(fp);
if (rename(temp_name, filename) != 0)
	{ remove(temp_name); return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::REBUILD_QUERY_SIGNER()
	-------------------------------------------
	(Re)materialize the index-wide projection in RAM from the loaded config.
	No-op when approximate is unconfigured or vectors are disabled.
*/
void ATIRE_segment_index::rebuild_query_signer(void)
{
delete query_signer;
query_signer = NULL;
if (signature_bits_current != 0 && vector_dimension_current != 0)
	query_signer = new ANT_signature(vector_dimension_current, signature_bits_current, signature_seed);
}

/*
	ATIRE_SEGMENT_INDEX::SET_APPROXIMATE_CONFIG()
	---------------------------------------------
	Enable the signature prefilter.  Requires the index open with vectors enabled.
	First enable picks bits (default 256) + a projection seed and persists them;
	the config is immutable thereafter (a second call is a no-op success).
*/
long ATIRE_segment_index::set_approximate_config(long long bits)
{
if (directory == NULL)
	return 1;					// must be open (needs directory + dimension)
if (vector_dimension_current == 0)
	return 1;					// approximate requires vectors enabled
if (signature_bits_current != 0)
	return 0;					// already configured; immutable
if (bits <= 0)
	bits = 256;
if (bits > 65536 || (bits % 8) != 0)
	return 1;
signature_bits_current = bits;
signature_seed = (unsigned long long)time(NULL) ^ ((unsigned long long)bits << 32) ^ 0x9E3779B97F4A7C15ULL;
if (save_signature_config() != 0)
	{ signature_bits_current = 0; signature_seed = 0; return 1; }
rebuild_query_signer();
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_HNSW_CONFIG()
	----------------------------------------
	Reads <dir>/hnsw.config (magic/version/M/ef_construction).  Absent => HNSW
	stays unconfigured.  Garbage => treated as absent (defensive parse, mirrors
	load_signature_config).
*/
long ATIRE_segment_index::load_hnsw_config(void)
{
char filename[4096];
FILE *fp;
unsigned long long magic, want;
unsigned int version;
long long M, efc;
const char *tag = "ANTHNSW1";

memcpy(&want, tag, 8);
snprintf(filename, sizeof(filename), "%s/hnsw.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != want
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != 1u
	|| fread(&M, sizeof(M), 1, fp) != 1 || fread(&efc, sizeof(efc), 1, fp) != 1
	|| M < 2 || M > 4096 || efc < 1 || efc > 100000)
	{ fclose(fp); return 0; }
fclose(fp);
hnsw_M_current = M;
hnsw_ef_construction_current = efc;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_HNSW_CONFIG()
	------------------------------------------
	Atomic write (temp + rename) of the index-wide HNSW config.
*/
long ATIRE_segment_index::save_hnsw_config(void)
{
char filename[4096], temp[4200];
FILE *fp;
unsigned long long magic;
unsigned int version = 1u;
long long M = hnsw_M_current, efc = hnsw_ef_construction_current;
const char *tag = "ANTHNSW1";

memcpy(&magic, tag, 8);
snprintf(filename, sizeof(filename), "%s/hnsw.config", directory);
if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp))
	return 1;
if ((fp = fopen(temp, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&M, sizeof(M), 1, fp) != 1 || fwrite(&efc, sizeof(efc), 1, fp) != 1)
	{ fclose(fp); remove(temp); return 1; }
fclose(fp);
if (rename(temp, filename) != 0)
	{ remove(temp); return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_HNSW_CONFIG()
	-----------------------------------------
	Enable the HNSW graph index.  Requires the index open with vectors
	enabled.  First enable picks M (default 16) + ef_construction (default
	200) and persists them; the config is immutable thereafter (a second
	call is a no-op success).

	M < 2 is clamped to 16 (NOT just M <= 0): ANT_hnsw::build() internally
	clamps M < 2 => 16 (M == 1 makes mL = 1/ln(1) = inf, which crashes the
	build).  If this setter persisted M=1 to hnsw.config while build() will
	actually use M=16, ANT_hnsw::load(..., expected_M=1, ...) would mismatch
	the graph's real stored M=16 and silently degrade every segment to an
	empty result.  Clamping identically here keeps the persisted config in
	lockstep with what build() will actually construct.
*/
long ATIRE_segment_index::set_hnsw_config(long long M, long long ef_construction)
{
if (directory == NULL)
	return 1;					// must be open (needs directory + dimension)
if (vector_dimension_current == 0)
	return 1;					// HNSW requires vectors enabled
if (hnsw_M_current != 0)
	return 0;					// already configured; immutable
if (M < 2)
	M = 16;						// matches ANT_hnsw::build()'s M>=2 clamp; keeps config == built graph
if (ef_construction <= 0)
	ef_construction = 200;
if (M > 4096 || ef_construction > 100000)
	return 1;
hnsw_M_current = M;
hnsw_ef_construction_current = ef_construction;
if (save_hnsw_config() != 0)
	{ hnsw_M_current = 0; hnsw_ef_construction_current = 0; return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_EF_SEARCH()
	----------------------------------------
	Query-time knob; clamps to >= 1.  May be called before or after open().
*/
void ATIRE_segment_index::set_ef_search(long long ef)
{
hnsw_ef_search = ef < 1 ? 1 : ef;
}

/*
	ATIRE_SEGMENT_INDEX::SET_CANDIDATE_MULTIPLIER()
	-----------------------------------------------
*/
void ATIRE_segment_index::set_candidate_multiplier(long long n)
{
candidate_multiplier = n < 1 ? 1 : n;
}

/*
	ATIRE_SEGMENT_INDEX::BUILD_SIGNATURES()
	---------------------------------------
	Idempotent backfill: for every manifested disk segment with vectors but no
	valid .vsig, sign its dense vectors into a fresh sidecar (loaded on next
	open()).  Per-segment failures are skipped (that segment stays
	signature-less / exact-scanned), never left corrupt.  Returns 0 on success
	(1 if approximate is unconfigured).
*/
long ATIRE_segment_index::build_signatures(void)
{
long long which, docid;
char vec_name[4096], vsig_name[4096];

if (signature_bits_current == 0 || query_signer == NULL)
	return 1;

for (which = 0; which < segment_count; which++)
	{
	long long generation = segments[which].generation;
	long long docs = segments[which].engine->get_document_count();

	segment_filename(vsig_name, sizeof(vsig_name), generation, "vsig");
	ANT_signature_store *existing = ANT_signature_store::load(vsig_name, signature_bits_current, docs);
	long long already = existing->document_count() == docs && docs > 0;
	delete existing;
	if (already)
		continue;

	segment_filename(vec_name, sizeof(vec_name), generation, "vec");
	ANT_vector_store *vectors = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
	if (vectors->document_count() != docs)
		{ delete vectors; continue; }

	ANT_signature_store_writer sig_writer;
	unsigned char *sig = new unsigned char[query_signer->signature_bytes()];
	long failed = sig_writer.create(vsig_name, signature_bits_current) != 0;
	for (docid = 0; !failed && docid < docs; docid++)
		{
		if (vectors->has(docid))
			{ query_signer->sign(vectors->get(docid), sig); failed = sig_writer.append(sig) != 0; }
		else
			failed = sig_writer.append(NULL) != 0;
		}
	if (!failed) sig_writer.finish(); else sig_writer.abandon();
	delete [] sig;
	delete vectors;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::WRITER_VECTOR_APPEND()
	-------------------------------------------
	Keeps the vector buffer parallel to the writer's docids: called exactly
	once per successfully indexed document (NULL for lexical-only docs).  The
	docid is passed explicitly by the caller (add_document_core(), before
	writer_documents is incremented) rather than derived from writer_documents
	here, so the ordering of the buffer append relative to that increment is
	the caller's concern, not this function's.  Cosine-mode normalization
	happens in the caller (add_document_core()) before this is reached.
*/
long ATIRE_segment_index::writer_vector_append(long long docid, const float *vector_or_null)
{
if (writer_vector_capacity == 0)
	{
	writer_vector_capacity = 1024;
	writer_vector_data = new float[writer_vector_capacity * vector_dimension_current];
	writer_vector_presence = new unsigned char[(writer_vector_capacity + 7) / 8];
	memset(writer_vector_presence, 0, (size_t)((writer_vector_capacity + 7) / 8));
	}
if (docid >= writer_vector_capacity)
	{
	long long new_capacity = writer_vector_capacity * 2;
	while (docid >= new_capacity)
		new_capacity *= 2;
	float *new_data = new float[new_capacity * vector_dimension_current];
	unsigned char *new_presence = new unsigned char[(new_capacity + 7) / 8];
	memset(new_presence, 0, (size_t)((new_capacity + 7) / 8));
	memcpy(new_data, writer_vector_data, (size_t)(writer_vector_capacity * vector_dimension_current * sizeof(float)));
	memcpy(new_presence, writer_vector_presence, (size_t)((writer_vector_capacity + 7) / 8));
	delete [] writer_vector_data;
	delete [] writer_vector_presence;
	writer_vector_data = new_data;
	writer_vector_presence = new_presence;
	writer_vector_capacity = new_capacity;
	}
if (vector_or_null == NULL)
	memset(writer_vector_data + docid * vector_dimension_current, 0, (size_t)(vector_dimension_current * sizeof(float)));
else
	{
	memcpy(writer_vector_data + docid * vector_dimension_current, vector_or_null, (size_t)(vector_dimension_current * sizeof(float)));
	writer_vector_presence[docid / 8] |= (unsigned char)(1 << (docid % 8));
	writer_vectors_present++;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES()
	----------------------------------------
	Exact top-k across every disk segment's vector store and the live memory
	buffer.  In cosine mode the query is normalized here (stored vectors were
	normalized at insertion -- see add_document_core()).  Returns the
	candidate count; caller supplies best[top_k].
*/
long long ATIRE_segment_index::vector_candidates(const float *query, long long top_k, ANT_vector_candidate *best)
{
long long which, docid, best_count = 0;
float *normalized = NULL;

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;

if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{
		delete [] normalized;
		return 0;
		}
	query = normalized;
	}

for (which = 0; which < segment_count; which++)
	if (segments[which].vectors != NULL)
		segments[which].vectors->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k);

for (docid = 0; docid < writer_documents; docid++)
	{
	if (writer_vector_presence == NULL || !(writer_vector_presence[docid / 8] & (1 << (docid % 8))))
		continue;
	if (writer_tombstones->is_deleted(docid))
		continue;
	ANT_vector_candidate_insert(best, &best_count, top_k, ANT_vector_store::kernel(query, writer_vector_data + docid * vector_dimension_current, vector_dimension_current, vector_metric), writer_generation, docid);
	}

delete [] normalized;
return best_count;
}

/*
	VECTOR_CANDIDATE_COMPARE()
	---------------------------
	qsort comparator for vector-search results: score descending, ties broken
	by (generation, docid) ascending for deterministic output.
*/
static int vector_candidate_compare(const void *a, const void *b)
{
const ANT_vector_candidate *one = (const ANT_vector_candidate *)a;
const ANT_vector_candidate *two = (const ANT_vector_candidate *)b;

if (one->score > two->score)
	return -1;
if (one->score < two->score)
	return 1;
if (one->generation != two->generation)
	return one->generation < two->generation ? -1 : 1;
return one->docid < two->docid ? -1 : (one->docid == two->docid ? 0 : 1);
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_VECTOR()
	--------------------------------------
	Exact top-k dense-vector search across the live memory buffer and every
	open disk segment's vector store.  Mirrors search()'s results[] contract
	(prior hits' filenames freed at entry) -- results[]/results_count are
	shared with search(); only one of the two result sets exists at a time.
*/
long long ATIRE_segment_index::search_vector(const float *query, long long top_k)
{
char filename_buffer[4096];
long long which, count;
ANT_vector_candidate *best;

reset_results();

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;

best = new ANT_vector_candidate[top_k];
count = vector_candidates(query, top_k, best);
qsort(best, (size_t)count, sizeof(*best), vector_candidate_compare);

for (which = 0; which < count; which++)
	{
	char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));

	hit *slot = append_result();

	slot->generation = best[which].generation;
	slot->docid = best[which].docid;
	slot->score = best[which].score;
	if (filename != NULL)
		{
		slot->filename = new char[strlen(filename) + 1];
		strcpy(slot->filename, filename);
		}
	else
		{
		slot->filename = new char[1];
		slot->filename[0] = '\0';
		}
	}

delete [] best;
return results_count;
}

/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES_APPROX()
	-----------------------------------------------
	Like vector_candidates(), but each disk segment WITH a valid cached signature
	store is Hamming-shortlisted (pool = top_k * candidate_multiplier) and only
	those candidates are exact-reranked.  Segments without signatures, and the
	live memory buffer, are exact-scanned.  Caller guarantees metric != L2 and
	approximate is configured.
*/
long long ATIRE_segment_index::vector_candidates_approx(const float *query, long long top_k, ANT_vector_candidate *best)
{
long long which, docid, best_count = 0, pool_size = top_k * candidate_multiplier;
float *normalized = NULL;
unsigned char *query_sig = new unsigned char[query_signer->signature_bytes()];
long long *pool = new long long[pool_size > 0 ? pool_size : 1];

if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{ delete [] normalized; delete [] query_sig; delete [] pool; return 0; }
	query = normalized;
	}

query_signer->sign(query, query_sig);

for (which = 0; which < segment_count; which++)
	{
	if (segments[which].vectors == NULL)
		continue;
	if (segments[which].signatures != NULL && segments[which].signatures->document_count() == segments[which].engine->get_document_count())
		{
		long long count = 0, p;
		segments[which].signatures->shortlist(query_sig, segments[which].tombstones, pool_size, pool, &count);
		for (p = 0; p < count; p++)
			{
			docid = pool[p];
			if (!segments[which].vectors->has(docid))
				continue;
			ANT_vector_candidate_insert(best, &best_count, top_k,
				ANT_vector_store::kernel(query, segments[which].vectors->get(docid), vector_dimension_current, vector_metric),
				segments[which].generation, docid);
			}
		}
	else
		segments[which].vectors->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k);
	}

for (docid = 0; docid < writer_documents; docid++)		// live memory buffer: always exact (mirrors vector_candidates())
	{
	if (writer_vector_presence == NULL || !(writer_vector_presence[docid / 8] & (1 << (docid % 8))))
		continue;
	if (writer_tombstones->is_deleted(docid))
		continue;
	ANT_vector_candidate_insert(best, &best_count, top_k, ANT_vector_store::kernel(query, writer_vector_data + docid * vector_dimension_current, vector_dimension_current, vector_metric), writer_generation, docid);
	}

delete [] normalized;
delete [] query_sig;
delete [] pool;
return best_count;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_VECTOR_APPROX()
	-------------------------------------------
	Signature-prefiltered dense-vector top-k.  Shares search_vector()'s
	downstream (sort + resolve + publish) verbatim so approximate and exact
	rankings are identical modulo the shortlist; only the candidate gatherer
	differs.  Transparently falls back to the exact path when approximate is
	unconfigured or the metric is L2 (SimHash tracks angular, not Euclidean,
	distance) -- giving byte-identical results in those cases.
*/
long long ATIRE_segment_index::search_vector_approx(const float *query, long long top_k)
{
char filename_buffer[4096];
long long which, count;
ANT_vector_candidate *best;

if (signature_bits_current == 0 || query_signer == NULL || vector_metric == VECTOR_METRIC_L2)
	return search_vector(query, top_k);			// transparent fallback

reset_results();

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;

best = new ANT_vector_candidate[top_k];
count = vector_candidates_approx(query, top_k, best);
qsort(best, (size_t)count, sizeof(*best), vector_candidate_compare);

for (which = 0; which < count; which++)
	{
	char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));

	hit *slot = append_result();

	slot->generation = best[which].generation;
	slot->docid = best[which].docid;
	slot->score = best[which].score;
	if (filename != NULL)
		{
		slot->filename = new char[strlen(filename) + 1];
		strcpy(slot->filename, filename);
		}
	else
		{
		slot->filename = new char[1];
		slot->filename[0] = '\0';
		}
	}

delete [] best;
return results_count;
}

/*
	struct ANT_FUSED_CANDIDATE
	---------------------------
	Bundles the RRF-scored candidate with its filename so the two travel
	together through qsort() (a parallel filename array desynchronizes from
	the candidate array once qsort reorders one but not the other).
*/
struct ANT_fused_candidate
{
ANT_vector_candidate candidate;
char *filename;		// owned; NULL only if not yet resolved (never published that way)
} ;

/*
	ANT_FUSED_CANDIDATE_COMPARE()
	-----------------------------
	qsort comparator for fused candidates: score descending, ties broken by
	(generation, docid) ascending, mirroring vector_candidate_compare().
*/
static int ANT_fused_candidate_compare(const void *a, const void *b)
{
const ANT_fused_candidate *one = (const ANT_fused_candidate *)a;
const ANT_fused_candidate *two = (const ANT_fused_candidate *)b;

if (one->candidate.score > two->candidate.score)
	return -1;
if (one->candidate.score < two->candidate.score)
	return 1;
if (one->candidate.generation != two->candidate.generation)
	return one->candidate.generation < two->candidate.generation ? -1 : 1;
if (one->candidate.docid != two->candidate.docid)
	return one->candidate.docid < two->candidate.docid ? -1 : 1;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_HYBRID()
	------------------------------------
	Reciprocal Rank Fusion of the lexical top-k and the vector top-k:
	fused(d) = sum over lists containing d of 1 / (60 + rank_d), ranks
	1-based.  60 is the standard RRF constant.  Either side may be absent;
	the result degrades to the other side (still RRF-scored, order preserved).

	The candidate and its filename are carried together in a single
	ANT_fused_candidate[] array (see struct above, just up) so that qsort()
	cannot desynchronize them; a parallel filename array would move the
	ANT_vector_candidate rows without moving the corresponding filename rows.
*/
long long ATIRE_segment_index::search_hybrid(char *query_text, const float *query_vector, long long top_k)
{
long long lexical_count = 0, vector_count = 0, fused_count = 0, which, other;
char filename_buffer[4096];

if (top_k < 1)
	return 0;

/*
	Lexical side first: run the existing search and snapshot its hits (the
	results array is shared, so the snapshot must deep-copy the filenames)
	into the fused array before it gets overwritten.
*/
ANT_fused_candidate *fused = new ANT_fused_candidate[top_k * 2];

if (query_text != NULL && *query_text != '\0')
	lexical_count = search(query_text, top_k);
for (which = 0; which < lexical_count; which++)
	{
	fused[fused_count].candidate.generation = results[which].generation;
	fused[fused_count].candidate.docid = results[which].docid;
	fused[fused_count].candidate.score = 1.0 / (60.0 + (double)(which + 1));
	fused[fused_count].filename = new char[strlen(results[which].filename) + 1];
	strcpy(fused[fused_count].filename, results[which].filename);
	fused_count++;
	}

/*
	Vector side: candidates + rank contribution, merged into the fused set
	by (generation, docid) identity.
*/
if (query_vector != NULL && vector_dimension_current != 0)
	{
	ANT_vector_candidate *best = new ANT_vector_candidate[top_k];
	vector_count = vector_candidates(query_vector, top_k, best);
	qsort(best, (size_t)vector_count, sizeof(*best), vector_candidate_compare);
	for (which = 0; which < vector_count; which++)
		{
		double contribution = 1.0 / (60.0 + (double)(which + 1));
		long found = false;
		for (other = 0; other < fused_count; other++)
			if (fused[other].candidate.generation == best[which].generation && fused[other].candidate.docid == best[which].docid)
				{
				fused[other].candidate.score += contribution;
				found = true;
				break;
				}
		if (!found)
			{
			fused[fused_count].candidate.generation = best[which].generation;
			fused[fused_count].candidate.docid = best[which].docid;
			fused[fused_count].candidate.score = contribution;
			char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));
			fused[fused_count].filename = new char[(filename != NULL ? strlen(filename) : 0) + 1];
			strcpy(fused[fused_count].filename, filename != NULL ? filename : "");
			fused_count++;
			}
		}
	delete [] best;
	}

/*
	Sort fused by score desc (ties: generation, docid asc) -- candidate and
	filename move together, so this cannot desynchronize them -- then
	truncate and publish into the shared results array.  The lexical
	search() call above already freed the PREVIOUS results at its entry (or,
	if query_text was NULL/empty, the results array is whatever it held
	before this call); either way those filenames are snapshotted into
	fused[] by now, so free them here before repopulating.
*/
qsort(fused, (size_t)fused_count, sizeof(*fused), ANT_fused_candidate_compare);

reset_results();

long long publish = fused_count < top_k ? fused_count : top_k;
for (which = 0; which < publish; which++)
	{
	hit *slot = append_result();

	slot->generation = fused[which].candidate.generation;
	slot->docid = fused[which].candidate.docid;
	slot->score = fused[which].candidate.score;
	slot->filename = fused[which].filename;		/* ownership transfer */
	fused[which].filename = NULL;
	}
for (which = publish; which < fused_count; which++)
	delete [] fused[which].filename;
delete [] fused;
return results_count;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_HYBRID_APPROX()
	--------------------------------------------
	Like search_hybrid(), but the vector leg is gathered via
	vector_candidates_approx() (signature-prefiltered) rather than
	vector_candidates() (exact). Verbatim copy of search_hybrid()'s body
	otherwise -- the RRF math (k=60), lexical leg, fused-merge logic, qsorts,
	and publish loop are all identical -- so the two stay aligned and a future
	reader can diff them to confirm only the gatherer differs. Transparently
	falls back to search_hybrid() when approximate is unconfigured or the
	metric is L2 (SimHash tracks angular, not Euclidean, distance).
*/
long long ATIRE_segment_index::search_hybrid_approx(char *query_text, const float *query_vector, long long top_k)
{
if (signature_bits_current == 0 || query_signer == NULL || vector_metric == VECTOR_METRIC_L2)
	return search_hybrid(query_text, query_vector, top_k);		// transparent fallback

long long lexical_count = 0, vector_count = 0, fused_count = 0, which, other;
char filename_buffer[4096];

if (top_k < 1)
	return 0;

/*
	Lexical side first: run the existing search and snapshot its hits (the
	results array is shared, so the snapshot must deep-copy the filenames)
	into the fused array before it gets overwritten.
*/
ANT_fused_candidate *fused = new ANT_fused_candidate[top_k * 2];

if (query_text != NULL && *query_text != '\0')
	lexical_count = search(query_text, top_k);
for (which = 0; which < lexical_count; which++)
	{
	fused[fused_count].candidate.generation = results[which].generation;
	fused[fused_count].candidate.docid = results[which].docid;
	fused[fused_count].candidate.score = 1.0 / (60.0 + (double)(which + 1));
	fused[fused_count].filename = new char[strlen(results[which].filename) + 1];
	strcpy(fused[fused_count].filename, results[which].filename);
	fused_count++;
	}

/*
	Vector side: candidates + rank contribution, merged into the fused set
	by (generation, docid) identity.
*/
if (query_vector != NULL && vector_dimension_current != 0)
	{
	ANT_vector_candidate *best = new ANT_vector_candidate[top_k];
	vector_count = vector_candidates_approx(query_vector, top_k, best);
	qsort(best, (size_t)vector_count, sizeof(*best), vector_candidate_compare);
	for (which = 0; which < vector_count; which++)
		{
		double contribution = 1.0 / (60.0 + (double)(which + 1));
		long found = false;
		for (other = 0; other < fused_count; other++)
			if (fused[other].candidate.generation == best[which].generation && fused[other].candidate.docid == best[which].docid)
				{
				fused[other].candidate.score += contribution;
				found = true;
				break;
				}
		if (!found)
			{
			fused[fused_count].candidate.generation = best[which].generation;
			fused[fused_count].candidate.docid = best[which].docid;
			fused[fused_count].candidate.score = contribution;
			char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));
			fused[fused_count].filename = new char[(filename != NULL ? strlen(filename) : 0) + 1];
			strcpy(fused[fused_count].filename, filename != NULL ? filename : "");
			fused_count++;
			}
		}
	delete [] best;
	}

/*
	Sort fused by score desc (ties: generation, docid asc) -- candidate and
	filename move together, so this cannot desynchronize them -- then
	truncate and publish into the shared results array.  The lexical
	search() call above already freed the PREVIOUS results at its entry (or,
	if query_text was NULL/empty, the results array is whatever it held
	before this call); either way those filenames are snapshotted into
	fused[] by now, so free them here before repopulating.
*/
qsort(fused, (size_t)fused_count, sizeof(*fused), ANT_fused_candidate_compare);

reset_results();

long long publish = fused_count < top_k ? fused_count : top_k;
for (which = 0; which < publish; which++)
	{
	hit *slot = append_result();

	slot->generation = fused[which].candidate.generation;
	slot->docid = fused[which].candidate.docid;
	slot->score = fused[which].candidate.score;
	slot->filename = fused[which].filename;		/* ownership transfer */
	fused[which].filename = NULL;
	}
for (which = publish; which < fused_count; which++)
	delete [] fused[which].filename;
delete [] fused;
return results_count;
}

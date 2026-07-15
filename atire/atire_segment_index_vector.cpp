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
#include <stdint.h>
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
#include "../source/filter.h"
#include "../source/wal.h"
#include "../source/signature.h"
#include "../source/signature_store.h"
#include "../source/hnsw.h"
#include "../source/multivector_store.h"
#include "../source/token_index.h"
#include "../source/pq_store.h"
#include "../source/multivector_pq_store.h"
#include "../source/pq_codec.h"

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

delete [] writer_multivector_data;
delete [] writer_multivector_counts;
writer_multivector_data = NULL;
writer_multivector_counts = NULL;
writer_multivector_capacity = 0;
writer_multivector_total = 0;
writer_multivector_counts_capacity = 0;

if (writer_attribute_sets != NULL)
	{
	for (long long i = 0; i < writer_attribute_sets_capacity; i++)
		delete writer_attribute_sets[i];
	delete [] writer_attribute_sets;
	writer_attribute_sets = NULL;
	}
writer_attribute_sets_capacity = 0;
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
	ATIRE_SEGMENT_INDEX::LOAD_QUANTIZATION_CONFIG()
	------------------------------------------------
	Reads <dir>/quantization.config (magic/version/mode).  Absent => leaves
	quantization_current unchanged (off).  Garbage => treated as absent
	(defensive parse, mirrors load_hnsw_config).
*/
long ATIRE_segment_index::load_quantization_config(void)
{
char filename[4096];
FILE *fp;
unsigned long long magic, want;
unsigned int version;
long long mode;
const char *tag = "ANTQUAN1";

memcpy(&want, tag, 8);
snprintf(filename, sizeof(filename), "%s/quantization.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != want
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != 1u
	|| fread(&mode, sizeof(mode), 1, fp) != 1
	|| mode < 0 || mode > 2)
	{ fclose(fp); return 0; }
fclose(fp);
quantization_current = (long)mode;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_QUANTIZATION_CONFIG()
	--------------------------------------------------
	Atomic write (temp + rename) of the index-wide quantization config.
*/
long ATIRE_segment_index::save_quantization_config(void)
{
char filename[4096], temp[4200];
FILE *fp;
unsigned long long magic;
unsigned int version = 1u;
long long mode = quantization_current;
const char *tag = "ANTQUAN1";

memcpy(&magic, tag, 8);
snprintf(filename, sizeof(filename), "%s/quantization.config", directory);
if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp))
	return 1;
if ((fp = fopen(temp, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&mode, sizeof(mode), 1, fp) != 1)
	{ fclose(fp); remove(temp); return 1; }
fclose(fp);
if (rename(temp, filename) != 0)
	{ remove(temp); return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_QUANTIZATION()
	------------------------------------------
	Enable quantization.  Requires the index open with vectors enabled.  Once
	set, the mode is immutable: setting the SAME mode again is a no-op success
	(idempotent), but setting a DIFFERENT mode is rejected.  This mirrors
	set_hnsw_config()/set_approximate_config()'s "first enable wins" contract,
	except those two are silently idempotent for any later call while
	quantization additionally rejects mode changes explicitly (a later task
	depends on this once flush/compaction start quantizing vectors on disk).
*/
long ATIRE_segment_index::set_quantization(long mode)
{
if (directory == NULL)
	return 1;					// must be open (needs directory + dimension)
if (vector_dimension_current == 0)
	return 1;					// quantization requires vectors enabled
if (pq_configured())
	return 1;					// mutually exclusive with PQ (set_pq_config())
if (mode != QUANTIZE_REPLACE && mode != QUANTIZE_EXACT)
	return 1;					// only these are settable
if (quantization_current != 0)
	return (quantization_current == mode) ? 0 : 1;		// idempotent same-mode; reject different-mode (immutable)
quantization_current = mode;
if (save_quantization_config() != 0)
	{ quantization_current = 0; return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::DEFAULT_PQ_M()
	--------------------------------------
	Chooses a subvector count when the caller passes m==0 to set_pq_config():
	the largest divisor of dimension that is <= 16 (a conventional PQ
	subvector-count ceiling).  Falls back to 1 if dimension itself is < 1.
*/
long long ATIRE_segment_index::default_pq_m(long long dimension)
{
long long cap, d;

cap = dimension < 16 ? dimension : 16;
for (d = cap; d >= 1; d--)
	if (dimension % d == 0)
		return d;
return 1;
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_PQ_CONFIG()
	----------------------------------------
	Reads <dir>/pq.config (magic/version/m/posture/rerank_quant).  Absent =>
	leaves pq_m_current unchanged (off).  Garbage => treated as absent
	(defensive parse, mirrors load_quantization_config).
*/
long ATIRE_segment_index::load_pq_config(void)
{
char filename[4096];
FILE *fp;
unsigned long long magic, want;
unsigned int version;
long long m, posture, rerank_quant, tier = PQ_TIER_FLOAT, opq = 0, global = 0, kk = 256;
const char *tag = "ANTPQCF1";

memcpy(&want, tag, 8);
snprintf(filename, sizeof(filename), "%s/pq.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != want
	|| fread(&version, sizeof(version), 1, fp) != 1 || (version != 1u && version != 2u && version != 3u && version != 4u && version != 5u)
	|| fread(&m, sizeof(m), 1, fp) != 1 || m < 1 || m > 65536
	|| fread(&posture, sizeof(posture), 1, fp) != 1 || (posture != 0 && posture != 1)
	|| fread(&rerank_quant, sizeof(rerank_quant), 1, fp) != 1 || (rerank_quant != 0 && rerank_quant != 1))
	{ fclose(fp); return 0; }
if (version == 2u || version == 3u || version == 4u || version == 5u)
	{
	if (fread(&tier, sizeof(tier), 1, fp) != 1 || tier < 0 || tier > 2)
		{ fclose(fp); return 0; }
	}
if (version == 3u || version == 4u || version == 5u)
	{
	if (fread(&opq, sizeof(opq), 1, fp) != 1 || (opq != 0 && opq != 1))
		{ fclose(fp); return 0; }
	}
if (version == 4u || version == 5u)
	{
	if (fread(&global, sizeof(global), 1, fp) != 1 || (global != 0 && global != 1))
		{ fclose(fp); return 0; }
	}
if (version == 5u)
	{
	if (fread(&kk, sizeof(kk), 1, fp) != 1 || ANT_pq_codec::bits_for_k(kk) < 0)
		{ fclose(fp); return 0; }
	}
fclose(fp);
if (vector_dimension_current != 0 && vector_dimension_current % m != 0)
	return 0;					/* m must divide the vector dimension; leave PQ unconfigured */
pq_m_current = m;
pq_posture_current = (long)posture;
pq_rerank_quant_current = (long)rerank_quant;
pq_resident_tier_current = (long)tier;
pq_opq_current = (long)opq;
pq_global_current = (long)global;
pq_k_current = kk;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_PQ_CONFIG()
	----------------------------------------
	Atomic write (temp + rename) of the index-wide PQ config.
*/
long ATIRE_segment_index::save_pq_config(void)
{
char filename[4096], temp[4200];
FILE *fp;
unsigned long long magic;
unsigned int version = 5u;
long long m = pq_m_current;
long long posture = pq_posture_current;
long long rerank_quant = pq_rerank_quant_current;
long long tier = pq_resident_tier_current;
long long opq = pq_opq_current;
long long global = pq_global_current;
long long kk = pq_k_current;
const char *tag = "ANTPQCF1";

memcpy(&magic, tag, 8);
snprintf(filename, sizeof(filename), "%s/pq.config", directory);
if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp))
	return 1;
if ((fp = fopen(temp, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&m, sizeof(m), 1, fp) != 1
	|| fwrite(&posture, sizeof(posture), 1, fp) != 1
	|| fwrite(&rerank_quant, sizeof(rerank_quant), 1, fp) != 1
	|| fwrite(&tier, sizeof(tier), 1, fp) != 1
	|| fwrite(&opq, sizeof(opq), 1, fp) != 1
	|| fwrite(&global, sizeof(global), 1, fp) != 1
	|| fwrite(&kk, sizeof(kk), 1, fp) != 1)
	{ fclose(fp); remove(temp); return 1; }
fclose(fp);
if (rename(temp, filename) != 0)
	{ remove(temp); return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_PQ_CONFIG()
	-----------------------------------------
	Enable Product Quantization.  Requires the index open with vectors
	enabled, and is mutually exclusive with V4 int8 quantization
	(set_quantization()) -- an index uses one dense-vector compression
	scheme or the other, never both.  m==0 requests the default subvector
	count (default_pq_m()); m must otherwise divide the configured vector
	dimension evenly.  Once set, the config is immutable: setting the SAME
	(m, posture, rerank_quant) again is a no-op success (idempotent), but
	any difference is rejected.  Mirrors set_quantization()'s "first enable
	wins" contract.
*/
long ATIRE_segment_index::set_pq_config(long long m, long posture, long rerank_quant)
{
long long mm;

if (directory == NULL)
	return 1;					// must be open (needs directory + dimension)
if (vector_dimension_current == 0)
	return 1;					// PQ requires vectors enabled
if (quantization_current != 0)
	return 1;					// mutually exclusive with V4 int8 quantization
if (posture != PQ_POSTURE_REPLACE && posture != PQ_POSTURE_RERANK)
	return 1;
if (rerank_quant != RERANK_QUANT_FLOAT && rerank_quant != RERANK_QUANT_INT8)
	return 1;
mm = (m == 0) ? default_pq_m(vector_dimension_current) : m;
if (mm < 1 || vector_dimension_current % mm != 0)
	return 1;
if (pq_m_current != 0)		// idempotent same-config; reject different-config (immutable)
	return (pq_m_current == mm && pq_posture_current == posture && pq_rerank_quant_current == rerank_quant) ? 0 : 1;
pq_m_current = mm;
pq_posture_current = posture;
pq_rerank_quant_current = rerank_quant;
if (save_pq_config() != 0)
	{ pq_m_current = 0; pq_posture_current = 0; pq_rerank_quant_current = 0; return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_PQ_RESIDENT_TIER()
	--------------------------------------------
	Selects what stays resident in RAM alongside the PQ codes: the full
	float vectors (FLOAT, default -- Phase 1 behaviour, no RAM win), an
	int8-quantized copy (INT8), or nothing (NONE -- PQ codes only, the
	maximal RAM win).  NONE is incompatible with rerank posture (there
	would be nothing to exact-rescore against).  Like set_pq_config(),
	once moved off the FLOAT default the tier is immutable: setting the
	SAME tier again is a no-op success, but any further change is
	rejected.
*/
long ATIRE_segment_index::set_pq_resident_tier(long tier)
{
if (directory == NULL || vector_dimension_current == 0)
	return 1;                       // must be open with vectors
if (!pq_configured())
	return 1;                       // PQ must be configured first
if (tier != PQ_TIER_FLOAT && tier != PQ_TIER_INT8 && tier != PQ_TIER_NONE)
	return 1;
if (tier == PQ_TIER_NONE && pq_posture_current == PQ_POSTURE_RERANK)
	return 1;                       // NONE is replace-only (no resident store to rescore)
if (pq_resident_tier_current != tier && pq_resident_tier_current != PQ_TIER_FLOAT)
	return 1;                       // immutable once moved off the default
if (pq_resident_tier_current == tier)
	return 0;                       // idempotent
long previous = pq_resident_tier_current;
pq_resident_tier_current = tier;
if (save_pq_config() != 0)
	{ pq_resident_tier_current = previous; return 1; }

/*
	#20: reaching here is a real tier change away from the FLOAT default
	(idempotent same-tier and off-FLOAT-immutable both returned above).  The
	resident graph source therefore changes (float -> int8 .pqr / pq_vectors),
	so any already-built .hnsw was built over the OLD (float) geometry -- its
	edges no longer match how vector_candidates_hnsw will now score nodes.
	Invalidate every per-segment graph (drop the sidecar + the in-memory graph)
	so the next build_hnsw()/compaction rebuilds it over the new tier source,
	keeping build-source == search-source.  Segments are exact/ADC-scanned in
	the meantime (best-effort, like any un-built graph).
*/
if (hnsw_M_current != 0)
	{
	char hnsw_name[4096];
	long long which;
	for (which = 0; which < segment_count; which++)
		{
		segment_filename(hnsw_name, sizeof(hnsw_name), segments[which].generation, "hnsw");
		remove(hnsw_name);
		delete segments[which].hnsw_graph;
		segments[which].hnsw_graph = NULL;
		}
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_PQ_OPQ()
	----------------------------------
	Enable OPQ rotation for the dense `.pq` store: a learned orthogonal D*D
	rotation applied before subspace splitting, improving recall at the same
	m/k (metric-exactly -- an orthogonal rotation preserves dot/L2/cosine).
	Requires PQ already configured (set_pq_config()).  Like set_pq_resident_tier(),
	once enabled it is immutable: setting the SAME value again is a no-op
	success (idempotent), but flipping it back off (or to any other value once
	on) is rejected.  Persists in pq.config v3; existing (v1/v2) writer create()
	call sites pass pq_opq_current so backfilled/compacted segments train R
	under the configured flag.
*/
long ATIRE_segment_index::set_pq_opq(long enable)
{
long want;

if (directory == NULL)
	return 1;                       // must be open
if (!pq_configured())
	return 1;                       // PQ must be configured first
want = enable ? 1 : 0;
if (pq_opq_current == want)
	return 0;                       // idempotent
if (pq_opq_current != 0)
	return 1;                       // immutable once enabled
pq_opq_current = want;
if (save_pq_config() != 0)
	{ pq_opq_current = 0; return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_PQ_GLOBAL_CODEBOOK()
	----------------------------------------------
	Opt-in to a single collection-wide (global) `.pq` codebook, trained ONCE
	(from the first segment build_pq() trains against) instead of a fresh
	codebook per segment: dense PQ codes then compare across segments (a
	prerequisite for cross-segment ANN structures). Requires PQ already
	configured (set_pq_config()).  Like set_pq_opq()/set_pq_resident_tier(),
	once enabled it is immutable: setting the SAME value again is a no-op
	success (idempotent), but flipping it back off (or to any other value
	once on) is rejected.  Persists in pq.config v4.  Composes with OPQ
	(#22.1): under global mode the learned rotation is ALSO shared (one R),
	trained alongside the codebook by ensure_global_pq_codebook().
*/
long ATIRE_segment_index::set_pq_global_codebook(long enable)
{
long want;

if (directory == NULL)
	return 1;                       // must be open
if (!pq_configured())
	return 1;                       // PQ must be configured first
want = enable ? 1 : 0;
if (pq_global_current == want)
	return 0;                       // idempotent
if (pq_global_current != 0)
	return 1;                       // immutable once enabled
pq_global_current = want;
if (save_pq_config() != 0)
	{ pq_global_current = 0; return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_PQ_K()
	--------------------------------
	Choose the dense `.pq` codebook size k (a power of two in [2,256]); k<256
	bit-packs codes below a byte per subvector for a smaller .pq, trading
	recall.  Requires PQ already configured (set_pq_config()).  Default 256
	(byte-per-code, byte-identical to pre-#22.3).  Like set_pq_opq(), once
	changed from 256 it is immutable: the SAME value is a no-op success
	(idempotent); any different value once changed is rejected.  Persists in
	pq.config v5.  Composes with OPQ/global/tier (orthogonal); writer create()
	sites pass pq_k_current so backfilled/compacted segments encode at k.
*/
long ATIRE_segment_index::set_pq_k(long long k)
{
if (directory == NULL)
	return 1;						// must be open
if (!pq_configured())
	return 1;						// PQ must be configured first
if (ANT_pq_codec::bits_for_k(k) < 0)
	return 1;						// not a power of two in [2,256]
if (pq_k_current == k)
	return 0;						// idempotent
if (pq_k_current != 256)
	return 1;						// immutable once changed from the default
pq_k_current = k;
if (save_pq_config() != 0)
	{ pq_k_current = 256; return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_PQ_CODEBOOK() / LOAD_PQ_CODEBOOK()
	----------------------------------------------------------------
	Persist/load the frozen global codebook (+ OPQ rotation, when configured)
	in <dir>/pq.codebook (magic "ANTPQGCB"): u32 version(1), i64 dimension,
	i64 m, i64 k, i64 opq, then (opq ? global_pq_rotation: dimension*dimension
	floats) + global_pq_codebook: m*k*(dimension/m) floats.  save_pq_codebook()
	is an atomic write (temp + rename).  load_pq_codebook() is forgiving: any
	mismatch (magic/version/dimension/m/k/opq/file size) leaves
	global_pq_codebook/global_pq_rotation NULL (untrained) rather than
	crashing or over-reading -- ensure_global_pq_codebook() will then
	(re)train on the next build_pq().  Validates before allocating, and
	bounds dimension*dimension (D <= 65536) before computing the rotation
	block size, so a corrupt/hostile sidecar cannot trigger an oversized
	allocation or an out-of-bounds read.
*/
long ATIRE_segment_index::save_pq_codebook(void)
{
char filename[4096], temp[4200];
FILE *fp;
unsigned long long magic;
unsigned int version = 1u;
long long dimension = vector_dimension_current;
long long m = pq_m_current;
long long k = pq_k_current;
long long opq = pq_opq_current;
long long sub = (m != 0) ? dimension / m : 0;
long long codebook_floats = m * k * sub;
const char *tag = "ANTPQGCB";

if (global_pq_codebook == NULL)
	return 1;					// nothing trained yet
memcpy(&magic, tag, 8);
snprintf(filename, sizeof(filename), "%s/pq.codebook", directory);
if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp))
	return 1;
if ((fp = fopen(temp, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&dimension, sizeof(dimension), 1, fp) != 1
	|| fwrite(&m, sizeof(m), 1, fp) != 1
	|| fwrite(&k, sizeof(k), 1, fp) != 1
	|| fwrite(&opq, sizeof(opq), 1, fp) != 1)
	{ fclose(fp); remove(temp); return 1; }
if (opq && global_pq_rotation != NULL)
	{
	if (fwrite(global_pq_rotation, sizeof(float), (size_t)(dimension * dimension), fp) != (size_t)(dimension * dimension))
		{ fclose(fp); remove(temp); return 1; }
	}
if (fwrite(global_pq_codebook, sizeof(float), (size_t)codebook_floats, fp) != (size_t)codebook_floats)
	{ fclose(fp); remove(temp); return 1; }
fclose(fp);
if (rename(temp, filename) != 0)
	{ remove(temp); return 1; }
return 0;
}

long ATIRE_segment_index::load_pq_codebook(void)
{
char filename[4096];
FILE *fp;
unsigned long long magic, want;
unsigned int version;
long long dimension, m, k, opq;
long long sub, codebook_floats, rotation_floats;
long fail;
const char *tag = "ANTPQGCB";

memcpy(&want, tag, 8);
snprintf(filename, sizeof(filename), "%s/pq.codebook", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;					// absent: leave untrained (ensure_global_pq_codebook() will train)

fail = (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != want
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != 1u
	|| fread(&dimension, sizeof(dimension), 1, fp) != 1 || dimension != vector_dimension_current
	|| fread(&m, sizeof(m), 1, fp) != 1 || m != pq_m_current
	|| fread(&k, sizeof(k), 1, fp) != 1 || k != (long long)pq_k_current
	|| fread(&opq, sizeof(opq), 1, fp) != 1 || (opq != 0 && opq != 1) || opq != pq_opq_current);
if (!fail && (dimension <= 0 || dimension > 65536 || m <= 0 || dimension % m != 0))
	fail = 1;					// bounds dimension*dimension before it is used below
if (fail)
	{ fclose(fp); return 0; }

sub = dimension / m;
codebook_floats = m * k * sub;
rotation_floats = opq ? dimension * dimension : 0;

/* validate the exact remaining file size before allocating anything */
{
long here = ftell(fp);
long end;
if (here < 0) { fclose(fp); return 0; }
fseek(fp, 0, SEEK_END);
end = ftell(fp);
fseek(fp, here, SEEK_SET);
if (end < 0 || (end - here) != (long)((rotation_floats + codebook_floats) * (long long)sizeof(float)))
	{ fclose(fp); return 0; }
}

float *new_rotation = NULL;
float *new_codebook = new float[codebook_floats > 0 ? codebook_floats : 1];
if (opq)
	{
	new_rotation = new float[rotation_floats > 0 ? rotation_floats : 1];
	if (fread(new_rotation, sizeof(float), (size_t)rotation_floats, fp) != (size_t)rotation_floats)
		{ delete [] new_rotation; delete [] new_codebook; fclose(fp); return 0; }
	}
if (fread(new_codebook, sizeof(float), (size_t)codebook_floats, fp) != (size_t)codebook_floats)
	{ delete [] new_rotation; delete [] new_codebook; fclose(fp); return 0; }
fclose(fp);

delete [] global_pq_codebook;
delete [] global_pq_rotation;
global_pq_codebook = new_codebook;
global_pq_rotation = new_rotation;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::ENSURE_GLOBAL_PQ_CODEBOOK()
	---------------------------------------------------
	Trains (and persists) the shared global codebook the FIRST time it is
	needed -- from segment `which`'s present on-disk float rows (mirrors how
	build_pq() itself loads a segment's float .vec, decoupled from whatever
	resident tier is currently realized).  A no-op (returns 0) once
	global_pq_codebook is already populated (by an earlier call in this
	session, or by load_pq_codebook() at open()).  Fail-soft: any failure
	(missing/degraded/empty .vec, training failure, or a save_pq_codebook()
	failure) leaves global_pq_codebook NULL and returns nonzero -- the caller
	(build_pq()) then skips set_external_codebook() and the writer trains a
	per-segment codebook instead, so a transient failure here never hard-fails
	a build.
*/
long ATIRE_segment_index::ensure_global_pq_codebook(long which)
{
char vec_name[4096];
long long docs, present_count, d;
float *rows, *tmp;
ANT_vector_store *src;

if (global_pq_codebook != NULL)
	return 0;					// already trained (this session or loaded at open())
if (which < 0 || which >= segment_count || pq_m_current == 0)
	return 1;

segment_filename(vec_name, sizeof(vec_name), segments[which].generation, "vec");
docs = segments[which].engine->get_document_count();
src = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
if (src->document_count() != docs || docs <= 0 || src->is_quantized())
	{ delete src; return 1; }

rows = new float[docs * vector_dimension_current];
present_count = 0;
for (d = 0; d < docs; d++)
	if (src->has(d))
		{
		src->reconstruct(d, rows + present_count * vector_dimension_current);
		present_count++;
		}
delete src;

if (present_count == 0)
	{ delete [] rows; return 1; }

if (pq_opq_current)
	{
	global_pq_rotation = new float[vector_dimension_current * vector_dimension_current];
	if (ANT_pq_codec::train_rotation(rows, vector_dimension_current, pq_m_current, present_count, global_pq_rotation) != 0)
		{ delete [] global_pq_rotation; global_pq_rotation = NULL; delete [] rows; return 1; }
	tmp = new float[vector_dimension_current];
	for (d = 0; d < present_count; d++)
		{
		ANT_pq_codec::apply_rotation(rows + d * vector_dimension_current, vector_dimension_current, global_pq_rotation, tmp);
		memcpy(rows + d * vector_dimension_current, tmp, (size_t)vector_dimension_current * sizeof(float));
		}
	delete [] tmp;
	}

{
long long sub = vector_dimension_current / pq_m_current;
long long floats = pq_m_current * pq_k_current * sub;
global_pq_codebook = new float[floats > 0 ? floats : 1];
if (ANT_pq_codec::train(rows, vector_dimension_current, pq_m_current, pq_k_current, present_count, global_pq_codebook) != 0)
	{
	delete [] global_pq_codebook; global_pq_codebook = NULL;
	delete [] global_pq_rotation; global_pq_rotation = NULL;
	delete [] rows;
	return 1;
	}
}
delete [] rows;

if (save_pq_codebook() != 0)
	{
	delete [] global_pq_codebook; global_pq_codebook = NULL;
	delete [] global_pq_rotation; global_pq_rotation = NULL;
	return 1;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::REBUILD_PQ_GLOBAL_CODEBOOK()
	--------------------------------------------------
	Explicit retrain + re-encode escape hatch (#22 Task 3).  Unlike
	ensure_global_pq_codebook() (which trains once, from a single segment,
	the first time it is needed), this gathers present float rows across
	EVERY disk segment's on-disk .vec, retrains a fresh codebook (+ OPQ
	rotation when configured) from that index-wide sample, persists it, then
	re-encodes every segment that currently has a .pq against the new
	codebook -- so all segments stay comparable after the retrain.

	Requires open + PQ configured + global mode.  Rows are gathered BEFORE
	the current global_pq_codebook/rotation are freed, so a failure during
	gathering (no present rows anywhere) leaves the prior codebook intact
	and every on-disk .pq untouched.  Once gathering succeeds the prior
	buffers are freed and retraining begins; a subsequent training/persist
	failure leaves global_pq_codebook/rotation NULL (untrained -- callers
	fall back to per-segment training, same fail-soft contract as
	ensure_global_pq_codebook()) but does NOT touch any on-disk .pq (the
	re-encode loop below has not started yet).  Once the new codebook is
	persisted, each segment is re-encoded independently; a per-segment
	failure is skipped (that segment's .pq is left as-is, referencing the
	OLD codebook until the next successful rebuild) rather than aborting
	the whole operation.  Returns 0 iff every segment with a .pq was
	re-encoded successfully.
*/
long ATIRE_segment_index::rebuild_pq_global_codebook(void)
{
char vec_name[4096], pq_name[4096];
long long which, d, docs, present_count, total_docs;
float *rows, *tmp;

if (directory == NULL || !pq_configured() || !pq_global_current)
	return 1;

total_docs = 0;
for (which = 0; which < segment_count; which++)
	total_docs += segments[which].engine->get_document_count();
if (total_docs <= 0)
	return 1;

/* Pass 1: gather every segment's present float rows into one contiguous buffer. */
rows = new float[total_docs * vector_dimension_current];
present_count = 0;
for (which = 0; which < segment_count; which++)
	{
	docs = segments[which].engine->get_document_count();
	if (docs <= 0)
		continue;
	segment_filename(vec_name, sizeof(vec_name), segments[which].generation, "vec");
	ANT_vector_store *src = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
	if (src->document_count() == docs && !src->is_quantized())
		for (d = 0; d < docs; d++)
			if (src->has(d))
				{
				src->reconstruct(d, rows + present_count * vector_dimension_current);
				present_count++;
				}
	delete src;
	}

if (present_count == 0)
	{ delete [] rows; return 1; }				/* nothing to retrain from -- leave the prior codebook/.pq files untouched */

/* Only now discard the prior codebook: we know we have rows to train a replacement. */
delete [] global_pq_codebook; global_pq_codebook = NULL;
delete [] global_pq_rotation; global_pq_rotation = NULL;

if (pq_opq_current)
	{
	global_pq_rotation = new float[vector_dimension_current * vector_dimension_current];
	if (ANT_pq_codec::train_rotation(rows, vector_dimension_current, pq_m_current, present_count, global_pq_rotation) != 0)
		{ delete [] global_pq_rotation; global_pq_rotation = NULL; delete [] rows; return 1; }
	tmp = new float[vector_dimension_current];
	for (d = 0; d < present_count; d++)
		{
		ANT_pq_codec::apply_rotation(rows + d * vector_dimension_current, vector_dimension_current, global_pq_rotation, tmp);
		memcpy(rows + d * vector_dimension_current, tmp, (size_t)vector_dimension_current * sizeof(float));
		}
	delete [] tmp;
	}

{
long long sub = vector_dimension_current / pq_m_current;
long long floats = pq_m_current * pq_k_current * sub;
global_pq_codebook = new float[floats > 0 ? floats : 1];
if (ANT_pq_codec::train(rows, vector_dimension_current, pq_m_current, pq_k_current, present_count, global_pq_codebook) != 0)
	{
	delete [] global_pq_codebook; global_pq_codebook = NULL;
	delete [] global_pq_rotation; global_pq_rotation = NULL;
	delete [] rows;
	return 1;
	}
}
delete [] rows;

if (save_pq_codebook() != 0)
	return 1;						/* persist failed -> skip re-encoding: on-disk sidecar and every .pq keep the OLD codebook (in-session search stays consistent via each .pq's embedded copy); resident global_pq_* now holds the NEW codebook, so a later same-session build_pq()/compact() would emit an incomparable segment -- caller must honor this nonzero return */

/*
	Pass 2: re-encode every segment that currently has a .pq against the new
	codebook (present rows reconstructed + absent rows appended as NULL, to
	keep docids aligned -- mirrors build_pq()'s append loop).  A segment
	without a valid resident .pq is left alone (nothing to re-encode).
*/
long any_failed = 0;
for (which = 0; which < segment_count; which++)
	{
	docs = segments[which].engine->get_document_count();
	int have_pq = (segments[which].pq_vectors != NULL && segments[which].pq_vectors->document_count() == docs && docs > 0);
	if (!have_pq)
		continue;

	segment_filename(vec_name, sizeof(vec_name), segments[which].generation, "vec");
	segment_filename(pq_name, sizeof(pq_name), segments[which].generation, "pq");
	ANT_vector_store *src = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
	if (src->document_count() != docs || src->is_quantized())
		{ delete src; any_failed = 1; continue; }		/* no usable float .vec -- leave this segment's .pq as-is */

	ANT_pq_store_writer w;
	long failed = w.create(pq_name, vector_dimension_current, pq_m_current, pq_k_current, vector_metric, pq_opq_current) != 0;
	if (!failed)
		w.set_external_codebook(global_pq_codebook, global_pq_rotation);
	float *buf = new float[vector_dimension_current];
	for (d = 0; !failed && d < docs; d++)
		{
		if (src->has(d))
			{ src->reconstruct(d, buf); failed = w.append(buf) != 0; }
		else
			failed = w.append(NULL) != 0;
		}
	delete [] buf;
	if (!failed)
		failed = w.finish() != 0;
	if (failed)
		{ w.abandon(); any_failed = 1; }
	else
		{
		delete segments[which].pq_vectors;
		segments[which].pq_vectors = load_segment_pq_vectors(pq_name, docs);
		}
	delete src;
	}

return any_failed ? 1 : 0;
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_MULTIVECTOR_PQ_CONFIG() / SAVE_...()
	-------------------------------------------------------------
	Persist token-PQ config in <dir>/multivector_pq.config (magic "ANTMVPQC").
	Version 2, four i64: m, posture, rerank_quant, resident_tier.  A version-1
	file (three i64) loads with tier defaulting to MV_TIER_FLOAT (#24
	back-compat).  Defensive parse.
*/
long ATIRE_segment_index::load_multivector_pq_config(void)
{
if (directory == NULL)
	return 1;

char name[4096];
snprintf(name, sizeof(name), "%s/multivector_pq.config", directory);

FILE *in = fopen(name, "rb");
if (in == NULL)
	return 1;

char tag[8];
unsigned int version;
long long vals[4];
if (fread(tag, 1, 8, in) != 8 || memcmp(tag, "ANTMVPQC", 8) != 0 || fread(&version, 4, 1, in) != 1)
	{ fclose(in); return 1; }
long ok;
if (version == 1)
	{ ok = (fread(vals, 8, 3, in) == 3); vals[3] = MV_TIER_FLOAT; }
else if (version == 2)
	{ ok = (fread(vals, 8, 4, in) == 4); }
else
	ok = 0;
fclose(in);
if (!ok)
	return 1;

long long m = vals[0], posture = vals[1], rq = vals[2], tier = vals[3];
if (m < 1
	|| (posture != PQ_POSTURE_REPLACE && posture != PQ_POSTURE_RERANK)
	|| (rq != RERANK_QUANT_FLOAT && rq != RERANK_QUANT_INT8)
	|| (tier != MV_TIER_FLOAT && tier != MV_TIER_NONE)
	|| (rerank_dimension_current != 0 && rerank_dimension_current % m != 0))
	return 1;					/* invalid persisted config; leave token-PQ unconfigured */

mvpq_m_current = m;
mvpq_posture_current = (long)posture;
mvpq_rerank_quant_current = (long)rq;
mvpq_resident_tier_current = (long)tier;
return 0;
}

long ATIRE_segment_index::save_multivector_pq_config(void)
{
if (directory == NULL)
	return 1;

char name[4096], temp[4200];
snprintf(name, sizeof(name), "%s/multivector_pq.config", directory);
if (snprintf(temp, sizeof(temp), "%s.tmp", name) >= (int)sizeof(temp))
	return 1;

FILE *out = fopen(temp, "wb");
if (out == NULL)
	return 1;

unsigned int version = 2;
long long vals[4] = { mvpq_m_current, mvpq_posture_current, mvpq_rerank_quant_current, mvpq_resident_tier_current };
long ok = fwrite("ANTMVPQC", 1, 8, out) == 8 && fwrite(&version, 4, 1, out) == 1 && fwrite(vals, 8, 4, out) == 4;
if (fclose(out) != 0)
	ok = 0;
if (ok && rename(temp, name) != 0)
	ok = 0;
if (!ok)
	remove(temp);
return ok ? 0 : 1;
}

/*
	ATIRE_SEGMENT_INDEX::SET_MULTIVECTOR_PQ_CONFIG()
	------------------------------------------------
	Enable token-PQ.  Requires rerank(multivectors) configured; m must divide the
	rerank dimension (m==0 => default_pq_m()); mutually exclusive with the .mvec
	int8 mode.  Immutable once set.
*/
long ATIRE_segment_index::set_multivector_pq_config(long long m, long posture, long rerank_quant)
{
if (!rerank_configured())
	return 1;
if (rerank_quant_current == RERANK_QUANT_INT8)
	return 1;					// mutually exclusive with the .mvec int8 token mode
if (posture != PQ_POSTURE_REPLACE && posture != PQ_POSTURE_RERANK)
	return 1;
if (rerank_quant != RERANK_QUANT_FLOAT && rerank_quant != RERANK_QUANT_INT8)
	return 1;
if (m == 0)
	m = default_pq_m(rerank_dimension_current);
if (m < 1 || m > rerank_dimension_current || rerank_dimension_current % m != 0)
	return 1;
if (multivector_pq_configured())		// idempotent same-config; reject different-config (immutable)
	return (mvpq_m_current == m && mvpq_posture_current == posture && mvpq_rerank_quant_current == rerank_quant) ? 0 : 1;
mvpq_m_current = m;
mvpq_posture_current = posture;
mvpq_rerank_quant_current = rerank_quant;
if (save_multivector_pq_config() != 0)
	{ mvpq_m_current = 0; mvpq_posture_current = PQ_POSTURE_REPLACE; mvpq_rerank_quant_current = RERANK_QUANT_FLOAT; return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_MULTIVECTOR_RESIDENT_TIER()
	------------------------------------------------------
	Selects whether the resident token graph source is the full float .mvec
	pool (FLOAT, default -- Phase-1/Task-1..2 behaviour, no RAM win) or the
	PQ-compressed .mvpq pool only (NONE -- float pool dropped from RAM, the
	maximal RAM win; float stays on disk for retrain/compaction/rerank).
	Like set_pq_resident_tier(), once moved off the FLOAT default the tier is
	immutable: setting the SAME tier again is a no-op success, but any
	further change is rejected.  The tier is realized at load (append_segment)
	-- this call only persists config + invalidates stale .tann sidecars built
	over the old (float) geometry; a reopen picks up the new tier source.
*/
long ATIRE_segment_index::set_multivector_resident_tier(long tier)
{
if (directory == NULL)
	return 1;
if (!multivector_pq_configured())
	return 1;
if (tier != MV_TIER_FLOAT && tier != MV_TIER_NONE)
	return 1;
if (tier == MV_TIER_NONE && mvpq_posture_current == PQ_POSTURE_RERANK)
	return 1;					// NONE is replace-only: no resident float to rerank against
if (mvpq_resident_tier_current != tier && mvpq_resident_tier_current != MV_TIER_FLOAT)
	return 1;						// immutable once moved off the default
if (mvpq_resident_tier_current == tier)
	return 0;						// idempotent
long previous = mvpq_resident_tier_current;
mvpq_resident_tier_current = tier;
if (save_multivector_pq_config() != 0)
	{ mvpq_resident_tier_current = previous; return 1; }

/*
	#24: a real tier change away from FLOAT changes the token graph source (float
	.mvec -> .mvpq ADC). Any .tann built over the OLD float geometry no longer
	matches how the PQ source scores nodes, so invalidate every per-segment .tann
	(+ .tann.g) and null the in-memory token_index; the next build_token_index()/
	compaction rebuilds over the new (PQ) source at reopen. Mirrors set_pq_resident_tier.
*/
char tann_name[4096], tanng_name[4200];
long long which;
for (which = 0; which < segment_count; which++)
	{
	segment_filename(tann_name, sizeof(tann_name), segments[which].generation, "tann");
	snprintf(tanng_name, sizeof(tanng_name), "%s.g", tann_name);
	remove(tann_name);
	remove(tanng_name);
	delete segments[which].token_index;
	segments[which].token_index = NULL;
	}
return 0;
}

long ATIRE_segment_index::disk_segment_resident_tier_mv(long long which)
{
if (which < 0 || which >= segment_count)
	return -1;
if (segments[which].multivectors == NULL && segments[which].multivector_pq != NULL)
	return MV_TIER_NONE;
return MV_TIER_FLOAT;
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_RERANK_CONFIG()
	--------------------------------------------
	Reads <dir>/rerank.config (magic/version/dimension/quant).  Absent =>
	leaves rerank_dimension_current unchanged (unconfigured).  Garbage =>
	treated as absent (defensive parse, mirrors load_quantization_config).
*/
long ATIRE_segment_index::load_rerank_config(void)
{
char filename[4096];
FILE *fp;
unsigned long long magic, want;
unsigned int version;
long long dimension, quant;
const char *tag = "ANTRR001";

memcpy(&want, tag, 8);
snprintf(filename, sizeof(filename), "%s/rerank.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != want
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != 1u
	|| fread(&dimension, sizeof(dimension), 1, fp) != 1
	|| fread(&quant, sizeof(quant), 1, fp) != 1
	|| dimension < 1 || dimension > 65536
	|| (quant != RERANK_QUANT_FLOAT && quant != RERANK_QUANT_INT8))
	{ fclose(fp); return 0; }
fclose(fp);
rerank_dimension_current = dimension;
rerank_quant_current = (long)quant;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_RERANK_CONFIG()
	----------------------------------------------
	Atomic write (temp + rename) of the index-wide rerank config.
*/
long ATIRE_segment_index::save_rerank_config(void)
{
char filename[4096], temp[4200];
FILE *fp;
unsigned long long magic;
unsigned int version = 1u;
long long dimension = rerank_dimension_current, quant = rerank_quant_current;
const char *tag = "ANTRR001";

memcpy(&magic, tag, 8);
snprintf(filename, sizeof(filename), "%s/rerank.config", directory);
if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp))
	return 1;
if ((fp = fopen(temp, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&dimension, sizeof(dimension), 1, fp) != 1 || fwrite(&quant, sizeof(quant), 1, fp) != 1)
	{ fclose(fp); remove(temp); return 1; }
fclose(fp);
if (rename(temp, filename) != 0)
	{ remove(temp); return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_RERANK_CONFIG()
	-----------------------------------------------
	Configure the index-wide late-interaction rerank multi-vector dimension +
	quantization mode.  Requires the index to be open.  Once set, both are
	immutable: setting the SAME (dimension, quant) pair again is a no-op
	success (idempotent), but any change is rejected.  Mirrors
	set_quantization()'s "first enable wins, reject different" contract.
*/
long ATIRE_segment_index::set_rerank_config(long long dimension, long quant)
{
if (directory == NULL)
	return 1;					// must be open
if (dimension < 1 || dimension > 65536)
	return 1;
if (quant != RERANK_QUANT_FLOAT && quant != RERANK_QUANT_INT8)
	return 1;
if (quant == RERANK_QUANT_INT8 && multivector_pq_configured())
	return 1;					// mutually exclusive with token-PQ (set_multivector_pq_config())
if (rerank_dimension_current != 0)			// already set: immutable
	return (rerank_dimension_current == dimension && rerank_quant_current == quant) ? 0 : 1;
rerank_dimension_current = dimension;
rerank_quant_current = quant;
if (save_rerank_config() != 0)
	{ rerank_dimension_current = 0; rerank_quant_current = 0; return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::LOAD_ATTRIBUTES_CONFIG()
	------------------------------------------------
	Reads <dir>/attributes.config (magic/version/field_count/(name,type,multi)*).
	Absent => leaves attribute_schema_current unchanged (unconfigured).
	Garbage => treated as absent (defensive parse, mirrors load_rerank_config).
*/
long ATIRE_segment_index::load_attributes_config(void)
{
char filename[4096];
FILE *fp;
unsigned long long magic, want;
unsigned int version;
long long field_count, i;
char name[64];
int32_t type, multi;
const char *tag = "ANTATTR1";
ANT_attribute_schema schema;

memcpy(&want, tag, 8);
snprintf(filename, sizeof(filename), "%s/attributes.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != want
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != 1u
	|| fread(&field_count, sizeof(field_count), 1, fp) != 1
	|| field_count < 0 || field_count > ANT_attribute_schema::MAX_FIELDS)
	{ fclose(fp); return 0; }

for (i = 0; i < field_count; i++)
	{
	if (fread(name, sizeof(name), 1, fp) != 1
		|| fread(&type, sizeof(type), 1, fp) != 1
		|| fread(&multi, sizeof(multi), 1, fp) != 1)
		{ fclose(fp); return 0; }
	name[sizeof(name) - 1] = '\0';
	if (schema.add_field(name, (int)type, (int)multi) != 0)
		{ fclose(fp); return 0; }
	}
fclose(fp);
attribute_schema_current = schema;
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SAVE_ATTRIBUTES_CONFIG()
	------------------------------------------------
	Atomic write (temp + rename) of the index-wide attribute schema config.
*/
long ATIRE_segment_index::save_attributes_config(void)
{
char filename[4096], temp[4200];
FILE *fp;
unsigned long long magic;
unsigned int version = 1u;
long long field_count = attribute_schema_current.count(), i;
char name[64];
int32_t type, multi;
const char *tag = "ANTATTR1";

memcpy(&magic, tag, 8);
snprintf(filename, sizeof(filename), "%s/attributes.config", directory);
if (snprintf(temp, sizeof(temp), "%s.tmp", filename) >= (int)sizeof(temp))
	return 1;
if ((fp = fopen(temp, "wb")) == NULL)
	return 1;
if (fwrite(&magic, sizeof(magic), 1, fp) != 1 || fwrite(&version, sizeof(version), 1, fp) != 1
	|| fwrite(&field_count, sizeof(field_count), 1, fp) != 1)
	{ fclose(fp); remove(temp); return 1; }
for (i = 0; i < field_count; i++)
	{
	memset(name, 0, sizeof(name));
	strncpy(name, attribute_schema_current.name(i), sizeof(name) - 1);
	type = (int32_t)attribute_schema_current.type(i);
	multi = (int32_t)attribute_schema_current.is_multi(i);
	if (fwrite(name, sizeof(name), 1, fp) != 1 || fwrite(&type, sizeof(type), 1, fp) != 1
		|| fwrite(&multi, sizeof(multi), 1, fp) != 1)
		{ fclose(fp); remove(temp); return 1; }
	}
fclose(fp);
if (rename(temp, filename) != 0)
	{ remove(temp); return 1; }
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::SET_ATTRIBUTES_CONFIG()
	-------------------------------------------------
	Configure the index-wide filterable attribute schema.  Requires the index
	to be open.  Once set (non-empty), it is immutable: setting the SAME
	schema again is a no-op success (idempotent), but any change is rejected.
	Mirrors set_rerank_config()'s "first enable wins, reject different"
	contract.
*/
long ATIRE_segment_index::set_attributes_config(const ANT_attribute_schema &schema)
{
if (directory == NULL)
	return 1;					// must be open
if (schema.count() == 0)
	return 1;					// reject empty schema
if (attribute_schema_current.count() != 0)		// already set: immutable
	return attribute_schema_current.equals(schema) ? 0 : 1;
attribute_schema_current = schema;
if (save_attributes_config() != 0)
	{ attribute_schema_current = ANT_attribute_schema(); return 1; }
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
	float *recon_buf = new float[vector_dimension_current];
	long failed = sig_writer.create(vsig_name, signature_bits_current) != 0;
	for (docid = 0; !failed && docid < docs; docid++)
		{
		if (vectors->has(docid))
			{ vectors->reconstruct(docid, recon_buf); query_signer->sign(recon_buf, sig); failed = sig_writer.append(sig) != 0; }
		else
			failed = sig_writer.append(NULL) != 0;
		}
	if (!failed) sig_writer.finish(); else sig_writer.abandon();
	delete [] recon_buf;
	delete [] sig;
	delete vectors;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::BUILD_HNSW()
	---------------------------------
	Idempotent backfill: for every manifested disk segment with vectors but no
	valid .hnsw, build a fresh HNSW graph sidecar (so HNSW can be enabled on an
	index whose segments predate it).  Per-segment failures are skipped (that
	segment stays graph-less / exact-scanned), never left corrupt.  Returns 0
	on success (1 if HNSW is unconfigured).
*/
long ATIRE_segment_index::build_hnsw(void)
{
long long which;
char hnsw_name[4096];

if (hnsw_M_current == 0)
	return 1;

for (which = 0; which < segment_count; which++)
	{
	long long generation = segments[which].generation;
	long long docs = segments[which].engine->get_document_count();

	segment_filename(hnsw_name, sizeof(hnsw_name), generation, "hnsw");
	ANT_hnsw *existing = ANT_hnsw::load(hnsw_name, hnsw_M_current, hnsw_ef_construction_current, docs);
	long long already = existing->node_count() == docs && docs > 0 && !existing->empty();
	delete existing;
	if (already)
		continue;

	/* Build over the resident tier source (float / int8 .pqr / pq_vectors under NONE). */
	ANT_vector_source *src = segments[which].vectors != NULL
		? (ANT_vector_source *)segments[which].vectors
		: (ANT_vector_source *)segments[which].pq_vectors;
	if (src != NULL && src->document_count() == docs && docs > 0)
		{
		ANT_hnsw graph;
		if (graph.build(src, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0 && graph.save(hnsw_name) == 0)
			{
			/* Refresh the in-memory graph too, mirroring build_pq()/build_token_index()'s
			   convention -- so a same-session backfill is usable immediately, not just on reopen. */
			delete segments[which].hnsw_graph;
			segments[which].hnsw_graph = ANT_hnsw::load(hnsw_name, hnsw_M_current, hnsw_ef_construction_current, docs);
			}
		}
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::BUILD_TOKEN_INDEX()
	--------------------------------------------
	Idempotent backfill: for every disk segment with a multi-vector store but
	no valid .tann, build a fresh V6 token-ANN graph sidecar (so a segment
	that has been search_multivector()-served by brute-force MaxSim so far
	can be upgraded to the graph path).  Mirrors build_hnsw()'s per-segment
	loop.  Per-segment failures (empty store, over-cap token count, build/save
	failure) are skipped -- that segment simply stays on the brute-force
	MaxSim fallback.  Returns 0 on success (1 if rerank/multi-vectors are
	unconfigured).
*/
long ATIRE_segment_index::build_token_index(void)
{
long long which;
char tann_name[4096];

if (!rerank_configured())
	return 1;

for (which = 0; which < segment_count; which++)
	{
	if (segments[which].token_source == NULL || segments[which].token_source->document_count() == 0)
		continue;	/* no resident token source (float dropped AND no .mvpq) */
	if (segments[which].token_index != NULL && !segments[which].token_index->empty())
		continue;	/* already built */

	segment_filename(tann_name, sizeof(tann_name), segments[which].generation, "tann");
	ANT_token_index *idx = ANT_token_index::build(segments[which].token_source, token_index_M, token_index_ef_construction, ANT_vector_store::METRIC_DOT);
	if (idx == NULL)
		continue;	/* empty/over-cap/failed -> segment stays in brute-force fallback */
	if (idx->save(tann_name) != 0)
		{ delete idx; continue; }	/* save failed -> fallback */
	delete segments[which].token_index;
	segments[which].token_index = idx;	/* built index retains the segment's multivectors store; usable immediately */
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::BUILD_QUANTIZED()
	----------------------------------------
	Idempotent backfill: rewrites each float .vec disk segment as an int8
	.qvec (replace mode), mirroring build_hnsw()'s per-segment loop.  Only
	meaningful once quantization is configured in QUANTIZE_REPLACE mode;
	segments already int8-backed are skipped.  On success the float sidecar
	is removed and the in-memory segment reloads from the new .qvec.
*/
long ATIRE_segment_index::build_quantized(void)
{
long long which;
char vec_name[4096], qvec_name[4096];

if (quantization_current != QUANTIZE_REPLACE)
	return 1;					/* only replace-mode backfill is defined here */

for (which = 0; which < segment_count; which++)
	{
	long long generation = segments[which].generation;
	long long docs = segments[which].engine->get_document_count();

	if (segments[which].vectors != NULL && segments[which].vectors->is_quantized())
		continue;				/* already int8: idempotent skip */

	segment_filename(vec_name, sizeof(vec_name), generation, "vec");
	segment_filename(qvec_name, sizeof(qvec_name), generation, "qvec");

	ANT_vector_store *src = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
	if (src->document_count() == docs && docs > 0 && !src->is_quantized())
		{
		ANT_vector_store_writer w;
		long failed = w.create(qvec_name, vector_dimension_current) != 0;
		if (!failed)
			w.set_quantization(ANT_vector_store_writer::QUANT_REPLACE);
		float *buf = new float[vector_dimension_current];
		for (long long docid = 0; !failed && docid < docs; docid++)
			{
			if (src->has(docid))
				{ src->reconstruct(docid, buf); failed = w.append(buf) != 0; }
			else
				failed = w.append(NULL) != 0;
			}
		delete [] buf;
		if (!failed)
			failed = w.finish() != 0;
		if (!failed)
			{
			remove(vec_name);				/* replace: drop the float sidecar */
			delete segments[which].vectors;	/* reload the segment's store as int8 */
			segments[which].vectors = ANT_vector_store::load(qvec_name, vector_dimension_current, docs);
			}
		}
	delete src;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::BUILD_PQ()
	-------------------------------
	On-demand backfill: for every open segment with a dense float .vec and no
	valid .pq, train per-segment codebooks + encode + write the .pq sidecar,
	then swap the in-memory PQ store.  The float .vec is KEPT (PQ replace still
	retains the resident float as a fallback/rerank tier in Phase 1).  Per-segment
	failures skip (segment stays float/int8-backed), never corrupt.  Idempotent.
	Returns 0 on success, 1 if PQ is unconfigured or the index has no vectors.
*/
long ATIRE_segment_index::build_pq(void)
{
long long which;
char vec_name[4096], pq_name[4096], pqr_name[4096];

if (!pq_configured() || vector_dimension_current == 0)
	return 1;

for (which = 0; which < segment_count; which++)
	{
	long long generation = segments[which].generation;
	long long docs = segments[which].engine->get_document_count();

	segment_filename(vec_name, sizeof(vec_name), generation, "vec");
	segment_filename(pq_name, sizeof(pq_name), generation, "pq");

	/*
		1. Build the .pq codes if this segment lacks a valid one (idempotent:
		   a segment that already has a valid .pq skips the retrain but STILL
		   falls through to the resident-tier realization below -- so a tier
		   set AFTER the .pq was built is honoured, not silently dropped).
	*/
	int have_pq = (segments[which].pq_vectors != NULL && segments[which].pq_vectors->document_count() == docs && docs > 0);
	if (!have_pq)
		{
		ANT_vector_store *src = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
		if (src->document_count() == docs && docs > 0 && !src->is_quantized())
			{
			ANT_pq_store_writer w;
			long failed = w.create(pq_name, vector_dimension_current, pq_m_current, pq_k_current, vector_metric, pq_opq_current) != 0;
			if (!failed && pq_global_current)
				{
				/*
					Global mode: reuse the shared codebook (training it now, from
					THIS segment's floats, if this is the first build this
					session/on-disk). Fail-soft -- if ensure_global_pq_codebook()
					could not train/persist one, fall through and let the writer
					train its own per-segment codebook (no hard build failure).
				*/
				ensure_global_pq_codebook((long)which);
				if (global_pq_codebook != NULL)
					w.set_external_codebook(global_pq_codebook, global_pq_rotation);
				}
			float *buf = new float[vector_dimension_current];
			for (long long docid = 0; !failed && docid < docs; docid++)
				{
				if (src->has(docid))
					{ src->reconstruct(docid, buf); failed = w.append(buf) != 0; }
				else
					failed = w.append(NULL) != 0;
				}
			delete [] buf;
			if (!failed)
				failed = w.finish() != 0;
			if (failed)
				w.abandon();
			else
				{
				delete segments[which].pq_vectors;
				segments[which].pq_vectors = load_segment_pq_vectors(pq_name, docs);
				have_pq = (segments[which].pq_vectors != NULL && segments[which].pq_vectors->document_count() == docs && docs > 0);
				}
			}
		delete src;
		}

	if (!have_pq)
		continue;				/* no PQ codes (empty/degraded .vec): leave this segment float-backed */

	/*
		2. Realize the resident tier for this segment IN-SESSION so the RAM /
		   precision win takes effect without waiting for a reopen:
		     INT8  -> ensure the int8 .pqr sidecar exists (write it from the
		              float .vec if missing/degraded), then make it the resident
		              rerank store and drop the float;
		     NONE  -> drop the resident float (pure ADC, codes only);
		     FLOAT -> keep the float resident (nothing to do).
		   The float .vec is NEVER removed from disk (build/compaction retrain
		   read it, and a degraded .pqr falls back to reconstruct-from-PQ).
	*/
	if (pq_resident_tier_current == PQ_TIER_INT8)
		{
		segment_filename(pqr_name, sizeof(pqr_name), generation, "pqr");
		ANT_vector_store *iv = ANT_vector_store::load(pqr_name, vector_dimension_current, docs);
		if (!(iv->document_count() == docs && docs > 0 && iv->is_quantized()))
			{	/* missing/degraded .pqr -> (re)write from the float .vec (best-effort), then reload */
			delete iv;
			ANT_vector_store *fsrc = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
			if (fsrc->document_count() == docs && docs > 0 && !fsrc->is_quantized())
				{
				ANT_vector_store_writer qw;
				long qfailed = qw.create(pqr_name, vector_dimension_current) != 0;
				if (!qfailed)
					qw.set_quantization(ANT_vector_store_writer::QUANT_REPLACE);
				float *qbuf = new float[vector_dimension_current];
				for (long long docid = 0; !qfailed && docid < docs; docid++)
					{
					if (fsrc->has(docid))
						{ fsrc->reconstruct(docid, qbuf); qfailed = qw.append(qbuf) != 0; }
					else
						qfailed = qw.append(NULL) != 0;
					}
				delete [] qbuf;
				if (!qfailed)
					qfailed = qw.finish() != 0;	/* writes int8 store to .pqr; float .vec is NOT removed */
				if (qfailed)
					qw.abandon();
				}
			delete fsrc;
			iv = ANT_vector_store::load(pqr_name, vector_dimension_current, docs);
			}
		delete segments[which].vectors;
		if (iv->document_count() == docs && docs > 0 && iv->is_quantized())
			segments[which].vectors = iv;		/* resident int8 rerank tier (float dropped) */
		else
			{ delete iv; segments[which].vectors = NULL; }	/* .pqr still degraded -> reconstruct-from-PQ at rerank */
		}
	else if (pq_resident_tier_current == PQ_TIER_NONE)
		{
		delete segments[which].vectors;
		segments[which].vectors = NULL;			/* pure ADC: nothing resident beside the codes */
		}
	/* FLOAT: leave segments[which].vectors (the float .vec) resident. */
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::DISK_SEGMENT_HAS_PQ()
	------------------------------------------
	Test accessor: 1 if segment `which` has a non-empty PQ store.
*/
long ATIRE_segment_index::disk_segment_has_pq(long long which)
{
return (which >= 0 && which < segment_count && segments[which].pq_vectors != NULL && segments[which].pq_vectors->document_count() > 0) ? 1 : 0;
}

/*
	ATIRE_SEGMENT_INDEX::DISK_SEGMENT_RESIDENT_TIER()
	--------------------------------------------------
	Test accessor: which resident tier segment `which`'s loaded vector store
	actually is (int8 store -> PQ_TIER_INT8, float store -> PQ_TIER_FLOAT,
	NULL -> PQ_TIER_NONE), or -1 if `which` is out of range.  This reflects the
	store CURRENTLY resident, which matches the configured tier once the segment
	has been through build_pq()/compaction (both realize the tier in-session) or
	a reopen.  Between an ondemand flush() and the next build_pq(), a PQ segment
	is still float-backed and reads as FLOAT regardless of the configured tier.
*/
long ATIRE_segment_index::disk_segment_resident_tier(long long which)
{
if (which < 0 || which >= segment_count)
	return -1;
if (segments[which].vectors == NULL)
	return PQ_TIER_NONE;
return segments[which].vectors->is_quantized() ? PQ_TIER_INT8 : PQ_TIER_FLOAT;
}

/*
	ATIRE_SEGMENT_INDEX::BUILD_MULTIVECTOR_PQ()
	-------------------------------------------
	On-demand backfill: for every segment with a float .mvec token pool and no
	valid .mvpq, train a per-segment codebook over the token pool + encode +
	write the sidecar, then swap the in-memory store.  The .mvec is KEPT resident.
	Per-segment failures skip; idempotent.  Returns 0, or 1 if token-PQ is
	unconfigured / no multivectors.
*/
long ATIRE_segment_index::build_multivector_pq(void)
{
long long which;
char mvpq_name[4096];

if (!multivector_pq_configured() || rerank_dimension_current == 0)
	return 1;

for (which = 0; which < segment_count; which++)
	{
	long long docs = segments[which].engine->get_document_count();
	ANT_multivector_store *mv = segments[which].multivectors;
	ANT_multivector_store *mv_disk = NULL;			/* loaded from disk when float pool not resident (NONE tier) */
	if (mv == NULL)
		{
		char mvec_name[4096];
		segment_filename(mvec_name, sizeof(mvec_name), segments[which].generation, "mvec");
		mv_disk = ANT_multivector_store::load(mvec_name, rerank_dimension_current, docs);
		if (mv_disk->document_count() == docs && mv_disk->token_count() > 0 && !mv_disk->tokens_quantized())
			mv = mv_disk;
		}
	if (mv == NULL || mv->tokens_quantized())
		{ delete mv_disk; continue; }
	if (segments[which].multivector_pq != NULL
		&& segments[which].multivector_pq->document_count() == docs
		&& segments[which].multivector_pq->token_count() > 0)
		{ delete mv_disk; continue; }

	segment_filename(mvpq_name, sizeof(mvpq_name), segments[which].generation, "mvpq");
	ANT_multivector_pq_store_writer w;
	long failed = w.create(mvpq_name, rerank_dimension_current, mvpq_m_current, ANT_pq_codec::METRIC_DOT) != 0;
	long long cap = mv->max_vector_count();
	float *buf = new float[(cap > 0 ? cap : 1) * rerank_dimension_current];
	for (long long d = 0; !failed && d < docs; d++)
		{
		long long md = mv->copy_vectors(d, buf);
		failed = w.append(md > 0 ? buf : NULL, md) != 0;
		}
	delete [] buf;
	if (!failed)
		failed = w.finish() != 0;
	if (failed)
		w.abandon();
	else
		{
		delete segments[which].multivector_pq;
		segments[which].multivector_pq = ANT_multivector_pq_store::load(mvpq_name, rerank_dimension_current, docs, ANT_pq_codec::METRIC_DOT);
		}
	delete mv_disk;
	}
return 0;
}

/*
	ATIRE_SEGMENT_INDEX::DISK_SEGMENT_HAS_MULTIVECTOR_PQ()
	-----------------------------------------------------
*/
long ATIRE_segment_index::disk_segment_has_multivector_pq(long long which)
{
return (which >= 0 && which < segment_count && segments[which].multivector_pq != NULL && segments[which].multivector_pq->token_count() > 0) ? 1 : 0;
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
	ATIRE_SEGMENT_INDEX::WRITER_MULTIVECTOR_APPEND()
	--------------------------------------------------
	Task 4: capture-only.  Keeps the per-writer-docid multi-vector row count
	(writer_multivector_counts[docid]) parallel to the writer's docids, mirroring
	writer_vector_append()'s growth pattern, and appends `num` rows (each of
	rerank_dimension_current floats) into the flat, ever-growing
	writer_multivector_data pool.  Each row is normalized in place with
	ANT_vector_store::normalize(); a zero row is left as zeros (normalize()
	returning nonzero is NOT a rejection here -- unlike the single, doc-level
	vector's cosine path, an all-zero multi-vector row is a legitimate, if
	inert, row).  Called from add_document_core() right after
	writer_vector_append(), i.e. before writer_documents is incremented, so
	docid here is 0-based and already final.  Flush/search over this buffer
	are later tasks; for now the data is captured and observable only via
	writer_multivector_count_for_test().
*/
long ATIRE_segment_index::writer_multivector_append(long long docid, const float *multivector, long long num_vectors)
{
long long row;
long long appended = (num_vectors > 0 && multivector != NULL && rerank_dimension_current > 0) ? num_vectors : 0;

if (writer_multivector_counts_capacity == 0)
	{
	writer_multivector_counts_capacity = 1024;
	writer_multivector_counts = new int[writer_multivector_counts_capacity];
	memset(writer_multivector_counts, 0, (size_t)(writer_multivector_counts_capacity * sizeof(int)));
	}
if (docid >= writer_multivector_counts_capacity)
	{
	long long new_capacity = writer_multivector_counts_capacity * 2;
	while (docid >= new_capacity)
		new_capacity *= 2;
	int *new_counts = new int[new_capacity];
	memset(new_counts, 0, (size_t)(new_capacity * sizeof(int)));
	memcpy(new_counts, writer_multivector_counts, (size_t)(writer_multivector_counts_capacity * sizeof(int)));
	delete [] writer_multivector_counts;
	writer_multivector_counts = new_counts;
	writer_multivector_counts_capacity = new_capacity;
	}

writer_multivector_counts[docid] = (int)appended;

if (appended > 0)
	{
	if (writer_multivector_capacity == 0)
		{
		writer_multivector_capacity = 1024;
		writer_multivector_data = new float[writer_multivector_capacity * rerank_dimension_current];
		}
	if (writer_multivector_total + appended > writer_multivector_capacity)
		{
		long long new_capacity = writer_multivector_capacity * 2;
		while (writer_multivector_total + appended > new_capacity)
			new_capacity *= 2;
		float *new_data = new float[new_capacity * rerank_dimension_current];
		memcpy(new_data, writer_multivector_data, (size_t)(writer_multivector_total * rerank_dimension_current * sizeof(float)));
		delete [] writer_multivector_data;
		writer_multivector_data = new_data;
		writer_multivector_capacity = new_capacity;
		}

	for (row = 0; row < appended; row++)
		{
		float *dst = writer_multivector_data + writer_multivector_total * rerank_dimension_current;
		memcpy(dst, multivector + row * rerank_dimension_current, (size_t)(rerank_dimension_current * sizeof(float)));
		ANT_vector_store::normalize(dst, rerank_dimension_current);	// zero row -> stays zero, not a rejection
		writer_multivector_total++;
		}
	}

return 0;
}

/*
	ATIRE_SEGMENT_INDEX::WRITER_MULTIVECTOR_COUNT_FOR_TEST()
	------------------------------------------------------------
	Test hook (Task 4): the number of multi-vector rows captured for `docid`
	in the live writer segment.  0 if docid was never given any (or a 0 was
	recorded), -1 if docid is out of range for the current writer.
*/
long long ATIRE_segment_index::writer_multivector_count_for_test(long long docid)
{
if (docid < 0 || docid >= writer_documents)
	return -1;
return (writer_multivector_counts != NULL && docid < writer_multivector_counts_capacity) ? writer_multivector_counts[docid] : 0;
}

/*
	ATIRE_SEGMENT_INDEX::SCAN_LIVE_BUFFER_EXACT()
	---------------------------------------------
	Exact-scan the live memory buffer (never signature/graph-indexed) into best[],
	skipping absent and tombstoned slots.  Shared verbatim by all three
	vector_candidates_* gatherers, so it lives here once.  Expects query already
	normalized in cosine mode (each gatherer normalizes up front).
*/
void ATIRE_segment_index::scan_live_buffer_exact(const float *query, ANT_vector_candidate *best, long long *best_count, long long top_k, const unsigned char *filter_bits)
{
long long docid;

for (docid = 0; docid < writer_documents; docid++)
	{
	if (writer_vector_presence == NULL || !(writer_vector_presence[docid / 8] & (1 << (docid % 8))))
		continue;
	if (writer_tombstones->is_deleted(docid))
		continue;
	if (filter_bits != NULL && !(filter_bits[docid >> 3] & (1 << (docid & 7))))
		continue;
	ANT_vector_candidate_insert(best, best_count, top_k, ANT_vector_store::kernel(query, writer_vector_data + docid * vector_dimension_current, vector_dimension_current, vector_metric), writer_generation, docid);
	}
}

/*
	ATIRE_SEGMENT_INDEX::EVALUATE_FILTER_FOR_SEGMENT()
	--------------------------------------------------
	Evaluate `filter` against disk segment `which`'s attribute store into a
	fresh match bitset (caller frees).  NULL filter => NULL (unfiltered).  A
	NULL/degraded attribute store yields an all-zero bitset (nothing admitted).
*/
unsigned char *ATIRE_segment_index::evaluate_filter_for_segment(long long which, const ANT_filter *filter)
{
if (filter == NULL)
	return NULL;
long long docs = segments[which].engine->get_document_count();
long long bytes = (docs + 7) / 8;
unsigned char *bits = new unsigned char[bytes > 0 ? bytes : 1];
filter->evaluate(segments[which].attributes, docs, bits);	/* attributes NULL/degraded => all-zero => nothing admitted from this segment */
return bits;
}

/*
	ATIRE_SEGMENT_INDEX::EVALUATE_FILTER_FOR_LIVE()
	-----------------------------------------------
	Evaluate `filter` against the live NRT buffer's captured attribute sets into
	a fresh match bitset (caller frees).  NULL filter => NULL (unfiltered).
*/
unsigned char *ATIRE_segment_index::evaluate_filter_for_live(const ANT_filter *filter)
{
if (filter == NULL)
	return NULL;
long long docs = writer_documents;
long long bytes = (docs + 7) / 8;
unsigned char *bits = new unsigned char[bytes > 0 ? bytes : 1];
filter->evaluate_live(writer_attribute_sets, writer_attribute_sets_capacity, docs, bits);
return bits;
}

/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES()
	----------------------------------------
	Exact top-k across every disk segment's vector store and the live memory
	buffer.  In cosine mode the query is normalized here (stored vectors were
	normalized at insertion -- see add_document_core()).  Returns the
	candidate count; caller supplies best[top_k].
*/
long long ATIRE_segment_index::vector_candidates(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter)
{
long long which, best_count = 0;
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
		{
		ANT_vector_store *src = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
		unsigned char *fbits = evaluate_filter_for_segment(which, filter);
		src->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
		delete [] fbits;
		}

unsigned char *lbits = evaluate_filter_for_live(filter);
scan_live_buffer_exact(query, best, &best_count, top_k, lbits);
delete [] lbits;

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
	ATIRE_SEGMENT_INDEX::SEARCH_VECTOR_IMPL()
	-----------------------------------------
	Unified dense-vector top-k across the live memory buffer and every open disk
	segment.  The mode selects the candidate gatherer (exact / signature-prefiltered
	/ HNSW graph); the downstream (sort + resolve + publish) is identical for all
	three, so it lives here once.  Mirrors search()'s results[] contract (prior
	hits' filenames freed at entry) -- results[]/results_count are shared with
	search(); only one of the two result sets exists at a time.
*/
long long ATIRE_segment_index::search_vector_impl(const float *query, long long top_k, vector_search_mode mode, const ANT_filter *filter)
{
char filename_buffer[4096];
long long which, count;
ANT_vector_candidate *best;

reset_results();

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;

best = new ANT_vector_candidate[top_k];
count = mode == VECTOR_MODE_APPROX ? vector_candidates_approx(query, top_k, best, filter)
	: mode == VECTOR_MODE_HNSW ? vector_candidates_hnsw(query, top_k, best, filter)
	: mode == VECTOR_MODE_PQ ? vector_candidates_pq(query, top_k, best, filter)
	: mode == VECTOR_MODE_PQ_RERANK ? vector_candidates_pq_rerank(query, top_k, best, filter)
	: vector_candidates(query, top_k, best, filter);
qsort(best, (size_t)count, sizeof(*best), vector_candidate_compare);

for (which = 0; which < count; which++)
	{
	char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));

	hit *slot = append_result();

	slot->generation = best[which].generation;
	slot->docid = best[which].docid;
	populate_hit_payload(slot);
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
	ATIRE_SEGMENT_INDEX::SEARCH_VECTOR()
	--------------------------------------
	Exact top-k dense-vector search across the live memory buffer and every
	open disk segment's vector store.
*/
long long ATIRE_segment_index::search_vector(const float *query, long long top_k)
{
vector_search_mode mode = VECTOR_MODE_EXACT;
if (pq_configured())
	mode = (pq_posture_current == PQ_POSTURE_REPLACE) ? VECTOR_MODE_PQ : VECTOR_MODE_PQ_RERANK;
return search_vector_impl(query, top_k, mode);
}

long long ATIRE_segment_index::search_vector(const float *query, long long top_k, const ANT_filter *filter)
{
vector_search_mode mode = VECTOR_MODE_EXACT;
if (pq_configured())
	mode = (pq_posture_current == PQ_POSTURE_REPLACE) ? VECTOR_MODE_PQ : VECTOR_MODE_PQ_RERANK;
return search_vector_impl(query, top_k, mode, filter);
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
long long ATIRE_segment_index::vector_candidates_approx(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter)
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
	ANT_vector_store *src = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
	unsigned char *fbits = evaluate_filter_for_segment(which, filter);
	if (segments[which].signatures != NULL && segments[which].signatures->document_count() == segments[which].engine->get_document_count())
		{
		long long count = 0, p;
		segments[which].signatures->shortlist(query_sig, segments[which].tombstones, pool_size, pool, &count);
		for (p = 0; p < count; p++)
			{
			docid = pool[p];
			if (!src->has(docid))
				continue;
			if (fbits != NULL && !(fbits[docid >> 3] & (1 << (docid & 7))))
				continue;
			ANT_vector_candidate_insert(best, &best_count, top_k,
				src->score(docid, query, vector_metric),
				segments[which].generation, docid);
			}
		}
	else
		src->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
	delete [] fbits;
	}

unsigned char *lbits = evaluate_filter_for_live(filter);
scan_live_buffer_exact(query, best, &best_count, top_k, lbits);		// live memory buffer: always exact (never signature-indexed)
delete [] lbits;

delete [] normalized;
delete [] query_sig;
delete [] pool;
return best_count;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_VECTOR_APPROX()
	-------------------------------------------
	Signature-prefiltered dense-vector top-k: search_vector_impl() with the
	APPROX gatherer, so approximate and exact rankings are identical modulo the
	shortlist.  Transparently falls back to the exact path when approximate is
	unconfigured or the metric is L2 (SimHash tracks angular, not Euclidean,
	distance) -- giving byte-identical results in those cases.
*/
long long ATIRE_segment_index::search_vector_approx(const float *query, long long top_k)
{
if (signature_bits_current == 0 || query_signer == NULL || vector_metric == VECTOR_METRIC_L2)
	return search_vector_impl(query, top_k, VECTOR_MODE_EXACT);		// transparent fallback
return search_vector_impl(query, top_k, VECTOR_MODE_APPROX);
}

long long ATIRE_segment_index::search_vector_approx(const float *query, long long top_k, const ANT_filter *filter)
{
if (signature_bits_current == 0 || query_signer == NULL || vector_metric == VECTOR_METRIC_L2)
	return search_vector_impl(query, top_k, VECTOR_MODE_EXACT, filter);		// transparent fallback still filters
return search_vector_impl(query, top_k, VECTOR_MODE_APPROX, filter);
}

/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES_HNSW()
	---------------------------------------------
	Like vector_candidates(), but each disk segment WITH a valid cached HNSW
	graph is navigated approximately (ef = max(hnsw_ef_search, top_k)); the graph
	returns exactly-scored (kernel) candidates so there is NO separate rerank.
	Segments without a usable graph, and the live memory buffer, are exact-scanned.
	Caller guarantees metric != DOT and HNSW is configured.
*/
long long ATIRE_segment_index::vector_candidates_hnsw(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter)
{
long long which, best_count = 0;
long long ef = hnsw_ef_search < top_k ? top_k : hnsw_ef_search;
float *normalized = NULL;
long long *cand_docids = new long long[ef > 0 ? ef : 1];
double *cand_scores = new double[ef > 0 ? ef : 1];

if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{ delete [] normalized; delete [] cand_docids; delete [] cand_scores; return 0; }
	query = normalized;
	}

for (which = 0; which < segment_count; which++)
	{
	ANT_vector_source *src = segments[which].vectors != NULL
		? (ANT_vector_source *)segments[which].vectors
		: (ANT_vector_source *)segments[which].pq_vectors;
	if (src == NULL)
		continue;							/* NONE with no pq_vectors: nothing resident to score */
	unsigned char *fbits = evaluate_filter_for_segment(which, filter);
	if (segments[which].hnsw_graph != NULL && !segments[which].hnsw_graph->empty()
		&& segments[which].hnsw_graph->node_count() == segments[which].engine->get_document_count())
		{
		long long c = segments[which].hnsw_graph->search(query, vector_metric, ef, ef,
			src, segments[which].tombstones, cand_docids, cand_scores, fbits);
		for (long long p = 0; p < c; p++)
			{
			double sc = segments[which].exact_vectors != NULL
				? segments[which].exact_vectors->score(cand_docids[p], query, vector_metric)
				: cand_scores[p];			/* PQ tiers: exact_vectors is NULL -> graph nav score (ADC/int8/float) */
			ANT_vector_candidate_insert(best, &best_count, top_k, sc, segments[which].generation, cand_docids[p]);
			}
		}
	else if (segments[which].vectors != NULL)		/* no graph: exact/int8 linear scan */
		{
		ANT_vector_store *s = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
		s->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
		}
	else if (segments[which].pq_vectors != NULL)	/* NONE, no graph: linear ADC scan */
		segments[which].pq_vectors->scan_adc(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
	delete [] fbits;
	}

unsigned char *lbits = evaluate_filter_for_live(filter);
scan_live_buffer_exact(query, best, &best_count, top_k, lbits);		// live memory buffer: always exact (never graph-indexed)
delete [] lbits;

delete [] normalized;
delete [] cand_docids;
delete [] cand_scores;
return best_count;
}

/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES_PQ()
	-------------------------------------------
	Replace-posture PQ gatherer: rank on the ADC (asymmetric distance) score
	directly.  Each disk segment WITH a valid .pq store is scanned via ADC; any
	segment lacking one (not yet built, or degraded on load) falls back to an
	exact float/int8 scan of its dense store, so results are always well-formed.
	The live memory buffer is always scanned exactly (never PQ-encoded).  Cosine
	is dot on normalized data, so the query is normalized here exactly as the
	other gatherers do.  Returns the candidate count; caller supplies best[top_k].
*/
long long ATIRE_segment_index::vector_candidates_pq(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter)
{
long long which, best_count = 0;
float *normalized = NULL;

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	return 0;

if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{ delete [] normalized; return 0; }
	query = normalized;
	}

for (which = 0; which < segment_count; which++)
	{
	unsigned char *fbits = evaluate_filter_for_segment(which, filter);
	if (segments[which].pq_vectors != NULL
		&& segments[which].pq_vectors->document_count() == segments[which].engine->get_document_count()
		&& segments[which].pq_vectors->document_count() > 0)
		segments[which].pq_vectors->scan_adc(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
	else if (segments[which].vectors != NULL)
		{
		ANT_vector_store *src = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
		src->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
		}
	delete [] fbits;
	}

unsigned char *lbits = evaluate_filter_for_live(filter);
scan_live_buffer_exact(query, best, &best_count, top_k, lbits);
delete [] lbits;

delete [] normalized;
return best_count;
}

/*
	ATIRE_SEGMENT_INDEX::VECTOR_CANDIDATES_PQ_RERANK()
	--------------------------------------------------
	Rerank-posture PQ gatherer: per segment, take a PQ ADC shortlist of
	top_k*candidate_multiplier candidates, then RESCORE each to restore
	precision, keeping only the exact top_k.  The rescore source is whatever
	the #19 resident tier populated in segments[].vectors -- float under
	PQ_TIER_FLOAT, or the int8 .pqr sidecar under PQ_TIER_INT8.  When no
	resident store is present for a shortlisted docid (PQ_TIER_INT8 with a
	missing .pqr, or PQ_TIER_NONE), the candidate is reconstructed from the
	.pq codes instead and scored with the exact metric kernel over that
	approximation.  Segments without a valid .pq and without a resident store
	are skipped; segments without .pq but WITH a resident store fall back to
	an exact scan of that store.  The live buffer is always exact.  Mirrors
	vector_candidates_approx (signature shortlist -> exact/approx rescore).
*/
long long ATIRE_segment_index::vector_candidates_pq_rerank(const float *query, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter)
{
long long which, p, best_count = 0, pool_size = top_k * candidate_multiplier;
float *normalized = NULL;
ANT_vector_candidate *shortlist = new ANT_vector_candidate[pool_size > 0 ? pool_size : 1];

if (vector_dimension_current == 0 || query == NULL || top_k < 1)
	{ delete [] shortlist; return 0; }

if (vector_metric == VECTOR_METRIC_COSINE)
	{
	normalized = new float[vector_dimension_current];
	memcpy(normalized, query, (size_t)(vector_dimension_current * sizeof(float)));
	if (ANT_vector_store::normalize(normalized, vector_dimension_current) != 0)
		{ delete [] normalized; delete [] shortlist; return 0; }
	query = normalized;
	}

for (which = 0; which < segment_count; which++)
	{
	ANT_pq_store *pq = segments[which].pq_vectors;
	long long docs = segments[which].engine->get_document_count();
	int pq_ok = (pq != NULL && pq->document_count() == docs && docs > 0);
	if (!pq_ok && segments[which].vectors == NULL)
		continue;								/* no PQ codes and no resident store: nothing to scan */
	unsigned char *fbits = evaluate_filter_for_segment(which, filter);
	if (pq_ok)
		{
		ANT_vector_store *src = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
		long long count = 0;
		pq->scan_adc(query, vector_metric, segments[which].tombstones, segments[which].generation, shortlist, &count, pool_size, fbits);
		float *recon = NULL;
		for (p = 0; p < count; p++)
			{
			long long docid = shortlist[p].docid;
			double score;
			if (src != NULL && src->has(docid))
				score = src->score(docid, query, vector_metric);		/* resident float or int8 rescore */
			else
				{
				if (recon == NULL) recon = new float[vector_dimension_current];
				pq->reconstruct(docid, recon);							/* .pqr absent -> ADC-precision reconstruct */
				score = ANT_vector_store::kernel(recon, query, vector_dimension_current, vector_metric);
				}
			ANT_vector_candidate_insert(best, &best_count, top_k, score, segments[which].generation, docid);
			}
		delete [] recon;
		}
	else
		segments[which].vectors->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
	delete [] fbits;
	}

unsigned char *lbits = evaluate_filter_for_live(filter);
scan_live_buffer_exact(query, best, &best_count, top_k, lbits);
delete [] lbits;

delete [] normalized;
delete [] shortlist;
return best_count;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_VECTOR_HNSW()
	-----------------------------------------
	HNSW-graph dense-vector top-k: search_vector_impl() with the HNSW gatherer,
	so exact and graph rankings are identical modulo the graph navigation.
	Transparently falls back to the exact path when HNSW is unconfigured or the
	metric is DOT (unnormalized dot has no bounded kernel for graph navigation) --
	giving byte-identical results in those cases.
*/
long long ATIRE_segment_index::search_vector_hnsw(const float *query, long long top_k)
{
if (hnsw_M_current == 0 || vector_metric == VECTOR_METRIC_DOT)
	return search_vector_impl(query, top_k, VECTOR_MODE_EXACT);		// transparent fallback (dot / unconfigured)
return search_vector_impl(query, top_k, VECTOR_MODE_HNSW);
}

long long ATIRE_segment_index::search_vector_hnsw(const float *query, long long top_k, const ANT_filter *filter)
{
if (hnsw_M_current == 0 || vector_metric == VECTOR_METRIC_DOT)
	return search_vector_impl(query, top_k, VECTOR_MODE_EXACT, filter);	/* transparent fallback still filters (Task 9 exact path) */
return search_vector_impl(query, top_k, VECTOR_MODE_HNSW, filter);
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
	ATIRE_SEGMENT_INDEX::SEARCH_HYBRID_IMPL()
	-----------------------------------------
	Reciprocal Rank Fusion of the lexical top-k and the vector top-k:
	fused(d) = sum over lists containing d of 1 / (60 + rank_d), ranks
	1-based.  60 is the standard RRF constant.  Either side may be absent;
	the result degrades to the other side (still RRF-scored, order preserved).
	The mode selects the vector-leg gatherer (exact / signature-prefiltered /
	HNSW graph); the lexical leg, fusion math, and publish are identical for all
	three, so they live here once.

	The candidate and its filename are carried together in a single
	ANT_fused_candidate[] array (see struct above, just up) so that qsort()
	cannot desynchronize them; a parallel filename array would move the
	ANT_vector_candidate rows without moving the corresponding filename rows.
*/
long long ATIRE_segment_index::search_hybrid_impl(char *query_text, const float *query_vector, long long top_k, vector_search_mode mode, const ANT_filter *filter)
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
	lexical_count = search(query_text, top_k, filter);
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
	vector_count = mode == VECTOR_MODE_APPROX ? vector_candidates_approx(query_vector, top_k, best, filter)
		: mode == VECTOR_MODE_HNSW ? vector_candidates_hnsw(query_vector, top_k, best, filter)
		: vector_candidates(query_vector, top_k, best, filter);
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
	populate_hit_payload(slot);
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
	ATIRE_SEGMENT_INDEX::SEARCH_HYBRID()
	------------------------------------
	RRF fusion of the lexical top-k and the EXACT vector top-k.
*/
long long ATIRE_segment_index::search_hybrid(char *query_text, const float *query_vector, long long top_k)
{
return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_EXACT);
}

long long ATIRE_segment_index::search_hybrid(char *query_text, const float *query_vector, long long top_k, const ANT_filter *filter)
{
return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_EXACT, filter);
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_HYBRID_APPROX()
	--------------------------------------------
	search_hybrid_impl() with the APPROX vector-leg gatherer (signature-
	prefiltered) instead of exact; RRF math, lexical leg, and publish are
	unchanged.  Transparently falls back to the exact fusion when approximate is
	unconfigured or the metric is L2 (SimHash tracks angular, not Euclidean,
	distance).
*/
long long ATIRE_segment_index::search_hybrid_approx(char *query_text, const float *query_vector, long long top_k)
{
if (signature_bits_current == 0 || query_signer == NULL || vector_metric == VECTOR_METRIC_L2)
	return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_EXACT);		// transparent fallback
return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_APPROX);
}

long long ATIRE_segment_index::search_hybrid_approx(char *query_text, const float *query_vector, long long top_k, const ANT_filter *filter)
{
if (signature_bits_current == 0 || query_signer == NULL || vector_metric == VECTOR_METRIC_L2)
	return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_EXACT, filter);		// transparent fallback still filters
return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_APPROX, filter);
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_HYBRID_HNSW()
	------------------------------------------
	search_hybrid_impl() with the HNSW vector-leg gatherer (per-segment graph)
	instead of exact; RRF math, lexical leg, and publish are unchanged.
	Transparently falls back to the exact fusion when HNSW is unconfigured or the
	metric is dot product (dot has no HNSW graph; see search_vector_hnsw()).
*/
long long ATIRE_segment_index::search_hybrid_hnsw(char *query_text, const float *query_vector, long long top_k, const ANT_filter *filter)
{
if (hnsw_M_current == 0 || vector_metric == VECTOR_METRIC_DOT)
	return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_EXACT, filter);		// transparent fallback still filters
return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_HNSW, filter);
}

long long ATIRE_segment_index::search_hybrid_hnsw(char *query_text, const float *query_vector, long long top_k)
{
if (hnsw_M_current == 0 || vector_metric == VECTOR_METRIC_DOT)
	return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_EXACT);		// transparent fallback
return search_hybrid_impl(query_text, query_vector, top_k, VECTOR_MODE_HNSW);
}

/*
	ATIRE_SEGMENT_INDEX::MAXSIM_LIVE()
	-----------------------------------
	MaxSim(query_vecs, docid's multi-vectors) over the live writer's multi-
	vector buffer: for each query vector, the best (highest) dot product
	against any of docid's rows, summed across query vectors.  query_vecs is
	assumed already L2-normalized by the caller (search_rerank()); the buffer
	rows were normalized at capture time (writer_multivector_append()).
	Returns 0.0 if docid has no captured rows.
*/
double ATIRE_segment_index::maxsim_live(long long docid, const float *query_vecs, long long num_query_vecs)
{
long long dim, off, d, i, j, m;
double total;

if (writer_multivector_counts == NULL || docid < 0 || docid >= writer_multivector_counts_capacity)
	return 0.0;

m = writer_multivector_counts[docid];
if (m <= 0 || num_query_vecs <= 0)
	return 0.0;

dim = rerank_dimension_current;
off = 0;
for (d = 0; d < docid; d++)
	off += writer_multivector_counts[d];

total = 0.0;
for (i = 0; i < num_query_vecs; i++)
	{
	double best = -1e30;

	for (j = 0; j < m; j++)
		{
		const float *v = writer_multivector_data + (off + j) * dim;
		double dot = 0.0;

		for (d = 0; d < dim; d++)
			dot += (double)query_vecs[i * dim + d] * (double)v[d];
		if (dot > best)
			best = dot;
		}
	total += best;
	}

return total;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_RERANK()
	--------------------------------------
	Two-stage retrieval: run the existing first-stage search (lexical / vector
	/ hybrid, whichever of query_text and query_vector are given) to get up to
	first_stage_n candidates into results[], then rescore each candidate by
	MaxSim over its multi-vectors (live writer buffer or disk segment
	sidecar, selected by matching the hit's generation) and reorder.
	Candidates with multi-vectors are sorted by MaxSim score descending (ties
	by generation, docid ascending); candidates without multi-vectors keep
	their stage-1 relative order and are appended after the reranked ones
	(graceful degradation).  Only the top_k survivors are published into
	results[]; the rest have their filenames freed (reset_results()'s
	convention) before results_count is set.

	If rerank is not configured, or no query multi-vector is supplied, this
	is just the plain first-stage search (still a sensible result).  If
	neither query_text nor query_vector is given (but a query multi-vector
	is, and rerank is configured), the first stage is instead the token-ANN
	candidate generator (search_multivector_impl()): it already does
	candidate-gen + exact MaxSim + publish, so this delegates straight to it.
*/
long long ATIRE_segment_index::search_rerank(char *query_text, const float *query_vector,
		const float *query_multivector, long long num_query_vecs, long long first_stage_n, long long top_k)
{
return search_rerank(query_text, query_vector, query_multivector, num_query_vecs, first_stage_n, top_k, NULL);
}

long long ATIRE_segment_index::search_rerank(char *query_text, const float *query_vector,
		const float *query_multivector, long long num_query_vecs, long long first_stage_n, long long top_k, const ANT_filter *filter)
{
long long dim = rerank_dimension_current, i, w, nc;
float *qn;
double *ms;
char *has_mv;
long long *order;
hit *tmp;

if (top_k < 1)
	return 0;
if (first_stage_n < top_k)
	first_stage_n = top_k;

if (!rerank_configured() || query_multivector == NULL || num_query_vecs < 1)
	{
	/* not usable -- plain first stage (still returns sensible hits) */
	if (query_text != NULL && query_vector != NULL)
		return search_hybrid(query_text, query_vector, top_k, filter);
	if (query_vector != NULL)
		return search_vector(query_vector, top_k, filter);
	if (query_text != NULL)
		return search(query_text, top_k, filter);
	return 0;
	}

/* V6: no lexical/dense first stage -> generate candidates via the token-ANN
   path (search_multivector), which does candidate-gen + exact MaxSim + publish.
   Was previously a return-0 guard. */
if (query_text == NULL && query_vector == NULL)
	return search_multivector_impl(query_multivector, num_query_vecs, top_k, filter);

qn = new float[num_query_vecs * dim];
memcpy(qn, query_multivector, (size_t)(num_query_vecs * dim) * sizeof(float));
for (i = 0; i < num_query_vecs; i++)
	ANT_vector_store::normalize(qn + i * dim, dim);

/* stage 1 -> results[] */
if (query_text != NULL && query_vector != NULL)
	search_hybrid(query_text, query_vector, first_stage_n, filter);
else if (query_vector != NULL)
	search_vector(query_vector, first_stage_n, filter);
else
	search(query_text, first_stage_n, filter);

nc = results_count;
if (nc == 0)
	{
	delete [] qn;
	return 0;
	}

ms = new double[nc];
has_mv = new char[nc];
for (i = 0; i < nc; i++)
	{
	long long gen = results[i].generation, did = results[i].docid;
	double score = 0.0;
	int found = 0;

	if (gen == writer_generation)
		{
		if (writer_multivector_counts != NULL && did >= 0 && did < writer_multivector_counts_capacity && writer_multivector_counts[did] > 0)
			{
			score = maxsim_live(did, qn, num_query_vecs);
			found = 1;
			}
		}
	else
		{
		for (w = 0; w < segment_count; w++)
			if (segments[w].generation == gen)
				{
				if (segments[w].multivectors != NULL && segments[w].multivectors->has(did))
					{
					score = segments[w].multivectors->maxsim(did, qn, num_query_vecs);
					found = 1;
					}
				break;
				}
		}
	ms[i] = score;
	has_mv[i] = (char)found;
	}

/*
	Build the publish order: multi-vector candidates first, sorted by MaxSim
	score descending (ties by generation, docid ascending) via a stable
	insertion sort over just that prefix, then the no-multi-vector
	candidates appended in their original stage-1 order.
*/
order = new long long[nc];
long long k = 0, a, b;

for (i = 0; i < nc; i++)
	if (has_mv[i])
		order[k++] = i;
for (a = 1; a < k; a++)
	{
	long long v = order[a];

	b = a - 1;
	while (b >= 0 && (ms[order[b]] < ms[v]
			|| (ms[order[b]] == ms[v] && (results[order[b]].generation > results[v].generation
			|| (results[order[b]].generation == results[v].generation && results[order[b]].docid > results[v].docid)))))
		{
		order[b + 1] = order[b];
		b--;
		}
	order[b + 1] = v;
	}
for (i = 0; i < nc; i++)
	if (!has_mv[i])
		order[k++] = i;

/*
	Reorder into a temp array, overwrite the score with the MaxSim score for
	reranked hits (stage-1 score is kept for the no-multi-vector tail), keep
	only the top_k, and free the dropped tail's filenames the same way
	reset_results() does (delete []) since they were allocated with new []
	(resolve_hit_filename() / append_result() callers).
*/
tmp = new hit[nc];
for (i = 0; i < nc; i++)
	{
	tmp[i] = results[order[i]];
	if (has_mv[order[i]])
		tmp[i].score = ms[order[i]];
	}

long long keep = (top_k < nc) ? top_k : nc;

for (i = keep; i < nc; i++)
	if (tmp[i].filename != NULL)
		delete [] tmp[i].filename;
for (i = 0; i < keep; i++)
	results[i] = tmp[i];
results_count = keep;

delete [] tmp;
delete [] order;
delete [] ms;
delete [] has_mv;
delete [] qn;
return results_count;
}

/*
	ATIRE_SEGMENT_INDEX::MULTIVECTOR_SCAN_SEGMENT_EXACT()
	--------------------------------------------------------
	Exhaustive exact-MaxSim scan of every doc in segment `which` with
	multi-vectors, applying tombstones/filter inline -- this IS exact (no
	under-fill).  Shared by the no-.tann branch of multivector_candidates()
	and, per #16, by the token-ANN branch's under-fill top-up.
*/
void ATIRE_segment_index::multivector_scan_segment_exact(long long which, const float *qn, long long num_query_vecs,
	long long top_k, ANT_vector_candidate *best, long long *best_count,
	ANT_multivector_store *mv, ANT_multivector_pq_store *pqs, long use_pq, const unsigned char *fbits)
{
long long docs = segments[which].engine->get_document_count();
for (long long did = 0; did < docs; did++)
	{
	if (mv != NULL ? !mv->has(did) : !pqs->has(did))
		continue;
	if (segments[which].tombstones != NULL && segments[which].tombstones->is_deleted(did))
		continue;
	if (fbits != NULL && !(fbits[did >> 3] & (1 << (did & 7))))
		continue;
	ANT_vector_candidate_insert(best, best_count, top_k, (use_pq ? pqs->maxsim(did, qn, num_query_vecs) : mv->maxsim(did, qn, num_query_vecs)), segments[which].generation, did);
	}
}

/*
	ATIRE_SEGMENT_INDEX::MULTIVECTOR_CANDIDATES()
	----------------------------------------------
	Per-disk-segment candidate gatherer for search_multivector().  Each segment
	with a built token-ANN graph is shortlisted via ANT_token_index::search_candidates(),
	which applies tombstones + the filter bitset at the doc level but only AFTER
	mapping token id -> docid (the token graph's nodes are tokens, not docids, so
	unlike the dense/HNSW path it cannot admit in-traversal); each surviving
	candidate is then exact-MaxSim-rescored.  When `filter` is non-NULL we widen
	the per-query-vector token pool (`eff_top_p`/`eff_pool`, below) to compensate
	for the doc-level admission being applied post-hoc; if the segment is STILL
	short of top_k after that widened shortlist (under-fill), we fall through to
	multivector_scan_segment_exact() -- an exhaustive scan that is a superset of
	the token-ANN matches, so it REPLACES (never supplements) the token-ANN
	inserts for that segment, guaranteeing no completeness gap under any filter
	selectivity.  Segments with no token index fall back to the same exact scan
	unconditionally.  `qn` is the already-normalized query multi-vector.
*/
long long ATIRE_segment_index::multivector_candidates(const float *qn, long long num_query_vecs, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter)
{
long long which, best_count = 0, pool_size = top_k * candidate_multiplier;

for (which = 0; which < segment_count; which++)
	{
	ANT_multivector_store *mv = segments[which].multivectors;
	ANT_multivector_pq_store *pqs = segments[which].multivector_pq;

	long use_pq = (multivector_pq_configured() && mvpq_posture_current == PQ_POSTURE_REPLACE
		&& pqs != NULL
		&& pqs->token_count() > 0
		&& pqs->document_count() == segments[which].engine->get_document_count());

	/*
		#24 NONE tier: mv is NULL (float pool dropped from RAM on load) and the
		segment's only per-doc existence/exact-score source is the PQ store
		(ADC maxsim).  That is only usable in REPLACE posture (use_pq); a
		NONE-tier segment configured RERANK-posture has no float to rerank
		against, so it is skipped -- same as any segment with neither source.
	*/
	if (mv == NULL && !use_pq)
		continue;

	unsigned char *fbits = evaluate_filter_for_segment(which, filter);   /* NULL when filter==NULL */

	if (segments[which].token_index != NULL && !segments[which].token_index->empty())
		{
		/* token-ANN shortlist; filtered queries widen the pool to compensate for
		   doc-level admission happening post-hoc.  If a selective filter still leaves
		   the segment short of top_k (under-fill), fall through to the exact scan
		   below -- it finds every matching doc (a superset), so no completeness gap. */
		long long eff_top_p = (fbits != NULL) ? token_top_p * candidate_multiplier : token_top_p;
		long long eff_pool  = (fbits != NULL) ? pool_size * candidate_multiplier : pool_size;
		long long *cand = new long long[eff_pool > 0 ? eff_pool : 1];
		long long n = segments[which].token_index->search_candidates(qn, num_query_vecs, eff_top_p, eff_pool, segments[which].tombstones, fbits, cand);

		if (fbits != NULL && n < top_k)
			multivector_scan_segment_exact(which, qn, num_query_vecs, top_k, best, &best_count, mv, pqs, use_pq, fbits);
		else
			for (long long p = 0; p < n; p++)
				{
				long long did = cand[p];
				if (mv != NULL ? !mv->has(did) : !pqs->has(did))
					continue;   /* tombstone+filter already applied by search_candidates */
				ANT_vector_candidate_insert(best, &best_count, top_k, (use_pq ? pqs->maxsim(did, qn, num_query_vecs) : mv->maxsim(did, qn, num_query_vecs)), segments[which].generation, did);
				}
		delete [] cand;
		}
	else
		multivector_scan_segment_exact(which, qn, num_query_vecs, top_k, best, &best_count, mv, pqs, use_pq, fbits);

	delete [] fbits;
	}

/* live-buffer merge: un-flushed docs with multi-vectors, scored by exact MaxSim */
unsigned char *lbits = evaluate_filter_for_live(filter);
for (long long did = 0; did < writer_documents; did++)
	{
	if (!(writer_multivector_counts != NULL && did < writer_multivector_counts_capacity && writer_multivector_counts[did] > 0))
		continue;
	if (writer_tombstones != NULL && writer_tombstones->is_deleted(did))
		continue;
	if (lbits != NULL && !(lbits[did >> 3] & (1 << (did & 7))))
		continue;
	ANT_vector_candidate_insert(best, &best_count, top_k, maxsim_live(did, qn, num_query_vecs), writer_generation, did);
	}
delete [] lbits;

return best_count;
}

/*
	ATIRE_SEGMENT_INDEX::SEARCH_MULTIVECTOR_IMPL()
	------------------------------------------------
	First-class late-interaction search: candidate-gen (token-ANN when built,
	brute-force MaxSim scan otherwise) -> exact MaxSim rescore -> publish.
	Mirrors search_vector_impl()'s sort/resolve/publish tail exactly.  Filtering
	is exact everywhere: the brute-force path scans every doc directly, and the
	token-ANN path over-gathers and, per #16, falls through to an exact scan on
	any per-segment under-fill (see multivector_candidates(), above) -- so no
	segment with matching docs can be silently short-changed regardless of
	filter selectivity.
*/
long long ATIRE_segment_index::search_multivector_impl(const float *query_multivector, long long num_query_vecs, long long top_k, const ANT_filter *filter)
{
char filename_buffer[4096];
long long which, count, i, dim = rerank_dimension_current;

reset_results();

if (!rerank_configured() || query_multivector == NULL || num_query_vecs < 1 || top_k < 1)
	return 0;

float *qn = new float[num_query_vecs * dim];
memcpy(qn, query_multivector, (size_t)(num_query_vecs * dim) * sizeof(float));
for (i = 0; i < num_query_vecs; i++)
	ANT_vector_store::normalize(qn + i * dim, dim);

ANT_vector_candidate *best = new ANT_vector_candidate[top_k];
count = multivector_candidates(qn, num_query_vecs, top_k, best, filter);
qsort(best, (size_t)count, sizeof(*best), vector_candidate_compare);

for (which = 0; which < count; which++)
	{
	char *filename = resolve_hit_filename(best[which].generation, best[which].docid, filename_buffer, sizeof(filename_buffer));

	hit *slot = append_result();

	slot->generation = best[which].generation;
	slot->docid = best[which].docid;
	populate_hit_payload(slot);
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
delete [] qn;
return results_count;
}

long long ATIRE_segment_index::search_multivector(const float *query_multivector, long long num_query_vecs, long long top_k)
{
return search_multivector_impl(query_multivector, num_query_vecs, top_k, NULL);
}

long long ATIRE_segment_index::search_multivector(const float *query_multivector, long long num_query_vecs, long long top_k, const ANT_filter *filter)
{
return search_multivector_impl(query_multivector, num_query_vecs, top_k, filter);
}

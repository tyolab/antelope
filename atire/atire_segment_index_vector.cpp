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
long long m, posture, rerank_quant;
const char *tag = "ANTPQCF1";

memcpy(&want, tag, 8);
snprintf(filename, sizeof(filename), "%s/pq.config", directory);
if ((fp = fopen(filename, "rb")) == NULL)
	return 0;
if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != want
	|| fread(&version, sizeof(version), 1, fp) != 1 || version != 1u
	|| fread(&m, sizeof(m), 1, fp) != 1 || m < 1 || m > 65536
	|| fread(&posture, sizeof(posture), 1, fp) != 1 || (posture != 0 && posture != 1)
	|| fread(&rerank_quant, sizeof(rerank_quant), 1, fp) != 1 || (rerank_quant != 0 && rerank_quant != 1))
	{ fclose(fp); return 0; }
fclose(fp);
pq_m_current = m;
pq_posture_current = (long)posture;
pq_rerank_quant_current = (long)rerank_quant;
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
unsigned int version = 1u;
long long m = pq_m_current;
long long posture = pq_posture_current;
long long rerank_quant = pq_rerank_quant_current;
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
	|| fwrite(&rerank_quant, sizeof(rerank_quant), 1, fp) != 1)
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
	ATIRE_SEGMENT_INDEX::LOAD_MULTIVECTOR_PQ_CONFIG() / SAVE_...()
	-------------------------------------------------------------
	Persist token-PQ config in <dir>/multivector_pq.config (magic "ANTMVPQC",
	version 1, three i64: m, posture, rerank_quant).  Defensive parse.
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
long long vals[3];
long ok = fread(tag, 1, 8, in) == 8 && memcmp(tag, "ANTMVPQC", 8) == 0
	&& fread(&version, 4, 1, in) == 1 && version == 1
	&& fread(vals, 8, 3, in) == 3;
fclose(in);
if (!ok)
	return 1;

mvpq_m_current = vals[0];
mvpq_posture_current = (long)vals[1];
mvpq_rerank_quant_current = (long)vals[2];
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

unsigned int version = 1;
long long vals[3] = { mvpq_m_current, mvpq_posture_current, mvpq_rerank_quant_current };
long ok = fwrite("ANTMVPQC", 1, 8, out) == 8 && fwrite(&version, 4, 1, out) == 1 && fwrite(vals, 8, 3, out) == 3;
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
char vec_name[4096], hnsw_name[4096];

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

	segment_filename(vec_name, sizeof(vec_name), generation, "vec");
	ANT_vector_store *vectors = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
	if (vectors->document_count() == docs && docs > 0)
		{
		ANT_hnsw graph;
		if (graph.build(vectors, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0)
			graph.save(hnsw_name);
		}
	delete vectors;
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
	if (segments[which].multivectors == NULL || segments[which].multivectors->document_count() == 0)
		continue;
	if (segments[which].token_index != NULL && !segments[which].token_index->empty())
		continue;	/* already built */

	segment_filename(tann_name, sizeof(tann_name), segments[which].generation, "tann");
	ANT_token_index *idx = ANT_token_index::build(segments[which].multivectors, token_index_M, token_index_ef_construction, ANT_vector_store::METRIC_DOT);
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
char vec_name[4096], pq_name[4096];

if (!pq_configured() || vector_dimension_current == 0)
	return 1;

for (which = 0; which < segment_count; which++)
	{
	long long generation = segments[which].generation;
	long long docs = segments[which].engine->get_document_count();

	if (segments[which].pq_vectors != NULL && segments[which].pq_vectors->document_count() == docs && docs > 0)
		continue;				/* already has a valid .pq: idempotent skip */

	segment_filename(vec_name, sizeof(vec_name), generation, "vec");
	segment_filename(pq_name, sizeof(pq_name), generation, "pq");

	ANT_vector_store *src = ANT_vector_store::load(vec_name, vector_dimension_current, docs);
	if (src->document_count() == docs && docs > 0 && !src->is_quantized())
		{
		ANT_pq_store_writer w;
		long failed = w.create(pq_name, vector_dimension_current, pq_m_current, vector_metric) != 0;
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
			segments[which].pq_vectors = ANT_pq_store::load(pq_name, vector_dimension_current, docs, vector_metric);
			}
		}
	delete src;
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
	ANT_multivector_store *mv = segments[which].multivectors;
	if (mv == NULL || mv->tokens_quantized())
		continue;
	long long docs = segments[which].engine->get_document_count();
	if (segments[which].multivector_pq != NULL
		&& segments[which].multivector_pq->document_count() == docs
		&& segments[which].multivector_pq->token_count() > 0)
		continue;

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
	if (segments[which].vectors == NULL)
		continue;
	if (segments[which].hnsw_graph != NULL && !segments[which].hnsw_graph->empty()
		&& segments[which].hnsw_graph->node_count() == segments[which].engine->get_document_count())
		{
		unsigned char *fbits = evaluate_filter_for_segment(which, filter);
		long long c = segments[which].hnsw_graph->search(query, vector_metric, ef, ef,
			segments[which].vectors, segments[which].tombstones, cand_docids, cand_scores, fbits);
		for (long long p = 0; p < c; p++)
			{
			double sc = segments[which].exact_vectors != NULL
				? segments[which].exact_vectors->score(cand_docids[p], query, vector_metric)
				: cand_scores[p];
			ANT_vector_candidate_insert(best, &best_count, top_k, sc, segments[which].generation, cand_docids[p]);
			}
		delete [] fbits;
		}
	else
		{
		ANT_vector_store *src = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
		unsigned char *fbits = evaluate_filter_for_segment(which, filter);
		src->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
		delete [] fbits;
		}
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
	top_k*candidate_multiplier candidates, then RESCORE each with the exact
	resident float store (segments[].vectors is float in PQ mode) to restore
	precision, keeping only the exact top_k.  Segments without a valid .pq fall
	back to an exact float scan; the live buffer is always exact.  Mirrors
	vector_candidates_approx (signature shortlist -> exact rescore).

	Note: pq_rerank_quant_current (FLOAT vs INT8) does not change behavior in
	Phase 1 -- the resident float store is the exact tier for both.  A distinct
	int8 rescore tier would need a separate persistent sidecar (excluded from PQ
	mode) and would be strictly less precise than the resident float anyway; it
	becomes meaningful only once the resident float is dropped (a Phase-2 memory
	optimization).
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
	if (segments[which].vectors == NULL)
		continue;
	ANT_vector_store *src = segments[which].exact_vectors != NULL ? segments[which].exact_vectors : segments[which].vectors;
	unsigned char *fbits = evaluate_filter_for_segment(which, filter);
	if (segments[which].pq_vectors != NULL
		&& segments[which].pq_vectors->document_count() == segments[which].engine->get_document_count()
		&& segments[which].pq_vectors->document_count() > 0)
		{
		long long count = 0;
		segments[which].pq_vectors->scan_adc(query, vector_metric, segments[which].tombstones, segments[which].generation, shortlist, &count, pool_size, fbits);
		for (p = 0; p < count; p++)
			{
			long long docid = shortlist[p].docid;
			if (!src->has(docid))
				continue;			/* shortlist already tombstone/filter-clean; presence belt-and-suspenders */
			ANT_vector_candidate_insert(best, &best_count, top_k, src->score(docid, query, vector_metric), segments[which].generation, docid);
			}
		}
	else
		src->scan(query, vector_metric, segments[which].tombstones, segments[which].generation, best, &best_count, top_k, fbits);
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
	ATIRE_SEGMENT_INDEX::MULTIVECTOR_CANDIDATES()
	----------------------------------------------
	Per-disk-segment candidate gatherer for search_multivector().  Each segment
	with a built token-ANN graph is shortlisted via ANT_token_index::search_candidates(),
	which applies tombstones + the filter bitset at the doc level but only AFTER
	mapping token id -> docid (the token graph's nodes are tokens, not docids, so
	unlike the dense/HNSW path it cannot admit in-traversal); each surviving
	candidate is then exact-MaxSim-rescored.  When `filter` is non-NULL we widen
	the per-query-vector token pool (`eff_top_p`/`eff_pool`, below) to compensate
	for the doc-level admission being applied post-hoc -- this is BEST-EFFORT,
	not a no-under-fill guarantee: under a highly selective filter, docs whose
	tokens aren't among the nearest widened pool can still be silently missed.
	Segments with no token index (V6 build_token_index is Task 9, so this is
	every segment for now) fall back to an exhaustive scan of every doc with
	multi-vectors, applying tombstones/filter inline -- this brute-force path
	IS exact (no under-fill) and is what this task's ranking-equality test
	locks against.  `qn` is the already-normalized query multi-vector.
*/
long long ATIRE_segment_index::multivector_candidates(const float *qn, long long num_query_vecs, long long top_k, ANT_vector_candidate *best, const ANT_filter *filter)
{
long long which, best_count = 0, pool_size = top_k * candidate_multiplier;

for (which = 0; which < segment_count; which++)
	{
	ANT_multivector_store *mv = segments[which].multivectors;
	if (mv == NULL)
		continue;

	unsigned char *fbits = evaluate_filter_for_segment(which, filter);   /* NULL when filter==NULL */

	if (segments[which].token_index != NULL && !segments[which].token_index->empty())
		{
		/* filtered queries widen the token-ANN pool to compensate for doc-level
		   filter admission happening post-hoc (see search_candidates' comment);
		   this is best-effort, NOT a no-under-fill guarantee -- under a highly
		   selective filter, matching docs whose tokens aren't among the widened
		   nearest set can still be missed from this token-ANN path. */
		long long eff_top_p = (fbits != NULL) ? token_top_p * candidate_multiplier : token_top_p;
		long long eff_pool  = (fbits != NULL) ? pool_size * candidate_multiplier : pool_size;
		long long *cand = new long long[eff_pool > 0 ? eff_pool : 1];
		long long n = segments[which].token_index->search_candidates(qn, num_query_vecs, eff_top_p, eff_pool, segments[which].tombstones, fbits, cand);

		for (long long p = 0; p < n; p++)
			{
			long long did = cand[p];
			if (!mv->has(did))
				continue;   /* tombstone+filter already applied by search_candidates */
			ANT_vector_candidate_insert(best, &best_count, top_k, mv->maxsim(did, qn, num_query_vecs), segments[which].generation, did);
			}
		delete [] cand;
		}
	else
		{
		long long docs = segments[which].engine->get_document_count();

		for (long long did = 0; did < docs; did++)
			{
			if (!mv->has(did))
				continue;
			if (segments[which].tombstones != NULL && segments[which].tombstones->is_deleted(did))
				continue;
			if (fbits != NULL && !(fbits[did >> 3] & (1 << (did & 7))))
				continue;
			ANT_vector_candidate_insert(best, &best_count, top_k, mv->maxsim(did, qn, num_query_vecs), segments[which].generation, did);
			}
		}

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
	is exact on the brute-force path; on the token-ANN path it is best-effort
	over-gather (see multivector_candidates(), above) and can under-fill for
	highly selective filters -- it is not a no-under-fill guarantee.
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

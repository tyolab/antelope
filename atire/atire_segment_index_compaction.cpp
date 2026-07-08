/*
	ATIRE_SEGMENT_INDEX_COMPACTION.CPP
	----------------------------------
	Compaction and tiered maintenance.  Part of ATIRE_segment_index, whose
	implementation is split across atire_segment_index*.cpp by feature (see
	docs/superpowers/specs/2026-07-06-segment-index-file-split-design.md).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

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
#include "../source/signature.h"
#include "../source/signature_store.h"
#include "../source/hnsw.h"
#include "../source/wal.h"
#include "../source/multivector_store.h"

/*
	ATIRE_SEGMENT_INDEX::COMPACT()
	------------------------------
	Merge the given disk segments into one new segment and swap it in.
	Ordering is crash-safe at every boundary (see the Phase 2 design spec,
	docs/superpowers/specs/2026-07-06-compacting-merge-design.md section 3):
	the output is written fully before anything references it; the
	"compacting" marker makes a mid-swap crash trigger a full keymap
	rebuild at the next open(); input files are deleted last (leftovers are
	swept as orphans).  Only disk segments may be named here -- the live
	writer is never compacted (callers pass manifest generations, all of
	which are, by construction, flushed/disk segments; see maintain()).

	Step-5 (atomic manifest swap) failure semantics: if the merge, marker
	and keymap remap (steps 1-4) all succeeded but manifest->save() then
	fails, the in-memory keymap ALREADY points every remapped key at the
	new output segment -- that cannot be undone cheaply (and undoing it
	would mean re-deriving which keys used to point where, information the
	remap loop does not retain). Rather than leave a torn state where the
	keymap and segments[] disagree, this method proceeds to complete the
	in-memory swap anyway: the inputs are dropped from segments[] (so
	search()/get_document_count() agree with the keymap) but their files
	are NOT deleted and the marker is NOT removed. The result: the
	in-process view is fully consistent (output live, inputs gone, keymap
	correct); the on-disk manifest still lists the old inputs (and not the
	output); and the leftover "compacting" marker forces open()'s
	marker-triggered keymap rebuild in open(), which
	reconstructs everything from whichever segments the reloaded manifest
	actually names. This is returned as failure (1) so the caller knows
	the on-disk state was not durably swapped, even though the running
	process now behaves as if it were.
*/
long ATIRE_segment_index::compact(long long *input_generations, long long input_count)
{
char output_name[4096], marker_name[4096], filename_buffer[4096];
long long which, input, docid;
const char *vext = (quantization_current == QUANTIZE_REPLACE) ? "qvec" : "vec";

if (input_count < 1)
	return 1;

/*
	Resolve the inputs to open segments (all must exist and be distinct).  A
	duplicate generation in input_generations[] would feed the same engine
	into merge() twice -- merge()'s N-way walk has no notion of "the same
	document seen through two inputs", so it would emit that segment's live
	documents a second time (duplicated postings, doubled document count),
	silently corrupting the output.  Reject before doing any work.
*/
segment **inputs = new segment *[input_count];
for (input = 0; input < input_count; input++)
	{
	for (which = 0; which < input; which++)
		if (input_generations[which] == input_generations[input])
			{
			delete [] inputs;
			return 1;
			}
	inputs[input] = NULL;
	for (which = 0; which < segment_count; which++)
		if (segments[which].generation == input_generations[input])
			inputs[input] = &segments[which];
	if (inputs[input] == NULL)
		{
		delete [] inputs;
		return 1;
		}
	}

/*
	Step 1: burn the output generation (manifest saved by contract --
	crash here leaves nothing, the generation number is simply skipped)
*/
long long output_generation = manifest->take_generation();
if (manifest->save() != 0)
	{
	delete [] inputs;
	return 1;
	}
segment_filename(output_name, sizeof(output_name), output_generation, "aspt");

/*
	Step 2: merge.  A crash or failure here leaves an unmanifested (or
	nonexistent) output file and the inputs untouched -- orphan-swept at
	the next open(), no data lost.
*/
ANT_search_engine **engines = new ANT_search_engine *[input_count];
ANT_index_tombstones **stones = new ANT_index_tombstones *[input_count];
for (input = 0; input < input_count; input++)
	{
	engines[input] = inputs[input]->engine->get_search_engine();
	stones[input] = inputs[input]->tombstones;
	}
ANT_index_merger *merger = new ANT_index_merger();
long merge_result = merger->merge(engines, stones, input_count, output_name);
delete merger;
delete [] engines;
delete [] stones;
if (merge_result != 0)
	{
	delete [] inputs;
	return 1;
	}

/*
	Step 2b: rewrite the vector sidecar for the merged output.  The
	renumbering below is byte-identical to the merger's: both are built from
	the same tombstones in the same input order (ANT_docid_renumberer is
	deterministic).  Inputs without vectors contribute absent rows.  A .vec
	failure aborts the compaction pre-marker, leaving the index untouched
	(the output .aspt is removed like any pre-step-3 failure).
*/
if (vector_dimension_current != 0)
	{
	long any_vectors = false;
	for (input = 0; input < input_count; input++)
		if (inputs[input]->vectors != NULL && inputs[input]->vectors->document_count() > 0)
			any_vectors = true;
	if (any_vectors)
		{
		ANT_index_tombstones **stone_list = new ANT_index_tombstones *[input_count];
		long long *doc_counts = new long long[input_count];
		for (input = 0; input < input_count; input++)
			{
			stone_list[input] = inputs[input]->tombstones;
			doc_counts[input] = inputs[input]->engine->get_document_count();
			}
		ANT_docid_renumberer *vec_renumberer = new ANT_docid_renumberer(stone_list, doc_counts, input_count);
		char vec_name[4096];
		segment_filename(vec_name, sizeof(vec_name), output_generation, vext);
		ANT_vector_store_writer vec_writer;
		long vec_failed = vec_writer.create(vec_name, vector_dimension_current) != 0;
		if (!vec_failed && quantization_current == QUANTIZE_REPLACE)
			vec_writer.set_quantization(ANT_vector_store_writer::QUANT_REPLACE);
		float *merge_buf = new float[vector_dimension_current];
		for (input = 0; !vec_failed && input < input_count; input++)
			for (docid = 0; !vec_failed && docid < doc_counts[input]; docid++)
				{
				if (vec_renumberer->renumber(input, docid) < 0)
					continue;		/* tombstoned: dropped, exactly like its postings */
				float *row;
				ANT_vector_store *vsrc = inputs[input]->exact_vectors != NULL
										 ? inputs[input]->exact_vectors : inputs[input]->vectors;
				if (vsrc != NULL && vsrc->has(docid))
					{ vsrc->reconstruct(docid, merge_buf); row = merge_buf; }
				else
					row = NULL;
				vec_failed = vec_writer.append(row) != 0;
				}
		delete [] merge_buf;
		if (!vec_failed)
			vec_failed = vec_writer.finish() != 0;
		if (!vec_failed && quantization_current == QUANTIZE_EXACT)
			{
			char qvec_name[4096];
			segment_filename(qvec_name, sizeof(qvec_name), output_generation, "qvec");
			vec_writer.finish_qvec(qvec_name);
			}
		delete vec_renumberer;
		delete [] stone_list;
		delete [] doc_counts;
		if (vec_failed)
			{
			remove(output_name);
			delete [] inputs;
			return 1;
			}
		}
	}

/*
	Step 2c: rewrite the multi-vector (.mvec) sidecar for the merged output,
	when rerank is configured.  Same renumbering discipline as the .vec block
	above (built fresh here rather than reusing the .vec block's locals,
	which are scoped to that block): iterate inputs in order, skip tombstoned
	docids, and append a (possibly-empty, variable-length) row per surviving
	document so output docids stay aligned with the merged .aspt.  Best-effort:
	a failure here leaves the output without a .mvec (never aborts a
	successful merge) -- rerank then falls back to stage-1 order for this
	segment, same as any other missing sidecar.
*/
if (rerank_configured())
	{
	long any_multivectors = 0;
	for (input = 0; input < input_count; input++)
		if (inputs[input]->multivectors != NULL && inputs[input]->multivectors->document_count() > 0)
			any_multivectors = 1;
	if (any_multivectors)
		{
		ANT_index_tombstones **mv_stone_list = new ANT_index_tombstones *[input_count];
		long long *mv_doc_counts = new long long[input_count];
		long long mv_maxm = 1;
		for (input = 0; input < input_count; input++)
			{
			mv_stone_list[input] = inputs[input]->tombstones;
			mv_doc_counts[input] = inputs[input]->engine->get_document_count();
			if (inputs[input]->multivectors != NULL)
				{
				long long mc = inputs[input]->multivectors->max_vector_count();
				if (mc > mv_maxm) mv_maxm = mc;
				}
			}
		ANT_docid_renumberer *mv_renumberer = new ANT_docid_renumberer(mv_stone_list, mv_doc_counts, input_count);
		char mvec_name[4096];
		segment_filename(mvec_name, sizeof(mvec_name), output_generation, "mvec");
		ANT_multivector_store_writer mvw;
		long mv_failed = mvw.create(mvec_name, rerank_dimension_current) != 0;
		if (!mv_failed)
			mvw.set_quantization(rerank_quant_current == RERANK_QUANT_INT8 ? ANT_multivector_store_writer::QUANT_INT8 : ANT_multivector_store_writer::QUANT_OFF);
		float *mvbuf = new float[mv_maxm * rerank_dimension_current];
		for (input = 0; !mv_failed && input < input_count; input++)
			for (docid = 0; !mv_failed && docid < mv_doc_counts[input]; docid++)
				{
				if (mv_renumberer->renumber(input, docid) < 0)
					continue;		/* tombstoned: dropped, exactly like its postings */
				long long m = 0;
				if (inputs[input]->multivectors != NULL)
					m = inputs[input]->multivectors->copy_vectors(docid, mvbuf);
				mv_failed = mvw.append(m > 0 ? mvbuf : NULL, m) != 0;
				}
		delete [] mvbuf;
		if (!mv_failed) mvw.finish(); else mvw.abandon();
		delete mv_renumberer;
		delete [] mv_stone_list;
		delete [] mv_doc_counts;
		/* best-effort, non-fatal -- mirrors the .vsig/.hnsw rebuild blocks below */
		}
	}

/*
	Step 2d: rewrite the attribute (.attr) + payload (.pay) sidecars for the
	merged output.  Same renumbering discipline as the .mvec block above
	(built fresh here rather than reusing that block's block-scoped locals):
	iterate inputs in order, skip tombstoned docids, and emit one attribute
	record + one payload blob per surviving document so output docids stay
	aligned with the merged .aspt.  The elegant part: each input has its OWN
	per-field string dictionary (ids differ across inputs), so we do NOT remap
	ids -- we read each surviving doc's string VALUES from the input store and
	re-write them into the fresh writer, which re-interns them into a single
	union dictionary with fresh contiguous ids.  Best-effort: a failure here
	leaves the output filter-less / payload-less (never aborts a successful
	merge), mirroring the .mvec block.
*/
if (attributes_configured())
	{
	ANT_index_tombstones **attr_stone_list = new ANT_index_tombstones *[input_count];
	long long *attr_doc_counts = new long long[input_count];
	for (input = 0; input < input_count; input++)
		{
		attr_stone_list[input] = inputs[input]->tombstones;
		attr_doc_counts[input] = inputs[input]->engine->get_document_count();
		}
	ANT_docid_renumberer *attr_renumberer = new ANT_docid_renumberer(attr_stone_list, attr_doc_counts, input_count);
	char attr_name[4096], pay_name[4096];
	segment_filename(attr_name, sizeof(attr_name), output_generation, "attr");
	segment_filename(pay_name, sizeof(pay_name), output_generation, "pay");
	ANT_attribute_store_writer attw;
	ANT_payload_store_writer payw;
	long attr_failed = attw.create(attr_name, &attribute_schema_current) != 0;
	long pay_failed = payw.create(pay_name) != 0;
	long long ncols = attribute_schema_current.count();
	char *strbuf = new char[65536];		/* attribute string values are short categorical metadata */
	for (input = 0; !attr_failed && !pay_failed && input < input_count; input++)
		for (docid = 0; !attr_failed && !pay_failed && docid < attr_doc_counts[input]; docid++)
			{
			if (attr_renumberer->renumber(input, docid) < 0)
				continue;		/* tombstoned: dropped, aligned with the merged .aspt */
			/* --- attributes --- */
			attw.begin_document();
			ANT_attribute_store *ain = inputs[input]->attributes;
			if (ain != NULL)
				for (long long field = 0; field < ncols; field++)
					{
					if (!ain->has_field(field, docid)) continue;
					int type = attribute_schema_current.type(field);
					int multi = attribute_schema_current.is_multi(field);
					if (type == ANT_attribute_schema::TYPE_INT64)
						{
						long long vc = ain->value_count(field, docid), k, v;
						if (multi) { for (k = 0; k < vc; k++) { if (ain->get_int_at(field, docid, k, &v)) attw.add_int(field, v); } }
						else { if (ain->get_int_at(field, docid, 0, &v)) attw.set_int(field, v); }
						}
					else if (type == ANT_attribute_schema::TYPE_STRING)
						{
						long long vc = ain->value_count(field, docid), k;
						if (multi) { for (k = 0; k < vc; k++) { if (ain->get_string_at(field, docid, k, strbuf, 65536)) attw.add_string(field, strbuf); } }
						else { if (ain->get_string_at(field, docid, 0, strbuf, 65536)) attw.set_string(field, strbuf); }
						}
					else /* BOOL */
						{ int b; if (ain->get_bool(field, docid, &b)) attw.set_bool(field, b); }
					}
			attw.end_document();
			/* --- payload --- */
			const unsigned char *pp = NULL; long long plen = 0;
			if (inputs[input]->payload != NULL)
				inputs[input]->payload->get(docid, &pp, &plen);
			payw.append(pp, plen);
			}
	delete [] strbuf;
	if (!attr_failed && attw.finish() != 0) attr_failed = 1;
	if (attr_failed) attw.abandon();
	if (!pay_failed && payw.finish() != 0) pay_failed = 1;
	if (pay_failed) payw.abandon();
	delete attr_renumberer;
	delete [] attr_stone_list;
	delete [] attr_doc_counts;
	/* best-effort, non-fatal: a failed .attr/.pay leaves the merged segment filter-less/payload-less, never aborts the merge (mirrors the .mvec block) */
	}

/*
	Step 3: marker -- from here until removal, a crash makes the next
	open() rebuild the keymap from the segments rather than trust the log
*/
snprintf(marker_name, sizeof(marker_name), "%s/compacting", directory);
FILE *marker = fopen(marker_name, "wb");
if (marker == NULL)
	{
	remove(output_name);
	delete [] inputs;
	return 1;
	}
fclose(marker);

/*
	Step 4: open the output and repoint the keymap at it.  Every document
	in the output is, by construction (the merger only emits live docs),
	the live copy of its key -- scan docid-ascending and unconditionally
	overwrite each key's keymap entry.
*/
if (append_segment(output_generation) != 0)
	{
	remove(output_name);
	remove(marker_name);
	delete [] inputs;
	return 1;
	}
segment *output_segment = &segments[segment_count - 1];
for (docid = 0; docid < output_segment->engine->get_document_count(); docid++)
	{
	char *filename = output_segment->engine->get_document_filename(filename_buffer, docid);
	if (filename != NULL && filename[0] != '\0')
		keymap->add(filename, output_generation, docid);
	}

/*
	V2: rebuild the merged segment's signature sidecar by signing the merged
	DENSE vectors just written (re-load the fresh output .vec) so signatures
	stay aligned to the compacted docids.  Best-effort: a failure leaves the
	output signature-less (exact-scanned), never aborts a successful merge.
	This must run BEFORE Step 6's shuffle, which invalidates output_segment.
*/
if (signature_bits_current != 0 && query_signer != NULL)
	{
	char out_vec[4096], out_vsig[4096];
	segment_filename(out_vec, sizeof(out_vec), output_generation, vext);
	segment_filename(out_vsig, sizeof(out_vsig), output_generation, "vsig");
	long long out_docs = output_segment->engine->get_document_count();
	ANT_vector_store *out_vectors = ANT_vector_store::load(out_vec, vector_dimension_current, out_docs);
	if (out_vectors->document_count() == out_docs && out_docs > 0)
		{
		ANT_signature_store_writer sig_writer;
		unsigned char *sig = new unsigned char[query_signer->signature_bytes()];
		float *recon_buf = new float[vector_dimension_current];
		long failed = sig_writer.create(out_vsig, signature_bits_current) != 0;
		for (long long d = 0; !failed && d < out_docs; d++)
			{
			if (out_vectors->has(d)) { out_vectors->reconstruct(d, recon_buf); query_signer->sign(recon_buf, sig); failed = sig_writer.append(sig) != 0; }
			else failed = sig_writer.append(NULL) != 0;
			}
		if (!failed) sig_writer.finish(); else sig_writer.abandon();
		delete [] recon_buf;
		delete [] sig;
		}
	delete out_vectors;
	/* refresh the in-memory cache so THIS session's search_vector_approx engages the prefilter */
	delete output_segment->signatures;
	output_segment->signatures = ANT_signature_store::load(out_vsig, signature_bits_current, output_segment->engine->get_document_count());
	}

/*
	V3: rebuild the merged segment's HNSW graph over the merged DENSE
	vectors (reload the fresh output .vec).  Best-effort: failure leaves the
	output graph-less (exact-scanned), never aborts a successful merge.
*/
if (hnsw_M_current != 0)
	{
	char out_vec[4096], out_hnsw[4096];
	segment_filename(out_vec, sizeof(out_vec), output_generation, vext);
	segment_filename(out_hnsw, sizeof(out_hnsw), output_generation, "hnsw");
	long long out_docs = output_segment->engine->get_document_count();
	ANT_vector_store *out_vectors = ANT_vector_store::load(out_vec, vector_dimension_current, out_docs);
	if (out_vectors->document_count() == out_docs && out_docs > 0)
		{
		ANT_hnsw graph;
		if (graph.build(out_vectors, hnsw_M_current, hnsw_ef_construction_current, vector_metric) == 0)
			graph.save(out_hnsw);
		}
	delete out_vectors;
	delete output_segment->hnsw_graph;
	output_segment->hnsw_graph = ANT_hnsw::load(out_hnsw, hnsw_M_current, hnsw_ef_construction_current, output_segment->engine->get_document_count());
	}

/*
	V5: refresh the merged segment's in-memory multi-vector cache from the
	.mvec sidecar just written (or the absence of one), so THIS session's
	search_rerank sees the compacted docids.  Must run before Step 6's
	shuffle, which invalidates output_segment.
*/
if (rerank_configured())
	{
	char mvec_name[4096];
	segment_filename(mvec_name, sizeof(mvec_name), output_generation, "mvec");
	delete output_segment->multivectors;
	output_segment->multivectors = ANT_multivector_store::load(mvec_name, rerank_dimension_current, output_segment->engine->get_document_count());
	}

/*
	Refresh the merged segment's in-memory attribute + payload stores from the
	.attr / .pay sidecars just written, so THIS session's filtered searches and
	hit payloads see the compacted docids.  Must run before Step 6's shuffle,
	which invalidates output_segment.
*/
if (attributes_configured())
	{
	char attr_name[4096], pay_name[4096];
	segment_filename(attr_name, sizeof(attr_name), output_generation, "attr");
	segment_filename(pay_name, sizeof(pay_name), output_generation, "pay");
	delete output_segment->attributes;
	output_segment->attributes = ANT_attribute_store::load(attr_name, &attribute_schema_current, output_segment->engine->get_document_count());
	delete output_segment->payload;
	output_segment->payload = ANT_payload_store::load(pay_name, output_segment->engine->get_document_count());
	}

/*
	Step 5: atomic manifest swap.  See the banner above for what happens
	if save() fails here -- the keymap is already remapped, so we proceed
	to step 6's in-memory removal regardless, but skip the file deletions
	and marker removal (the marker forces a keymap rebuild -- reconciling
	everything -- at the next open()).
*/
for (input = 0; input < input_count; input++)
	manifest->remove_segment(input_generations[input]);
manifest->add_segment(output_generation);
long manifest_swapped = manifest->save() == 0;

/*
	Step 6: drop the inputs from segments[] (in-memory swap complete
	either way).  inputs[] holds pointers into segments[], which THIS
	shuffle invalidates as soon as the first removal happens -- so each
	iteration re-finds its victim by generation rather than trusting the
	now-stale inputs[input] pointer.
*/
for (input = 0; input < input_count; input++)
	for (which = 0; which < segment_count; which++)
		if (segments[which].generation == input_generations[input])
			{
			delete segments[which].engine;
			delete segments[which].tombstones;
			delete segments[which].vectors;
			delete segments[which].exact_vectors;
			delete segments[which].signatures;
			delete segments[which].hnsw_graph;
			delete segments[which].multivectors;
			delete segments[which].token_index;
			delete segments[which].attributes;
			delete segments[which].payload;
			for (long long shuffle = which; shuffle < segment_count - 1; shuffle++)
				segments[shuffle] = segments[shuffle + 1];
			segment_count--;
			break;
			}

delete [] inputs;

if (!manifest_swapped)
	return 1;			// degraded but consistent -- see the banner above

/*
	Only once the manifest swap is durable is it safe to delete the input
	files and consume the marker.
*/
for (input = 0; input < input_count; input++)
	delete_segment_files(input_generations[input]);
remove(marker_name);

/*
	The compacted output replaced its inputs (a new engine with its own local
	statistics): repush the collection-wide global statistics so scores stay
	single-segment-equivalent.  The total N is unchanged by a merge, but the
	output engine snapshotted its own locals at append_segment() time.
*/
refresh_global_statistics();

return 0;
}

/*
	TIER_OF()
	---------
	Size tier = number of decimal digits in the live-document count
	(1-9 -> tier 1, 10-99 -> tier 2, ...).  File-local: only maintain() uses it.
*/
static long tier_of(long long live_documents)
{
long tier = 1;

while (live_documents >= 10)
	{
	live_documents /= 10;
	tier++;
	}
return tier;
}

/*
	ATIRE_SEGMENT_INDEX::MAINTAIN()
	-------------------------------
	Run the tiered merge policy (design spec section 4) to quiescence, over
	disk segments only -- the live writer is never a compact() input.

	Triggers, evaluated fresh on every iteration (segment_count/generations
	change after each compact()):
	1. Tier trigger: bucket disk segments by tier_of(live document count);
	   the first tier holding >= merge_factor members has ALL of its members
	   merged in one compact() call.
	2. Tombstone trigger: any segment whose tombstones->count()/document_count
	   exceeds tombstone_compact_ratio joins the candidate set too (added
	   after the tier trigger, deduplicated against it) -- if the tier
	   trigger did not fire, an over-deleted segment is compacted alone
	   (a 1-way "merge", which just rewrites it without its dead documents;
	   see ANT_index_merger::merge()'s N-way walk, which degrades to N=1
	   without any special-casing).

	Stops when neither trigger fires (0 candidates -> quiescent) or after a
	safety cap of 10 iterations, so a pathological inventory that keeps
	re-triggering cannot loop maintain() forever; a later maintain() call
	picks up where this one left off.
*/
long ATIRE_segment_index::maintain(void)
{
long long candidates[1024];
long long candidate_count, which, other;
long iteration;
long any_compacted = 0;

for (iteration = 0; iteration < 10; iteration++)
	{
	candidate_count = 0;

	/*
		Tier trigger: find the first tier with >= merge_factor members
	*/
	for (which = 0; which < segment_count && candidate_count == 0; which++)
		{
		long long in_tier = 0;
		long tier = tier_of(segments[which].engine->get_document_count() - segments[which].tombstones->count());
		for (other = 0; other < segment_count; other++)
			if (tier_of(segments[other].engine->get_document_count() - segments[other].tombstones->count()) == tier)
				in_tier++;
		if (in_tier >= merge_factor)
			for (other = 0; other < segment_count && candidate_count < 1024; other++)
				if (tier_of(segments[other].engine->get_document_count() - segments[other].tombstones->count()) == tier)
					candidates[candidate_count++] = segments[other].generation;
		}

	/*
		Tombstone trigger: over-deleted segments join (or run alone)
	*/
	for (which = 0; which < segment_count && candidate_count < 1024; which++)
		{
		long long docs = segments[which].engine->get_document_count();
		if (docs > 0 && (double)segments[which].tombstones->count() / (double)docs > tombstone_compact_ratio)
			{
			long already_in = false;
			for (other = 0; other < candidate_count; other++)
				if (candidates[other] == segments[which].generation)
					already_in = true;
			if (!already_in)
				candidates[candidate_count++] = segments[which].generation;
			}
		}

	if (candidate_count == 0)
		{
		if (any_compacted)
			keymap->compact_log();		// best-effort; compaction floods log with remap records
		return 0;					// quiescent
		}
	if (compact(candidates, candidate_count) != 0)
		return 1;
	any_compacted = 1;
	}
if (any_compacted)
	keymap->compact_log();				// best-effort; compaction floods log with remap records
return 0;						// safety cap: good enough, next maintain() continues
}

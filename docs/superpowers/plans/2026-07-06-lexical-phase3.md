# Lexical Phase 3 Implementation Plan (WAL, Global Stats, Keymap Compaction)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver spec `docs/superpowers/specs/2026-07-06-lexical-phase3-design.md`: opt-in WAL durability for the memory segment (vectors included), global N/mean-length ranking statistics across segments (default on), and opportunistic keymap log compaction — plus the Node binding rider.

**Architecture:** Three near-independent pieces. (1) `ANT_write_ahead_log` (`source/wal.{h,cpp}`) — length-prefixed binary records, fflush/fsync per record, bounds-checked replay iterator with the torn-tail rule, unhealthy-flag semantics. Coordinator hooks: append after engine success, replay at end of `open()`, truncate on successful `flush()`. (2) Global stats: `ANT_search_engine::set_global_document_statistics(...)` writes the `documents`/`mean_document_length` members, and — the load-bearing subtlety — `ATIRE_API` must RECONSTRUCT its ranking function afterwards, because `ANT_ranking_function`'s constructor snapshots `document_count()` and precomputes per-document priors from `mean_document_length` at `ATIRE_API::open` time (verified: `source/ranking_function.cpp:20-29`, `source/ranking_function_bm25.cpp:29`); the coordinator refreshes at open/flush/compact/NRT-rebuild boundaries. (3) `ANT_index_keymap::compact_log()` + dead-ratio counters + coordinator triggers.

**Tech Stack:** House C++ (pre-C++11 style in `source/`/`atire/`), the established test harness (`tests/<name>.cpp` → `make <name>` → `bin/<name>`; `CHECK` macro; `make_index_dir()`/`unique_term(buffer,n)` in `tests/test_segment_index.cpp`, currently 27 green functions). Node binding: `nodejs/addon/segment_index.cpp` conventions from the binding plan. Build facts: repo-wide `-fPIC`; `npm run build:segment` / `npm run test:segment`; `make engine_lib` before addon builds.

**Verified engine facts:** `ANT_search_engine` members `long long documents` (search_engine.h:84) and `double mean_document_length` (:89) are what ranking reads — but only via the ranking function's own snapshot: `ANT_ranking_function::ANT_ranking_function(engine, ...)` captures `documents_as_integer = engine->document_count()` (ranking_function.cpp:22) and derived ctors precompute `document_prior_probability[]` from `mean_document_length` (ranking_function_bm25.cpp:29). `ATIRE_API::open` constructs the ranking function once (atire_api.cpp ~292-311, branch depends on quantized/readability state — the implementer must read which branch our INDEX_IN_MEMORY non-quantized segments take and factor THAT selection into a reusable private helper). The NRT wrapper path (`open_from_memory_index`) constructs its own ranking function too — same treatment. `ANT_search_engine::get_document_lengths(double *mean)` returns the mean (search_engine.h:154); `document_count()` returns `documents`.

**Worktree:** `.worktrees/lexical-phase3`, branch `feature/lexical-phase3`. Baseline: `mkdir -p obj bin && make all && make tests && make engine_lib`; seven C++ suites + (nodejs: `npm install`, `npm run build:segment`, `npm run test:segment`) 9 JS tests green.

---

### Task 1: Keymap log compaction

**Files:**
- Modify: `source/index_keymap.h` / `source/index_keymap.cpp`
- Modify: `atire/atire_segment_index.cpp` (two trigger sites)
- Test: `tests/test_index_keymap.cpp` (append), `tests/test_segment_index.cpp` (append `test_keymap_log_compaction`)

- [ ] **Step 1: failing unit test — append to `tests/test_index_keymap.cpp` before the final PASSED:**

```cpp
/*
	compact_log(): rewrite the append-only log keeping only live entries
*/
char cl_template[] = "/tmp/ant_keymap_XXXXXX";
char *cl_dir = mkdtemp(cl_template);
CHECK(cl_dir != NULL);
ANT_index_keymap *cl = ANT_index_keymap::load(cl_dir);
long long cl_gen, cl_docid;
char cl_key[64];
for (long long i = 0; i < 100; i++)
	{
	sprintf(cl_key, "churn-%lld", i % 10);		// 10 keys, updated 10x each
	cl->add(cl_key, 1, i);
	}
delete cl;

ANT_index_keymap *cl2 = ANT_index_keymap::load(cl_dir);
CHECK(cl2->log_dead_ratio() > 0.8);				// 100 records, 10 live
char cl_log[1200];
snprintf(cl_log, sizeof(cl_log), "%s/keymap.log", cl_dir);
struct stat before_stat;
CHECK(stat(cl_log, &before_stat) == 0);
CHECK(cl2->compact_log() == 0);
struct stat after_stat;
CHECK(stat(cl_log, &after_stat) == 0);
CHECK(after_stat.st_size < before_stat.st_size / 2);
/* state preserved and still appendable */
for (long long i = 0; i < 10; i++)
	{
	sprintf(cl_key, "churn-%lld", i);
	CHECK(cl2->find(cl_key, &cl_gen, &cl_docid) && cl_docid == 90 + i);
	}
cl2->add("post-compact", 2, 7);
delete cl2;
ANT_index_keymap *cl3 = ANT_index_keymap::load(cl_dir);
CHECK(cl3->log_dead_ratio() < 0.1);
CHECK(cl3->find("post-compact", &cl_gen, &cl_docid) && cl_gen == 2 && cl_docid == 7);
sprintf(cl_key, "churn-%lld", (long long)4);
CHECK(cl3->find(cl_key, &cl_gen, &cl_docid) && cl_docid == 94);
delete cl3;
```
Add `#include <sys/stat.h>` at the top if absent.

- [ ] **Step 2:** `make test_index_keymap` → FAIL (methods undeclared).

- [ ] **Step 3: implement.** `source/index_keymap.h` additions (public):

```cpp
	double log_dead_ratio(void);				// dead replayed records / total, from the last load()
	long compact_log(void);						// rewrite the log keeping live entries; 0 on success
```
Private counters: `long long replayed_records;` (count every A and D record successfully replayed in `load()`, including ones later superseded) — live count is derivable by walking the table.

`source/index_keymap.cpp`:

```cpp
/*
	ANT_INDEX_KEYMAP::LOG_DEAD_RATIO()
	----------------------------------
	Proportion of the replayed log that no longer contributes a live entry.
	0 when the log was empty.
*/
double ANT_index_keymap::log_dead_ratio(void)
{
long long live = 0, which;

if (replayed_records == 0)
	return 0.0;
for (which = 0; which < slots_allocated; which++)
	if (table[which].key != NULL && table[which].docid >= 0)
		live++;
return (double)(replayed_records - live) / (double)replayed_records;
}

/*
	ANT_INDEX_KEYMAP::COMPACT_LOG()
	-------------------------------
	Write a fresh log holding one A record per live entry (temp + rename),
	then reopen the append handle.  On any failure the old log remains
	fully usable -- merely uncompacted, never lost.
*/
long ANT_index_keymap::compact_log(void)
{
char filename[4096], temp_name[4200];
FILE *fresh;
long long which, live = 0;

snprintf(filename, sizeof(filename), "%s/keymap.log", directory);
if (snprintf(temp_name, sizeof(temp_name), "%s.compact", filename) >= (int)sizeof(temp_name))
	return 1;
if ((fresh = fopen(temp_name, "wb")) == NULL)
	return 1;
for (which = 0; which < slots_allocated; which++)
	if (table[which].key != NULL && table[which].docid >= 0)
		{
		if (fprintf(fresh, "A\t%lld\t%lld\t%s\n", table[which].generation, table[which].docid, table[which].key) < 0)
			{
			fclose(fresh);
			remove(temp_name);
			return 1;
			}
		live++;
		}
fclose(fresh);

if (log != NULL)
	{
	fclose(log);
	log = NULL;
	}
if (rename(temp_name, filename) != 0)
	{
	remove(temp_name);
	log = fopen(filename, "ab");		// reopen the old log; state unchanged
	return 1;
	}
log = fopen(filename, "ab");
replayed_records = live;
return 0;
}
```
`load()`: initialize `replayed_records = 0` in the ctor and increment it once per successfully applied A/D record during replay (place the increments beside the existing record handling; malformed/skipped records do NOT count).

- [ ] **Step 4: coordinator triggers** (`atire/atire_segment_index.cpp`):
  1. In `open()`, immediately after the keymap-consistency block completes (after `retain_generations`/rebuild, before `start_new_writer()`): `if (keymap->log_dead_ratio() > 0.5) keymap->compact_log();` (best-effort — ignore the return; comment says why).
  2. In `maintain()`, track whether any `compact()` succeeded in the loop; before returning 0 after at least one success: same best-effort call, with a comment that compaction floods the log with remap records.

- [ ] **Step 5: e2e test — append `test_keymap_log_compaction` to `tests/test_segment_index.cpp`, call from `main()`:**

```cpp
/*
	TEST_KEYMAP_LOG_COMPACTION()
	----------------------------
	Update churn bloats keymap.log; reopen compacts it; state intact.
*/
static void test_keymap_log_compaction(void)
{
char *dir = make_index_dir();
char key[64], doc[256], letters[16], log_name[4096];
long long i, round;
struct stat bloated, compacted;

ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->open(dir) == 0);
for (i = 0; i < 5; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc) >= 0);
	}
CHECK(index->flush() == 0);
for (round = 0; round < 20; round++)
	for (i = 0; i < 5; i++)
		{
		sprintf(key, "doc-%lld", i);
		unique_term(letters, i);
		sprintf(doc, "<DOC>common round%lld %s</DOC>", round, letters);
		CHECK(index->update_document(key, doc) >= 0);
		}
CHECK(index->flush() == 0);
delete index;

snprintf(log_name, sizeof(log_name), "%s/keymap.log", dir);
CHECK(stat(log_name, &bloated) == 0);

ATIRE_segment_index *reopened = new ATIRE_segment_index();
CHECK(reopened->open(dir) == 0);				// dead ratio >> 0.5 -> compacts
CHECK(stat(log_name, &compacted) == 0);
CHECK(compacted.st_size < bloated.st_size / 2);
CHECK(reopened->get_document_count() == 5);
unique_term(letters, 3);
char query[64];
sprintf(query, "round%lld", (long long)19);
CHECK(reopened->search(query, 10) == 5);		// newest bodies live
CHECK(reopened->delete_document("doc-3") == 0);	// keymap still correct
CHECK(reopened->get_document_count() == 4);
delete reopened;
delete [] dir;
printf("test_keymap_log_compaction OK\n");
}
```
(Add `#include <sys/stat.h>` to the test file if absent. If "round19" trips the letter/digit tokeniser split — digits! — replace the round marker with letter suffixes: `sprintf(doc, "<DOC>common r%c%c %s</DOC>", 'a'+round/26, 'a'+round%26, letters)` and query `"r"+letters-for-19` — the implementer adapts using the established unique_term approach. The assertion intent: the LATEST round's term matches all 5 docs.)

- [ ] **Step 6:** `make test_index_keymap && ./bin/test_index_keymap` PASSED; `make test_segment_index && ./bin/test_segment_index` → 28 functions OK + PASSED; other five C++ binaries PASSED; `make internal` exit 0.

- [ ] **Step 7: Commit** — `feat: keymap log compaction with dead-ratio triggers`

---

### Task 2: Global ranking statistics

**Files:**
- Modify: `source/search_engine.h` (setter + saved locals)
- Modify: `atire/atire_api.h` / `atire/atire_api.cpp` (ranking-function selection helper + `apply_global_statistics`)
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp` (`refresh_global_statistics`, `set_global_stats`, refresh points)
- Test: `tests/test_segment_index.cpp` (append `test_global_stats_score_equality`)

- [x] **Step 1: test — append, call from `main()` (as landed; revised from the original draft, see NOTE below):**

```cpp

/*
	FIND_SCORE_BY_KEY()
	-------------------
	Return the score of the hit whose filename is key, from the given
	index's first hits results; CHECK-fails if the key is absent.
	Helper for test_global_stats_score_equality().
*/
static double find_score_by_key(ATIRE_segment_index *index, long long hits, const char *key)
{
long long which;

for (which = 0; which < hits; which++)
	if (strcmp(index->get_hit(which)->filename, key) == 0)
		return index->get_hit(which)->score;
CHECK(!"key not found in results");
return 0.0;
}

/*
	TEST_GLOBAL_STATS_SCORE_EQUALITY()
	----------------------------------
	Global statistics push the collection-wide N and mean document length
	into every segment engine and reconstruct each engine's ranking
	function so it snapshots them.  Per-term document/collection frequency
	stays PER-SEGMENT by design (the spec's section 6 pins global df out of
	scope; per-segment df converges to the collection df as maintain()
	merges segments).  The ranking function in this build is DFR divergence
	(I(ne)B2), whose score is the product of a per-document part driven by
	N and mean length and a per-term part driven by df/cf.  The contract
	verified here is therefore:

	1. A query term whose df is LAYOUT-INVARIANT (confined to documents
	   that share one segment) scores STRICTLY EQUALLY (1e-4) in the multi-
	   and single-segment layouts: every input to the formula is identical.
	2. A term shared ACROSS segments ranks in the SAME ORDER, with a
	   CONSTANT multi/single score ratio across all documents.  A constant
	   ratio proves N and length normalization are globalized -- only the
	   per-term df/cf factor differs between the layouts, and it is the
	   same multiplicative factor for every document.
	3. Negative control: with set_global_stats(0) each segment reverts to
	   its own local N/mean, whose length normalization drifts BY SEGMENT,
	   so the multi/single ratio is measurably NOT constant.
*/
static void test_global_stats_score_equality(void)
{
char *dir_multi = make_index_dir();
char *dir_single = make_index_dir();
char key[64], doc[512], letters[16], query[64];
long long i, which;

/*
	12 docs with VARIED lengths (length normalization is what drifts);
	every doc contains "common"; doc i also has its unique term; docs 4-7
	(exactly the multi index's second segment) additionally share
	"confinedterm", a term whose df is 4 in BOTH layouts.
*/
ATIRE_segment_index *multi = new ATIRE_segment_index();
CHECK(multi->open(dir_multi) == 0);
multi->set_flush_threshold(4);					// three segments
ATIRE_segment_index *single = new ATIRE_segment_index();
CHECK(single->open(dir_single) == 0);

for (i = 0; i < 12; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	/* length varies: i+1 copies of filler words */
	char body[400];
	body[0] = '\0';
	for (which = 0; which <= i; which++)
		strcat(body, "filler ");
	sprintf(doc, "<DOC>common %s %s%s</DOC>", letters, i >= 4 && i <= 7 ? "confinedterm " : "", body);
	CHECK(multi->add_document(key, doc) >= 0);
	CHECK(single->add_document(key, doc) >= 0);
	}
CHECK(multi->flush() == 0);
CHECK(single->flush() == 0);
CHECK(multi->disk_segment_count() == 3);
CHECK(single->disk_segment_count() == 1);

/*
	1. STRICT score equality for the layout-invariant-df term (docs 4-7
	   live together in one multi segment: df=4, cf=4 in both layouts)
*/
strcpy(query, "confinedterm");
long long multi_hits = multi->search(query, 20);
strcpy(query, "confinedterm");
long long single_hits = single->search(query, 20);
CHECK(multi_hits == 4 && single_hits == 4);
for (which = 0; which < 4; which++)
	{
	const char *want = multi->get_hit(which)->filename;
	double multi_score = multi->get_hit(which)->score;
	CHECK(fabs(find_score_by_key(single, single_hits, want) - multi_score) < 1e-4);
	}

/*
	2. Cross-segment shared term: identical rank order and a constant
	   multi/single score ratio (per-term df/cf stays per-segment, so the
	   two layouts' scores differ by exactly one collection-wide factor)
*/
strcpy(query, "common");
multi_hits = multi->search(query, 20);
strcpy(query, "common");
single_hits = single->search(query, 20);
CHECK(multi_hits == 12 && single_hits == 12);
double min_ratio = 0.0, max_ratio = 0.0;
for (which = 0; which < 12; which++)
	{
	/* rank order: the hit at each position names the same document */
	CHECK(strcmp(multi->get_hit(which)->filename, single->get_hit(which)->filename) == 0);
	double ratio = multi->get_hit(which)->score / single->get_hit(which)->score;
	if (which == 0)
		min_ratio = max_ratio = ratio;
	else
		{
		if (ratio < min_ratio)
			min_ratio = ratio;
		if (ratio > max_ratio)
			max_ratio = ratio;
		}
	}
CHECK((max_ratio - min_ratio) / min_ratio < 1e-3);

/*
	3. Negative control: global stats OFF -> each segment scores with its
	   own local N/mean and the multi/single ratio varies by segment
*/
multi->set_global_stats(0);
strcpy(query, "common");
CHECK(multi->search(query, 20) == 12);
double off_min_ratio = 0.0, off_max_ratio = 0.0;
for (which = 0; which < 12; which++)
	{
	const char *want = multi->get_hit(which)->filename;
	double ratio = multi->get_hit(which)->score / find_score_by_key(single, single_hits, want);
	if (which == 0)
		off_min_ratio = off_max_ratio = ratio;
	else
		{
		if (ratio < off_min_ratio)
			off_min_ratio = ratio;
		if (ratio > off_max_ratio)
			off_max_ratio = ratio;
		}
	}
CHECK((off_max_ratio - off_min_ratio) / off_min_ratio > 0.01);	// measured spread with stats off: 0.47 relative
multi->set_global_stats(1);

/*
	NRT: two more docs in the memory segment only.  Their shared term
	"tailmark" is confined to the memory segments of BOTH sides (df=2,
	cf=2 in both layouts), so strict equality must hold for a fresh query
	(the writer engine gets the global override at rebuild).
*/
for (i = 12; i < 14; i++)
	{
	sprintf(key, "doc-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common tailmark %s tail words</DOC>", letters);
	CHECK(multi->add_document(key, doc) >= 0);
	CHECK(single->add_document(key, doc) >= 0);
	}
strcpy(query, "common");
multi_hits = multi->search(query, 20);
strcpy(query, "common");
single_hits = single->search(query, 20);
CHECK(multi_hits == 14 && single_hits == 14);
strcpy(query, "tailmark");
multi_hits = multi->search(query, 20);
strcpy(query, "tailmark");
single_hits = single->search(query, 20);
CHECK(multi_hits == 2 && single_hits == 2);
for (which = 0; which < 2; which++)
	{
	const char *want = multi->get_hit(which)->filename;
	double multi_score = multi->get_hit(which)->score;
	CHECK(fabs(find_score_by_key(single, single_hits, want) - multi_score) < 1e-4);
	}

delete multi;
delete single;
delete [] dir_multi;
delete [] dir_single;
printf("test_global_stats_score_equality OK\n");
}
```
NOTE (contract revision, discovered during implementation): the original draft asserted
strict per-document score equality between the multi- and single-segment layouts for a
term shared across ALL segments ("common"), on the premise that the fixture was
df-neutral.  That premise was wrong: a term present in every document has df = 4 in each
4-document segment but df = 12 in the single segment — per-term df/cf stays per-segment
(spec section 6 pins global df out of scope), and the default ranker (DFR divergence
I(ne)B2, NOT BM25 — see atire_api.cpp's open()/open_from_memory_index()) consumes df and
cf directly, so strict equality for cross-segment terms is unreachable by design.
Measured before the revision: a CONSTANT multi/single ratio of 2.9943 across all 12
documents — constant ratio proves N and mean length ARE correctly globalized (the entire
residual is the one per-term df/cf factor).  The revised contract, as tested above:
(1) strict 1e-4 equality for terms whose df is layout-invariant ("confinedterm" in one
disk segment; "tailmark" in the NRT memory segments); (2) identical rank order plus
constant ratio (relative spread < 1e-3) for the cross-segment term; (3) negative
control — with stats off the ratio spread is > 0.01 (measured: 0.47 relative).
ALSO discovered and fixed en route: under SPECIAL_COMPRESSION, df <= 2 postings are
packed into the vocabulary leaf — "~length" included — and both length loaders
(search_engine.cpp open() and search_engine_memory_index.cpp open()) mis-decoded them,
so 1- and 2-document segments (e.g. any small NRT writer) had garbage/zero document
lengths and NaN length-normalised scores; both loaders now recover the packed values
directly, and refresh_global_statistics() defensively refuses to push a non-finite or
non-positive mean.

- [ ] **Step 2:** run → FAIL (`set_global_stats` undeclared).

- [ ] **Step 3: engine hook.** `source/search_engine.h`, public section:

```cpp
	/*
		Global-statistics override (segmented indexes): replaces the values
		the ranking function will snapshot at its NEXT construction.  Pass
		documents == 0 to restore this engine's own local values.
	*/
	void set_global_document_statistics(long long global_documents, double global_mean_document_length)
	{
	if (local_documents_saved == 0)
		{
		local_documents_saved = documents;
		local_mean_document_length_saved = mean_document_length;
		}
	if (global_documents == 0)
		{
		documents = local_documents_saved;
		mean_document_length = local_mean_document_length_saved;
		}
	else
		{
		documents = global_documents;
		mean_document_length = global_mean_document_length;
		}
	}
```
Private members `long long local_documents_saved;` / `double local_mean_document_length_saved;` initialized to 0/0.0 in the constructor(s) — find every ANT_search_engine ctor and init there.

- [ ] **Step 4: ATIRE_API reconstruction.** In `atire/atire_api.cpp`, read the ranking-function construction in `open()` AND in `open_from_memory_index()` (both paths). Factor the exact selection each performs into a private helper `void construct_default_ranking_function(void)` (delete the old `ranking_function` first if non-NULL, then run the same new-expression the open path used — preserve any quantize/bits arguments verbatim). Replace the inline constructions with helper calls. Then add:

```cpp
/*
	ATIRE_API::APPLY_GLOBAL_STATISTICS()
	------------------------------------
	Overrides the engine's document count and mean length, then rebuilds
	the ranking function -- REQUIRED because ANT_ranking_function snapshots
	document_count() and precomputes per-document priors from
	mean_document_length at construction (ranking_function.cpp:20-29,
	ranking_function_bm25.cpp:29); poking the engine members alone changes
	nothing at query time.  documents == 0 restores local statistics.
*/
void ATIRE_API::apply_global_statistics(long long global_documents, double global_mean_document_length)
{
search_engine->set_global_document_statistics(global_documents, global_mean_document_length);
construct_default_ranking_function();
}
```
Declaration in `atire/atire_api.h`. CAUTION: verify what else hangs off `ranking_function` (feedback ranking function, forum writers) — the helper must only rebuild what open() itself built for our path; if open() also wires `feedback_ranking_function` from the same object, mirror that wiring. Read before writing; report what the selection branch actually is for our segments (non-quantized, INDEX_IN_MEMORY / memory-index wrapper).

- [ ] **Step 5: coordinator.** `atire/atire_segment_index.h`: public `void set_global_stats(long on);` private `long global_stats_enabled;` (ctor: 1) and `void refresh_global_statistics(void);`. Implementation:

```cpp
/*
	ATIRE_SEGMENT_INDEX::REFRESH_GLOBAL_STATISTICS()
	------------------------------------------------
	Pushes global N and mean document length into every open engine (disk
	segments + the NRT wrapper) and rebuilds their ranking functions.  When
	disabled, pushes the restore sentinel instead.  Called at every boundary
	that changes the segment set; O(segments).
*/
void ATIRE_segment_index::refresh_global_statistics(void)
{
long long which, total_documents = 0;
double total_length = 0.0;

if (!global_stats_enabled)
	{
	for (which = 0; which < segment_count; which++)
		segments[which].engine->apply_global_statistics(0, 0.0);
	if (writer_engine != NULL)
		writer_engine->apply_global_statistics(0, 0.0);
	return;
	}

for (which = 0; which < segment_count; which++)
	{
	long long docs = segments[which].engine->get_document_count();
	double mean = 0.0;
	segments[which].engine->get_search_engine()->get_document_lengths(&mean);
	total_documents += docs;
	total_length += (double)docs * mean;
	}
if (writer_engine != NULL)
	{
	double mean = 0.0;
	writer_engine->get_search_engine()->get_document_lengths(&mean);
	total_documents += writer_documents;
	total_length += (double)writer_documents * mean;
	}
else
	total_documents += writer_documents;	/* lengths unknown until a wrapper exists; next refresh corrects */

if (total_documents == 0)
	return;
double global_mean = total_length / (double)total_documents;
for (which = 0; which < segment_count; which++)
	segments[which].engine->apply_global_statistics(total_documents, global_mean);
if (writer_engine != NULL)
	writer_engine->apply_global_statistics(total_documents, global_mean);
}

void ATIRE_segment_index::set_global_stats(long on)
{
global_stats_enabled = on;
if (directory != NULL)
	refresh_global_statistics();
}
```
Refresh points: end of `open()` (before returning 0), end of successful `flush()`, success path of `compact()` (after step 6), and in `rebuild_writer_engine()` immediately after the new wrapper is constructed. CHECK `get_document_lengths`'s exact signature/behavior on a segment engine (it returns the lengths array and writes the mean — confirm it's loaded for INDEX_IN_MEMORY segments; it is for the memory wrapper per the June work). NOTE the writer-without-wrapper case in the code above: on a fresh open with an empty writer this is moot (0 docs); after adds but before any search, disk engines get a slightly stale mean until the next refresh/search — acceptable, documented in the comment.

- [ ] **Step 6:** run the test → green. Then the FULL suite: this change alters default lexical scoring in multi-segment indexes — run `./bin/test_segment_index` (now 29 functions) and scrutinize any failure in EXISTING tests: membership/count assertions must be unaffected (scores aren't asserted lexically anywhere except vector/hybrid paths, which don't use BM25 stats — if something fails, investigate rather than force; report findings). All seven binaries + `make internal`.

- [ ] **Step 7: Commit** — `feat: global document statistics across segments with ranking-function rebuild`

---

### Task 3: `ANT_write_ahead_log` (core, unit-tested)

**Files:**
- Create: `source/wal.h` / `source/wal.cpp`
- Test: `tests/test_wal.cpp`

- [ ] **Step 1: failing unit test:**

```cpp
/*
	TEST_WAL.CPP
	------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wal.h"

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

int main(void)
{
char dir_template[] = "/tmp/ant_wal_XXXXXX";
char *dir = mkdtemp(dir_template);
CHECK(dir != NULL);
float vec[3] = {0.5f, 0.25f, 0.0f};

/*
	Append three ops, one with a vector
*/
ANT_write_ahead_log *wal = ANT_write_ahead_log::open(dir, 3);
CHECK(wal != NULL);
CHECK(wal->healthy());
CHECK(wal->append('A', "doc-1", "<DOC>alpha</DOC>", NULL) == 0);
CHECK(wal->append('U', "doc-1", "<DOC>beta</DOC>", vec) == 0);
CHECK(wal->append('D', "doc-2", NULL, NULL) == 0);
delete wal;

/*
	Replay: exact records back, in order
*/
ANT_write_ahead_log *replay = ANT_write_ahead_log::open(dir, 3);
ANT_write_ahead_log::record record;
CHECK(replay->replay_next(&record));
CHECK(record.op == 'A' && strcmp(record.key, "doc-1") == 0 && strcmp(record.document, "<DOC>alpha</DOC>") == 0 && record.vector == NULL);
CHECK(replay->replay_next(&record));
CHECK(record.op == 'U' && record.vector != NULL && record.vector[1] == 0.25f);
CHECK(replay->replay_next(&record));
CHECK(record.op == 'D' && strcmp(record.key, "doc-2") == 0 && record.document == NULL);
CHECK(!replay->replay_next(&record));			// clean end

/*
	Truncate resets to empty
*/
CHECK(replay->truncate() == 0);
delete replay;
ANT_write_ahead_log *empty = ANT_write_ahead_log::open(dir, 3);
CHECK(!empty->replay_next(&record));
CHECK(empty->append('A', "doc-3", "<DOC>gamma</DOC>", NULL) == 0);
delete empty;

/*
	Torn tail: append garbage bytes; replay yields the good record then stops
*/
char wal_name[1200];
snprintf(wal_name, sizeof(wal_name), "%s/wal.log", dir);
FILE *fp = fopen(wal_name, "ab");
fputc('A', fp);
fputc(0x42, fp);								// half a record
fclose(fp);
ANT_write_ahead_log *torn = ANT_write_ahead_log::open(dir, 3);
CHECK(torn->replay_next(&record));
CHECK(record.op == 'A' && strcmp(record.key, "doc-3") == 0);
CHECK(!torn->replay_next(&record));				// tear ends replay, no crash
delete torn;

/*
	Bounds: a fabricated record claiming a huge key length is rejected
	(replay ends) without attempting the allocation
*/
CHECK(truncate(wal_name, 0) == 0);
fp = fopen(wal_name, "ab");
unsigned char op = 'A';
int32_t bad_len = 1 << 30;
fwrite(&op, 1, 1, fp);
fwrite(&bad_len, sizeof(bad_len), 1, fp);
fclose(fp);
ANT_write_ahead_log *bad = ANT_write_ahead_log::open(dir, 3);
CHECK(!bad->replay_next(&record));
delete bad;

printf("PASSED\n");
return 0;
}
```

- [ ] **Step 2:** `make test_wal` → compile FAILURE.

- [ ] **Step 3: implement.** `source/wal.h`:

```cpp
/*
	WAL.H
	-----
	Write-ahead log for the segmented index's memory segment (see
	docs/superpowers/specs/2026-07-06-lexical-phase3-design.md section 1).

	Record format (binary, little-endian, length-prefixed):
		uint8  op ('A' | 'U' | 'D')
		int32  key_length      + key bytes            (1..8192)
		int64  document_length + document bytes       (0..256MB; 0 for 'D')
		uint8  has_vector      + dimension float32s when 1

	Appends are fflush()ed per record (optionally fsync()ed).  Replay stops
	at the first short read or out-of-bounds field: the torn-tail rule.  A
	failed append marks the log unhealthy; the engine state remains the
	source of truth and the next truncate() restores health.
*/
#ifndef WAL_H_
#define WAL_H_

#include <stdio.h>
#include <stdint.h>

class ANT_write_ahead_log
{
public:
	struct record
	{
	char op;
	char *key;			// owned by the log object; valid until the next replay_next/destruction
	char *document;		// NULL for 'D'
	float *vector;		// NULL when absent
	} ;

private:
	FILE *fp;			// open "a+b": append writes, seekable reads
	long long dimension;	// 0 = vectors disabled
	long is_healthy;
	long use_fsync;
	long long replay_position;
	char *key_buffer;
	char *document_buffer;
	float *vector_buffer;

private:
	ANT_write_ahead_log();

public:
	~ANT_write_ahead_log();

	static ANT_write_ahead_log *open(const char *directory, long long vector_dimension);	// NULL only on unopenable file
	long append(char op, const char *key, const char *document_or_null, const float *vector_or_null);	// 0 on success
	long replay_next(record *into);			// 1 = record produced; 0 = end (clean or torn)
	long truncate(void);					// empty the log; restores health; 0 on success
	long healthy(void) { return is_healthy; }
	void set_fsync(long on) { use_fsync = on; }
	long long size(void);					// current byte size (tests/diagnostics)
} ;

#endif /* WAL_H_ */
```

`source/wal.cpp` — write it in house style with banner comments; behavior:
- `open()`: snprintf path `<dir>/wal.log` (truncation → NULL); `fopen(path, "a+b")` (creates if absent) → NULL on failure; `replay_position = 0`; buffers NULL; healthy = 1; fsync off.
- `append()`: if `!is_healthy` return 1 (silently skipped by callers). Validate: op in {A,U,D}; key non-NULL, 1..8192 bytes; document length ≤ 256MB ('D' → write length 0, no bytes even if a document was passed); vector only when `dimension > 0`. `fseek(fp, 0, SEEK_END)`; write the fields; any short write → `is_healthy = 0`, return 1. `fflush`; if `use_fsync` then `fsync(fileno(fp))`. Return 0.
- `replay_next()`: `fseek` to `replay_position`; read op (EOF → return 0); validate op else return 0; read+bounds-check key_length (1..8192) else 0; (re)allocate `key_buffer` to length+1, read, NUL-terminate — short read → 0; same for document (0..256MB; length 0 → `document = NULL`); read has_vector (must be 0, or 1 with dimension > 0) else 0; when 1, read `dimension` floats into `vector_buffer` — short read → 0. Update `replay_position = ftell(fp)` ONLY after a fully valid record; fill `into` with buffer pointers; return 1. (Buffers reused across calls — documented in the struct comment.)
- `truncate()`: `fclose`; `fopen(path, "wb")` + `fclose` (empties); reopen "a+b"; `replay_position = 0`; `is_healthy = 1`; 0 on success, 1 on reopen failure (then unhealthy).
- `size()`: fseek END + ftell (restore nothing — appends seek END anyway; replay seeks its own position).
- dtor: fclose, free the three buffers.

- [ ] **Step 4:** `make test_wal && ./bin/test_wal` → PASSED. Other suites unaffected; `make internal` exit 0.

- [ ] **Step 5: Commit** — `feat: write-ahead log with torn-tail-tolerant replay`

---

### Task 4: WAL integration in the coordinator

**Files:**
- Modify: `atire/atire_segment_index.h` / `atire/atire_segment_index.cpp`
- Test: `tests/test_segment_index.cpp` (append `test_wal_durability`)

- [ ] **Step 1: failing test — append, call from `main()`:**

```cpp
/*
	TEST_WAL_DURABILITY()
	---------------------
	With set_durable(1), everything since the last flush survives a crash
	(destruction without flush): adds, updates, deletes, vectors.
*/
static void test_wal_durability(void)
{
char *dir = make_index_dir();
char key[64], doc[256], letters[16], query[64], wal_name[4096];
float va[4] = {1.0f, 0.0f, 0.0f, 0.0f};
float vb[4] = {0.0f, 1.0f, 0.0f, 0.0f};
long long i;
struct stat wal_stat;

/*
	Session 1: durable index; one flushed segment; then unflushed churn
*/
ATIRE_segment_index *index = new ATIRE_segment_index();
CHECK(index->set_vector_config(4, ATIRE_segment_index::VECTOR_METRIC_DOT) == 0);
CHECK(index->set_durable(1) == 0);
CHECK(index->open(dir) == 0);
CHECK(index->wal_healthy());
for (i = 0; i < 3; i++)
	{
	sprintf(key, "base-%lld", i);
	unique_term(letters, i);
	sprintf(doc, "<DOC>common %s</DOC>", letters);
	CHECK(index->add_document(key, doc, va) >= 0);
	}
CHECK(index->flush() == 0);
snprintf(wal_name, sizeof(wal_name), "%s/wal.log", dir);
CHECK(stat(wal_name, &wal_stat) == 0);
CHECK(wal_stat.st_size == 0);					// truncated by flush

unique_term(letters, 10);
sprintf(doc, "<DOC>fresh %s</DOC>", letters);
CHECK(index->add_document("wal-add", doc, vb) >= 0);
unique_term(letters, 0);
sprintf(doc, "<DOC>revised %s</DOC>", letters);
CHECK(index->update_document("base-0", doc, vb) >= 0);
CHECK(index->delete_document("base-1") == 0);
CHECK(stat(wal_name, &wal_stat) == 0);
CHECK(wal_stat.st_size > 0);
delete index;									// crash: no flush

/*
	Session 2: durable reopen replays the WAL
*/
ATIRE_segment_index *recovered = new ATIRE_segment_index();
CHECK(recovered->set_durable(1) == 0);
CHECK(recovered->open(dir) == 0);
CHECK(recovered->get_document_count() == 3);	// 3 base - 1 deleted + 1 added, update net 0
strcpy(query, "fresh");
CHECK(recovered->search(query, 10) == 1);
strcpy(query, "revised");
CHECK(recovered->search(query, 10) == 1);
unique_term(letters, 1);
strcpy(query, letters);
CHECK(recovered->search(query, 10) == 0);		// base-1 deleted
/* vectors replayed: vb-direction query finds the two vb docs */
CHECK(recovered->search_vector(vb, 10) >= 2);
CHECK(strcmp(recovered->get_hit(0)->filename, "wal-add") == 0 || strcmp(recovered->get_hit(0)->filename, "base-0") == 0);
/* replay did not re-log: WAL size unchanged after reopen */
struct stat after_replay;
CHECK(stat(wal_name, &after_replay) == 0);
CHECK(after_replay.st_size == wal_stat.st_size);
CHECK(recovered->flush() == 0);
CHECK(stat(wal_name, &after_replay) == 0);
CHECK(after_replay.st_size == 0);
delete recovered;

/*
	Relaxed mode ignores an existing WAL (no replay, no deletion)
*/
ATIRE_segment_index *relaxed = new ATIRE_segment_index();
CHECK(relaxed->open(dir) == 0);
CHECK(relaxed->get_document_count() == 3);		// all durable now anyway
delete relaxed;

delete [] dir;
printf("test_wal_durability OK\n");
}
```

- [ ] **Step 2:** run → FAIL (`set_durable` undeclared).

- [ ] **Step 3: implement.** Header: public `long set_durable(long on);` (before open, like set_vector_config — return 1 if already open), `void set_wal_fsync(long on);`, `long wal_healthy(void);` (1 when healthy OR disabled); private `long durable; long wal_fsync_pending; ANT_write_ahead_log *wal; long wal_replaying;` (ctor zeros; dtor deletes wal). Include `../source/wal.h`.

Integration points in `atire/atire_segment_index.cpp`:
1. `open()`: after everything else succeeds (global-stats refresh included), when `durable`: `wal = ANT_write_ahead_log::open(directory, vector_dimension_current);` (NULL → return 1); apply pending fsync flag; then replay: `wal_replaying = 1;` loop `replay_next` — 'A' → `add_document(record.key, record.document, record.vector)` (three-arg handles NULL vector), 'U' → `update_document(...)`, 'D' → `delete_document(record.key)`; ignore individual failures (comment: mirrors what the original caller saw); `wal_replaying = 0;`. IMPORTANT: replay uses the PUBLIC methods, whose append hooks must check `wal_replaying` (below).
2. Append hooks — one per public mutator, AFTER engine success, before return, guarded by `wal != NULL && !wal_replaying`:
   - `add_document` (three-arg core caller — put the hook where the SUCCESS handle exists; the two-arg overload flows through the same path with vector NULL): `wal->append('A', key, document, vector)`.
   - `update_document` (three-arg): `wal->append('U', key, document, vector)` after success.
   - `delete_document`: `wal->append('D', key, NULL, NULL)` after success.
   CAUTION on placement: `add_document`'s auto-flush fires INSIDE the call after the keymap add — the WAL append must happen BEFORE the auto-flush check (else flush truncates, then the append re-adds a record for an already-durable doc → replay would duplicate it). Trace the exact current ordering in `add_document_core` and place the append immediately after the success point and BEFORE the auto-flush check, mirroring how the vector-buffer ordering trap was solved. For `update_document`, the underlying `add_document` call must NOT log an 'A' for what is really an update: guard — the public `update_document` sets a `wal_suppress_add` flag around its `add_document` call (or refactor: core takes an `log_op` char). Pick the flag approach; document it.
3. `flush()`: on the success path (right before `return 0`), when `wal != NULL`: `wal->truncate()` (best-effort; failure leaves records that will harmlessly replay against already-durable state — comment this).
4. `wal_healthy()`: `return wal == NULL ? 1 : wal->healthy();`

- [ ] **Step 4: unhealthy-WAL test — append inside the same test function or as `test_wal_unhealthy` (implementer's choice, keep assertions):** open durable, add one doc, `chmod(dir, 0500)`, add another doc — CHECK it still returns a valid handle AND `wal_healthy() == 0`; `chmod(dir, 0700)` back; `flush()` succeeds; `wal_healthy() == 1` again.

- [ ] **Step 5:** full suite: `test_segment_index` → 30 functions (or 31 with the separate unhealthy test) OK + PASSED; all seven binaries; `make internal` exit 0.

- [ ] **Step 6: Commit** — `feat: WAL durable mode with replay, truncation, and health recovery`

---

### Task 5: Node binding rider + full sweep

**Files:**
- Modify: `nodejs/addon/segment_index.cpp` (three options)
- Modify: `nodejs/segment_index.d.ts`, `nodejs/README.md`
- Test: `nodejs/test/segment_index.test.js` (append)

- [ ] **Step 1: failing JS test — append:**

```js
test('durable mode recovers unflushed writes across sessions', async () => {
	const dir = freshDir();
	let idx = new SegmentIndex({ durable: true, dimension: 4, metric: 'dot' });
	idx.open(dir);
	idx.addDocument('doc-1', '<DOC>alpha survivor</DOC>', Float32Array.from([1, 0, 0, 0]));
	idx.close();		// no flush: relaxed mode would lose this

	idx = new SegmentIndex({ durable: true, dimension: 4, metric: 'dot' });
	idx.open(dir);
	assert.strictEqual(idx.documentCount(), 1);
	assert.strictEqual(idx.search('survivor', 5).length, 1);
	assert.strictEqual(idx.searchVector(Float32Array.from([1, 0, 0, 0]), 5).length, 1);
	await idx.flush();
	idx.close();
});

test('globalStats option round-trips', () => {
	const idx = new SegmentIndex({ globalStats: false });
	idx.open(freshDir());
	idx.addDocument('doc-1', '<DOC>alpha</DOC>');
	assert.strictEqual(idx.search('alpha', 5).length, 1);	// still functional
	idx.close();
});
```

- [ ] **Step 2: implement.** In the wrapper constructor's options parsing: `durable` (bool → store; applied via `engine->set_durable(1)` in `Open()` BEFORE `engine->open`), `walFsync` (bool → `engine->set_wal_fsync(1)` after construction, before open is fine), `globalStats` (bool, default true → when false call `engine->set_global_stats(0)` before open). Update `segment_index.d.ts` (`durable?: boolean; walFsync?: boolean; globalStats?: boolean;`) and the README options table/paragraph.

- [ ] **Step 3: full sweep, paste outputs:** `make engine_lib && cd nodejs && npm run build:segment && npm run test:segment` → 11/11 (twice); all seven C++ binaries PASSED (`test_segment_index` at its new count, run twice); `make internal` exit 0; `git status --short` clean after commit.

- [ ] **Step 4: Commit** — `feat: durable/walFsync/globalStats options in the Node binding`

---

## Self-review record

- **Spec coverage:** §1.1-1.4 (opt-in set_durable, record format+bounds, append-after-success, fflush+fsync toggle, truncate-on-flush-only, replay via public methods with re-log suppression, torn tail, unhealthy semantics, relaxed-mode ignore) → Tasks 3/4; §2.1-2.2 (engine setter with local-restore, ATIRE_API reconstruction with the snapshot rationale, coordinator refresh + all four refresh points, default-on + opt-out) → Task 2; §3 (counters, dead ratio, compact_log, both triggers) → Task 1; §4 API summary → Tasks 1-4 signatures match verbatim + binding rider Task 5; §5 tests: every listed scenario has a concrete test (headline score equality WITH negative control and NRT case; WAL crash/truncate/torn/unhealthy/relaxed; keymap churn-shrink-reload; JS durable-reopen) — fsync toggle covered as a smoke inside Task 4's flow (set_wal_fsync exercised in the unhealthy test setup; acceptable); §6 out-of-scope respected.
- **Risks flagged in-task:** ranking-function selection branch + feedback-object wiring (Task 2 step 4 CAUTION), the WAL-append vs auto-flush ordering trap and the update-suppresses-add flag (Task 4 step 3), score-equality tolerance escalation rule (Task 2 step 1 note), tokeniser digit-split in the churn test (Task 1 step 5 note).
- **Type consistency:** `ANT_write_ahead_log::{open,append,replay_next,truncate,healthy,set_fsync,size}` and `record` used identically in Tasks 3/4; coordinator API names match spec §4 verbatim; `apply_global_statistics(0, 0.0)` restore sentinel consistent between engine hook, ATIRE_API, and coordinator.

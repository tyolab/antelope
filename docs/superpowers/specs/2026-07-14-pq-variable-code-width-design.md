# Dense PQ Variable Code-Width (k≠256) Design (#22, sub-project 3 of 3)

**Status:** approved 2026-07-14. Issue **#22** — advanced PQ codec options. This spec covers **sub-project 3: a variable code-width `k` for the dense `.pq` store**, replacing the hard-wired `k=256` (one byte per subvector). Sub-project 1 (OPQ, #22.1) shipped 647aa7c; sub-project 2 (global codebook, #22.2) shipped 5bf7025. This is the last of the three.

**Goal:** let `k` be a power of two in `[2, 256]` (bits = log₂k ∈ {1..8}) so codes **bit-pack below a byte per subvector** — a smaller `.pq` (both the codes region *and* the codebook shrink), trading recall for size. Opt-in, persisted, deterministic. Default `k=256` is byte-identical to today. Composes with OPQ (#22.1), the global codebook (#22.2), and the resident tiers (#19).

**Architecture (one sentence):** `ANT_pq_codec`'s compile-time `K=256` becomes a runtime `k` parameter on every codec entry point (the k-means/ADC math is otherwise unchanged), two new pure `pack_codes`/`unpack_codes` helpers translate between unpacked byte-codes and a bit-packed per-row layout, and `ANT_pq_store` stores `documents·row_bytes` packed codes (`row_bytes = (m·bits + 7)/8`) behind a `.pq` v3 format — so at `k=256` (bits=8) packing is the identity and the on-disk bytes are unchanged.

**Tech stack:** C++ engine/codec — `source/pq_codec.{h,cpp}` (k-param + pack/unpack), `source/pq_store.{h,cpp}` (row layout, `.pq` v3, ADC-table size, all code read/write sites), `atire/atire_segment_index*` (`set_pq_k`, `pq.config` v5, global-codebook sizing, compaction), tests `tests/*.cpp`.

**Scope:** engine + codec only, dense `.pq` only (token `.mvpq` variable-k is a follow-up). No binding changes. Default off ⇒ per-segment `k=256` path byte-identical to today.

---

## 1. Chosen shape (and rejected alternatives)

- **Bit-pack codes (chosen), not merely a smaller codebook.** A smaller `k` with byte-per-code (rejected "A") shrinks only the codebook (`dimension·k·4` bytes — fixed, independent of document count), while the codes region (`documents·m` bytes) dominates `.pq` size at scale; A delivers almost nothing. **B — bit-packing the codes** — is what the issue asks for and is where the size win lives (and it shrinks the codebook too, as a bonus).
- **Per-row, byte-aligned packing (chosen), not a fully-packed bitstream.** Each document's `m` codes pack into `row_bytes = (m·bits + 7)/8` bytes; the codes region is `documents·row_bytes`. This preserves the `codes + docid·row_bytes` O(1) random access the scan/score hot path relies on. A fully-packed stream (`documents·m·bits` bits, no per-row padding) saves <1 byte/doc but needs cross-document bit-offset arithmetic per lookup — not worth it (YAGNI).
- **Codec math on unpacked byte-codes + pack/unpack helpers (chosen), not packed-aware kernels.** The k-means, `encode`, `adc_table`, `adc_score`, and `reconstruct` math is unchanged except that `K` becomes a runtime `k` argument; packing is confined to two pure helpers plus the store. Cleaner and keeps the numeric contract identical.

## 2. Codec: `k` as a runtime parameter (`source/pq_codec.{h,cpp}`)

- Keep `enum { K = 256, KMEANS_ITERS = 25 }` as the **default** value, but add a `long long k` parameter to:
  - `train(vectors, dimension, m, k, n, codebook)` — codebook is `m·k·sub` floats; the k-means loop bound changes `K → k`; `n==0` memsets `m·k·sub`; empty-cluster-keeps-prior and first-k-distinct init unchanged (with `k` distinct instead of 256). Deterministic/byte-identical contract preserved.
  - `encode(vector, dimension, m, k, codebook, codes)` — assigns each subvector to the nearest of `k` centroids; `codes[s]` is an **unpacked byte** in `[0,k)`. (Packing is the caller's job.)
  - `adc_table(query, dimension, m, k, codebook, metric, table)` — table is `m·k` doubles, `table[s·k + c]`.
  - `adc_score(codes, m, k, table)` — `codes` is unpacked bytes; `total += table[s·k + codes[s]]`. (Gains a `k` param because the table stride is now `k`, not 256.)
  - `reconstruct(codes, dimension, m, k, codebook, out)` — unpacked-byte `codes`; centroid stride `k`.
- `train_rotation`/`apply_rotation`/`apply_rotation_transpose` are **unchanged** — the OPQ rotation `R` is independent of `k`.
- **New pure helpers (no k-means, no file I/O):**
  - `static long bits_for_k(long long k)` — returns log₂k for a power of two in `[2,256]`, else −1 (validation helper; `k` must be a power of two ≤256).
  - `static void pack_codes(const unsigned char *codes, long long m, long long bits, unsigned char *packed)` — packs `m` byte-codes (each `< 2^bits`) LSB-first into `row_bytes = (m·bits+7)/8` bytes, deterministic. At `bits==8` this is a straight `memcpy` (identity).
  - `static void unpack_codes(const unsigned char *packed, long long m, long long bits, unsigned char *codes)` — inverse; writes `m` byte-codes. At `bits==8`, `memcpy`.

**Packing layout (normative):** codes are concatenated LSB-first into a little bit-stream within the row: code `s` occupies bit positions `[s·bits, (s+1)·bits)`, bit `j` of code `s` at byte `(s·bits+j)/8`, bit `(s·bits+j)%8` (bit 0 = LSB). Trailing bits in the last byte are zero. `pack`/`unpack` are exact inverses for all `bits ∈ {1..8}`.

## 3. Store: row layout + `.pq` v3 (`source/pq_store.{h,cpp}`)

- **Header:** the `.pq` `k` field (already present, always 256 today) now holds the actual `k`. Bump `.pq` version **v2 → v3** (one-way, mirrors OPQ's v1→v2): v3 files may carry a non-256 `k`; v2 files are read as `k=256`/byte codes. Loader **validates** `k` is a power of two in `[2,256]` (via `bits_for_k`), computes `bits`/`row_bytes`, and sizes the codes region as `documents·row_bytes` (at `k=256`, `row_bytes==m`, so v2 and pre-existing files load byte-identically). Forgiving-load contract preserved: any mismatch (bad `k`, wrong file size) ⇒ degraded empty store.
- **Member:** store caches `bits` and `row_bytes` (derived from `k`/`m` at load).
- **Codes region:** `documents·row_bytes` (was `documents·m`).
- **Read sites** (`reconstruct`, `score`, `score_prepared`, `scan_adc`, `codes_for`): each unpacks the doc's row into a small stack buffer `unsigned char code_bytes[m]` (m ≤ dimension) via `unpack_codes(codes + docid·row_bytes, m, bits, code_bytes)`, then calls the existing `adc_score`/`reconstruct` on `code_bytes`. ADC table is `m·k` (allocations at the three build sites change `K → k`). `codes_for(docid)` returns a pointer to the **packed** row (`codes + docid·row_bytes`); callers that need code values must `unpack_codes` (tests updated accordingly).
- **Writer** (`ANT_pq_store_writer`): `create(...)` gains `k` (alongside `m`, `metric`, `opq`). `finish()` trains at `k` (or uses the external codebook under #22.2), then for each row `encode → pack_codes → write row_bytes`. `set_external_codebook` unchanged in signature (the codebook is `m·k·sub` floats; `k` comes from `create`).

## 4. Config + engine (`atire/atire_segment_index*`)

- `long set_pq_k(long k)` — opt-in, requires open + `pq_configured()`; `k` a power of two in `[2,256]` (rejects otherwise); idempotent same-value; **immutable once set** to a non-default; persisted. Composes with `pq_posture`/`pq_opq`/global/tier (orthogonal). Mirrors `set_pq_opq`/`set_pq_global_codebook` exactly.
- Getter `long pq_k(void)` returning the configured `k` (default 256).
- **`pq.config` v4 → v5:** append a `k` i64 after the `global` field. Back-compat: absent (v1–v4) ⇒ `k=256`. Loader validates `k` is a power of two in `[2,256]` on read; invalid ⇒ treat as unconfigured/degrade (mirror the existing field-validation posture).
- **Wire `k` into every `.pq` writer** (flush-time eager build, `build_pq`, compaction, `rebuild_pq_global_codebook`): pass the configured `k` to `create(...)`.
- **Global codebook (#22.2):** `global_pq_codebook` is `m·k·sub` floats; `ensure_global_pq_codebook`/`rebuild_pq_global_codebook` train/allocate at `k`; the `pq.codebook` sidecar already stores `k` in its header — write the configured `k`, and its loader validates `k` matches the index config (mismatch ⇒ forgiving-degrade to NULL, same as today).
- **Tiers (#19):** unchanged — NONE-tier reconstruct goes through unpack→`reconstruct`; INT8 `.pqr` and resident float are independent of `k`.

## 5. Testing

- **Codec pack/unpack round-trip:** for every `bits ∈ {1..8}` and representative `m`, `unpack_codes(pack_codes(x)) == x` for random byte-codes `< 2^bits`; `bits==8` pack is a `memcpy` (identity). `bits_for_k` accepts {2,4,8,…,256}, rejects non-powers-of-two and >256.
- **Codec k-param:** `train`/`encode`/`adc_table`/`adc_score`/`reconstruct` at `k=16` and `k=64` produce self-consistent results (encode→adc_score ranks the true nearest centroid highest; reconstruct returns the assigned centroid). `k=256` byte-identical to the pre-change codec (codebook bytes + codes + table).
- **Store round-trip at k≠256:** write a store at `k=16`, reload, `document_count`/`reconstruct`/`scan_adc` correct; codes region size == `documents·row_bytes`; `.pq` is v3. A v2 (`k=256`) file still loads (row_bytes==m).
- **`.pq` size shrinks:** at `k=16` (bits=4) the codes region is ~half a `k=256` store of the same docs; assert `row_bytes == (m·4+7)/8`.
- **Recall vs exact-float:** at `k=256` recall matches Phase-1; at `k=16` recall degrades but rerank posture recovers exact top-k (locks the tradeoff direction, not a fixed number).
- **Config round-trip:** `set_pq_k(16)` persists to `pq.config` v5; reopen restores `pq_k()==16`; a v4 config (no `k`) loads as 256; invalid `k` rejected. Immutability + idempotent-same-value enforced.
- **Composition:** `set_pq_k(64)` + `set_pq_opq(1)` (rotate then encode at k=64; recall gain preserved); `set_pq_k(16)` + `set_pq_global_codebook(1)` (global codebook is `m·16·sub`; cross-segment codes comparable; sidecar carries k=16); compaction under `k≠256` reuses/rebuilds correctly.
- **Default off byte-identity:** no `set_pq_k` ⇒ `k=256` ⇒ existing PQ suites (`test_pq_store/config/compaction/opq/global/metrics/resident_tier/hnsw`) unchanged.
- **Forgiving load:** a `.pq` with a non-power-of-two or >256 `k`, or a size-inconsistent codes region, ⇒ degraded empty store (fallback), no crash/over-read. ASan/UBSan sweep (environment-blocked per prior sub-projects — report).

## 6. Sequencing (TDD tasks)

1. **Codec k-param + pack/unpack** (`pq_codec.{h,cpp}`): thread `k` through train/encode/adc_table/adc_score/reconstruct; add `bits_for_k`/`pack_codes`/`unpack_codes`. Tests: pack/unpack round-trip all bit-widths, k-param self-consistency, `k=256` byte-identity.
2. **Store row layout + `.pq` v3 + config v5 + `set_pq_k`** (`pq_store.{h,cpp}`, `atire/*`): row-packed codes, v3 load/store + forgiving validation, `bits`/`row_bytes` caching, all read sites unpack, writer `create(k)`+`encode→pack`, ADC-table `m·k`, `set_pq_k` + `pq.config` v5. Tests: store round-trip k≠256, size-shrink, config round-trip/immutability, default-off byte-identity, forgiving load.
3. **Global codebook + compaction + composition** (`atire/*`): size `global_pq_codebook`/ensure/rebuild at `k`; sidecar `k`; wire `k` into all writer sites incl. compaction. Tests: OPQ+k, global+k (cross-segment comparability, sidecar k), compaction under k≠256, recall-vs-exact tradeoff.

## 7. Repo constraints

Preserve the `.pq` deterministic-rebuild + forgiving-load contracts (variable-`k` extends them — same `k`+inputs ⇒ same packed codes). `.pq` v2→v3 and `pq.config` v4→v5 are add-a-field/one-way bumps mirroring #22.1/#22.2 (old versions still load). Header changes → `rm -f obj/*.o lib/libantelope_engine.a`; fresh worktree `mkdir -p obj bin lib` + copy `external/**/*.a`; `source/*.cpp`+`tests/*.cpp` auto-discovered → `bin/<name>` via `make <name>`; config setters POST-open; `pq_k`/`set_pq_k` mirror the `set_pq_opq`/`set_pq_global_codebook` immutable-once pattern. Confirm signatures/line numbers by grep before editing.

# spoor — Agent-Facing Local Code Search (MVP) Design

**Status:** approved (design) 2026-09-03. First sub-project of the **spoor** product line: a thin, end-to-end vertical slice that turns the existing Antelope engine into an agent-facing local code-search tool, competitive with Alibaba Zvec's `zg` (zvec-grep) but differentiated on native/embedded use and multi-vector + PQ compression.

**Motivation:** `zg` stitches ripgrep + BM25 + vector search behind one local-first CLI+MCP for AI agents, reportedly cutting agent tokens ~37%, tool calls ~43%, latency ~39% when plugged into Claude Code / Codex / Cursor. Antelope's *engine* already meets or beats `zg`'s retrieval core (lexical BM25/DFR + dense ANN + late-interaction multi-vectors + a heavy token-PQ compression stack). What Antelope lacks is the **agent-facing product layer**: a repo ingester, a code-aware chunker, an embedder pipeline, token-budgeted output, and an MCP/CLI that drops into a coding agent. spoor is that layer — a wrapper, not a new engine.

**Design partner:** `work3-agent#5` (building **Parley**, the inter-agent messaging layer, on the **tyode** harness). tyode auto-merges stdio MCP servers into every agent's config at launch, so a standard spoor MCP server auto-installs fleet-wide (work2/work3/oldnuc/elitebook2/watch1). Parley reuses spoor's `path:start-end` reference format so search hits and cross-agent messages share one coordinate system.

---

## 1. Scope

**In scope (this MVP):** a walking skeleton that does the whole loop end-to-end for real repos:
repo ingest → symbol-aware chunk → embed → Antelope vector + lexical index → RRF fuse → token-budgeted `search` result → over a stdio MCP server + a `spoor` CLI.

**Two embedding representations behind one identical `search` contract** (§5): `multi` (ColBERT multi-vector, our reference service — the premium/default-when-available tier and headline differentiator) and `single` (any standard OpenAI-compatible `/embeddings` API — the universal fallback so spoor works for users who can't run the ColBERT service). Representation is an internal quality tier; the calling agent sees the same `search` schema either way.

**Host language:** **Node.js**, over the existing hand-written Node-API `SegmentIndex` addon (which already exposes lexical + vector + multivector + PQ). Chosen for Antelope's strong existing Node integration and parity with `zg`'s Node ecosystem (npx install).

**Explicitly deferred to follow-on sub-projects (NOT in this spec):**
- Management dashboard for embedder config (MVP uses env/config file).
- Additional tree-sitter grammars beyond JS/TS + Python (Go, Rust, C/C++).
- Code-tuned fine-tuning of the ColBERT model.
- Sparse-vector leg; a dedicated fused regex/exact-match "verify" leg beyond lexical BM25.
- Auto-detection niceties for the embedder (MVP: representation is set by config, with a simple capability probe; richer auto-negotiation is later).

(Note: single-vector via a *remote* standard `/embeddings` API is **in scope** as the fallback tier — see §5 — so remote embedding is supported for `single`. Only the *multi-vector remote/hosted* tier is deferred; MVP serves ColBERT locally.)

**Out of scope entirely:** any change to the Antelope C++ engine internals. spoor consumes the engine through the Node addon as-is. If a required surface is missing on the addon, that is a separate, flagged engine task — not part of spoor.

---

## 2. Architecture

Five modules in one Node process, plus one out-of-process embedding service.

```
spoor (Node)
├─ ingester   walk repo (gitignore-aware); content-hash + mtime → dirty set
├─ chunker    tree-sitter (JS/TS + Python) → symbol chunks; line-window fallback
├─ embedder   client, two modes: `multi` (ColBERT → per-token vectors) | `single` (OpenAI-compatible → one vector)
├─ index      Antelope SegmentIndex addon: `multi` → .mvec/.mvpq (MaxSim) | `single` → .vec/HNSW (+.pq); + lexical (BM25); RRF fuse
└─ surface    stdio MCP server "codesearch" (search / index tools) + `spoor` CLI

embedding backend (out of process) — one of:
├─ `multi`:  FastAPI + PyLate serving GTE-ModernColBERT-v1 (work2 / RTX 3060); /embed_multivector → {tokens, dim}
└─ `single`: any OpenAI-compatible /embeddings endpoint (OpenAI, Ollama, Cohere, …); one vector per input
```

Each module has one purpose, a narrow interface, and is testable in isolation:

- **ingester** — `scan(root) → [{path, hash, mtime}]` and `diff(prev, cur) → {added, changed, deleted}`. Honors `.gitignore` (and `.spoorignore`). No knowledge of chunking or embedding.
- **chunker** — `chunk(path, bytes) → [{path, span:[start,end], symbol?, kind?, text}]`. Tries tree-sitter for known extensions; falls back to line-windows otherwise. No I/O beyond the bytes handed in.
- **embedder** — `embed(texts, role) → EmbedResult[]` where a result is either `{tokens: float[][], dim}` (`multi`) or `{vector: float[], dim}` (`single`), per the configured mode. Talks either the multi-vector wire protocol or a standard OpenAI-compatible `/embeddings` call to `EMBED_URL` (§5). Stateless client.
- **index** — wraps the addon: `upsert(chunk, embedding)`, `remove(path)`, `search(query, opts)`. Branches on representation (`multi` → multivector pool; `single` → dense vector). Owns the Antelope index dir, the fusion, and the token budget. The `search` result shape is representation-independent.
- **surface** — MCP tool registration + CLI arg parsing. Thin; delegates to the modules above.

---

## 3. Data flow

The flow is identical across embedding modes; only the vector representation written/queried differs (`multi` = per-token pool, `single` = one dense vector).

**Index (`spoor index`, or auto-triggered — see §7):**
1. ingester scans `root`, diffs against stored manifest → dirty files.
2. For each dirty file: chunker → chunks; for deleted files: index.remove(path).
3. embedder vectorizes each chunk's `text` → per-token vectors (`multi`) or one dense vector (`single`).
4. index.upsert: write lexical tokens + the chunk's vector representation, keyed by canonical ref `path:start-end`.
5. Persist manifest (path → hash, mtime) and index metadata (`embed_mode`, `embed_model`, `dim`).

**Search (`search` MCP tool / `spoor query`):**
1. If index is stale (manifest vs. current tree), run a cheap incremental reindex first (still report `index_stale`).
2. embedder embeds the query in the active mode.
3. index runs lexical retrieval (BM25/DFR) and vector retrieval (`multi`: `search_multivector` token-HNSW → candidates → exact MaxSim; `single`: dense ANN/HNSW).
4. RRF-fuse the two ranked lists → single ranking.
5. Rank → fill the response up to `max_tokens` → set `truncated`. Build each result's snippet as the best-scoring window.

---

## 4. The `search` contract

Co-designed with `work3-agent#5`. This is the token-paid interface agents actually call.

```
search(query: string,
       k: number = 8,
       paths?: string[],
       mode: "hybrid" | "lexical" | "vector" = "hybrid",
       max_tokens: number = 1500)
  -> {
    results: [{
      path: string,                 // repo-relative
      span: [start_line, end_line], // FULL re-openable symbol/chunk range
      symbol?: string,              // populated for code (tree-sitter); absent for prose/unknown
      kind?: string,                // e.g. "function" | "method" | "class"
      score: number,                // normalized fused rank in [0,1]; comparable WITHIN one response only
      snippet: string               // best-scoring window (a few lines centered on the hit), NOT the whole chunk
    }],
    truncated: boolean,             // true if k or max_tokens clipped results
    token_estimate: number,         // total response token estimate, so the harness can budget
    index_stale: boolean            // worktree changed since last index (even after auto-reindex, report honestly)
  }
```

**Baked-in rules:**
1. **Hard cap on the total response.** Rank, fill the budget, then set `truncated`. Recall past the budget is negative value in a harness.
2. **`path` + `span` is the sole "where" contract** and MUST re-open verbatim (`span` = full symbol range; `snippet` = focused evidence within it).
3. **`score` semantics are declared:** higher = better, normalized [0,1], comparable only within a single response (RRF-fused rank, not a raw cosine).
4. **`symbol` is chunker-dependent:** always present for code (tree-sitter symbol/signature), absent for prose or fallback-chunked content.
5. **Canonical reference format is `path:start-end`** — byte-for-byte identical to what an agent pastes into a Parley room, so search output and cross-agent messages share one coordinate system with zero glue.

`index` maintenance tool:
```
index(paths?: string[], incremental: boolean = true)
  -> { indexed_files: number, chunks: number, elapsed_ms: number, dirty: boolean }
```

---

## 5. Embedder seam — two representations, one contract

spoor supports two embedding representations, chosen by config (`EMBED_MODE = multi | single`, with a capability probe as a hint). Both produce the identical `search` contract (§4); they differ only in retrieval quality and in what backend the user must run.

**`multi` — ColBERT multi-vector (premium / default when available).** Late-interaction (MaxSim) is chosen over single dense because code chunks are heterogeneous — a query targeting a few lines inside a large function scores strongly under MaxSim instead of being averaged away by the rest of the chunk. It is also Antelope's headline differentiator vs `zg`: the token-PQ epic (OPQ + global codebook + variable-k + single-resident) exists precisely to make multi-vector affordable in RAM. Because standard `/embeddings` endpoints return **one** vector per input, spoor defines its own small multi-vector wire protocol and ships a reference server:
- **Wire protocol (spoor-defined):** `POST {EMBED_URL}/embed_multivector` with `{ "inputs": [text, ...], "role": "doc"|"query" }` → `{ "vectors": [ [[f,...], ...], ... ], "dim": D, "model": "..." }` (one list of per-token vectors per input). `role` lets the model apply ColBERT's asymmetric doc/query prefixing.
- **Reference server:** FastAPI + **PyLate** serving **`GTE-ModernColBERT-v1`** (LightOn; ModernBERT backbone, 8192-token context suits long code chunks; runs comfortably on work2's RTX 3060 with headroom). Alternative: `jina-colbert-v2`. Exact VRAM/dim confirmed by a short benchmark on work2 during planning.

**`single` — standard vector (universal fallback).** For users who cannot run the ColBERT service, spoor points at **any OpenAI-compatible `/embeddings` API** (OpenAI, Ollama, Cohere, a hosted provider, …) and indexes one dense vector per chunk into Antelope's dense path (`.vec`/HNSW, optional dense `.pq`). Lower ceiling than `multi`, but works anywhere with zero bespoke serving.
- **Wire protocol (standard):** `POST {EMBED_URL}/v1/embeddings` (or the provider's equivalent) with `{ "input": [text, ...], "model": "..." }` → `{ "data": [{ "embedding": [f,...] }, ...] }`.
- **Snippet caveat:** `multi` has per-token MaxSim match locations, so snippets center precisely on the hit; `single` has no token-level locations, so its snippet centering is coarser (lexical-match line, else chunk head). The `span` (re-open target) is unaffected.

**Common config:** `EMBED_URL`, `EMBED_MODEL`, `EMBED_MODE`. The active `mode` + `model` + `dim` are written into index metadata; a change to any of them **auto-invalidates** the index (forces a rebuild), so vectors are never mixed across models or representations. The endpoint is injectable — swappable without code change; only the two wire protocols above are fixed. (A future dashboard sub-project manages this config.)

---

## 6. Index integration (Antelope addon)

- Each chunk is one indexed unit: lexical text (for BM25/DFR) + a vector representation keyed by `path:start-end`. The vector representation depends on mode:
  - **`multi`:** a multi-vector token pool (`.mvec`); retrieval via `search_multivector` (token-HNSW over the pool → candidate docids → exact MaxSim). **PQ compression (`.mvpq`) is a config flag** — off for first-loop validation, then enabled; this is the RAM differentiator, already built/tested in the engine (a setter + build call on the addon, not new engine work).
  - **`single`:** one dense vector (`.vec`); retrieval via dense ANN/HNSW, with dense **`.pq`** compression available as the analogous flag.
- Both modes fuse their vector leg with lexical retrieval via **RRF**; the fused ranking and the `search` result shape are identical across modes.
- Incremental: `upsert` replaces a file's chunks; `remove` drops them. Compaction is the addon's existing responsibility.
- **Addon-surface check (planning gate):** confirm the Node addon exposes, for `multi`: multi-vector add + `search_multivector` (or `search_rerank`) + `.mvpq` setters; and for `single`: dense-vector add + dense ANN search + `.pq` setters. Project memory says both are present (PQ config bags + async `buildPq`/`buildMultivectorPq` shipped). Any missing call is a separate flagged engine task, surfaced before implementation — not silently worked around.

---

## 7. Index freshness

- Manifest = `path → {hash, mtime}`, persisted next to the index. Dirty = mtime changed AND content-hash changed (mtime is the cheap filter, hash is the authority).
- **`.gitignore`-aware** (plus optional `.spoorignore`); never index ignored paths.
- **`search` auto-triggers a cheap incremental reindex when stale** so a tyode worker landing on a fresh worktree "just works" with no explicit `index` call — but the response still reports `index_stale` truthfully so a caller can force a full `index`.
- Mode/model/dim change in metadata → full invalidation + rebuild (never mix representations in one index).

---

## 8. Surface: MCP server + CLI

- **stdio MCP server**, stable name **`codesearch`**, reads `EMBED_URL` / `EMBED_MODEL` / index dir from env/config so tyode's one-line merge-into-`.mcp.json` injects it fleet-wide and each box points at its local ColBERT service without touching the tool. Tools: `search` (§4), `index` (§4). (Optional read-only `index_info` mirroring the existing Python server.)
- **CLI** (`spoor`): `spoor index [paths...]`, `spoor query "<text>" [--k --mode --max-tokens]` (human-readable output), `spoor install --target <agent>` (merge the MCP server into a coding agent's config, `zg`-style). CLI and MCP share the same underlying modules.

---

## 9. Error handling

- **Embedder unreachable:** `index`/`search` fail fast with a clear message naming `EMBED_URL`; no partial/garbage vectors written. A failed embed of one chunk drops that chunk from the batch and logs it (never a null vector).
- **Grammar missing for an extension:** fall back to line-window chunking (`symbol: null`); never fail the file.
- **Corrupt/oversized file:** skip with a logged reason; indexing of the repo continues.
- **Stale index during search:** auto-reindex best-effort; if reindex fails, serve the stale index and set `index_stale: true` rather than erroring.
- **Model/dim mismatch on open:** treat as invalidation → rebuild, not a crash.

---

## 10. Testing

- **ingester:** fixture tree with `.gitignore` → correct dirty set; touch-without-change (mtime up, hash same) → not dirty; delete → removal.
- **chunker:** JS/TS + Python fixtures → expected `symbol`/`kind`/`span`; unknown extension → line-window fallback with `symbol: null`; span re-opens to the exact bytes.
- **embedder client:** `multi` against a stub multi-vector server → correct request shape (incl. `role`), parses per-token vectors + dim; `single` against a stub OpenAI-compatible `/embeddings` server → parses one vector + dim; unreachable → fail-fast in both modes.
- **index/search (both modes):** total response ≤ `max_tokens` (budget cap enforced); `truncated` set when clipped; RRF ordering sane; `mode` (search arg) toggles lexical/vector/hybrid; `path:start-end` re-openable. The `search` response schema is byte-shape-identical across `multi` and `single` embedding representations.
- **freshness:** search on a changed worktree auto-reindexes and reports `index_stale`; mode/model/dim change invalidates.
- **end-to-end:** index a small fixture repo, `search` via MCP returns correct token-budgeted, re-openable hits — run once per representation (`multi` and `single`).
- **PQ flag:** with `.mvpq` (multi) / `.pq` (single) enabled, search results remain correct (recall within tolerance of float baseline) and RAM is lower.

---

## 11. Success criterion

`npx spoor index` then an MCP `search` from a coding agent (Claude Code / Codex) returns correct, token-budgeted, re-openable hits on a real repo — installable into the tyode fleet for `work3-agent#5` to dogfood. Validated in **both** embedding modes: `single` (pointed at a standard `/embeddings` API — the works-anywhere baseline) and `multi` (ColBERT service, with the `.mvpq` PQ flag demonstrating the low-RAM differentiator).

---

## 12. Repo / process constraints

- No Antelope C++ engine change; spoor consumes the Node addon. A missing addon surface is a flagged separate task, not worked around.
- Keep modules small and single-purpose (§2); each independently testable.
- The multi-vector wire protocol (§5) is spoor's own contract; document it so alternative embedder servers can conform.
- Coordinate the `search` response schema (§4) with `work3-agent#5` before locking — it is a shared contract with tyode/Parley.

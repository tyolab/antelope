# spoor

Agent-facing local code search over the [Antelope](../) hybrid search engine — a stdio MCP
server + CLI that indexes a repository symbol-aware, embeds chunks, and returns **token-budgeted,
source-linked** results (`path:start-end`). The Antelope analogue of Alibaba's `zg` (zvec-grep),
differentiated by native/embedded use and multi-vector (ColBERT/MaxSim) + PQ compression.

## Install

```bash
cd spoor
npm install
./scripts/fetch-grammars.sh        # fetches tree-sitter runtime + js/ts/python grammar wasm (gitignored)
```

Requires the built Antelope Node addon (`nodejs/build/Release/antelope_segment.node`). If it's not
built, run `nodejs/build.sh`. Override its location with `ANTELOPE_ADDON=/abs/path/antelope_segment.node`.

## Configuration (env)

| Var | Meaning | Default |
|---|---|---|
| `SPOOR_INDEX_DIR` | where the index + sidecars live | (required) |
| `EMBED_URL` | embedding endpoint base URL | (required) |
| `EMBED_MODE` | `single` or `multi` | `single` |
| `EMBED_MODEL` | model name passed to the endpoint | mode default |
| `ANTELOPE_ADDON` | override path to `antelope_segment.node` | auto-resolved |

**Two embedding representations, one identical search contract:**
- **`single`** — any OpenAI-compatible `/embeddings` API (OpenAI, Ollama, Cohere…). Dense `.vec`/HNSW. Works anywhere.
- **`multi`** — the ColBERT service in `spoor-embed/` (`/embed_multivector`). Multi-vector `.mvec`/`.mvpq` MaxSim, with a mean-pooled dense first-stage. Higher quality; the differentiator.

Representation is recorded in index metadata (`mode`/`model`/`dim`); changing any of them
invalidates and rebuilds the index.

## CLI

```bash
spoor index                       # (re)index the cwd repo, incremental
spoor query "how does login work" # search; prints JSON
spoor query "retry" --k 5 --mode hybrid --max-tokens 1500
spoor serve                       # run the stdio MCP server ("codesearch")
spoor install --config .mcp.json  # merge the codesearch MCP server into an agent config
spoor index --pq                  # build with PQ compression (residentTier NONE = RAM win)
```

## MCP tools (server name `codesearch`)

- `search(query, k=8, mode="hybrid"|"lexical"|"vector", max_tokens=1500)` →
  `{ results:[{ path, span:[start,end], symbol?, kind?, score, snippet }], truncated, token_estimate, index_stale }`
  - `path`+`span` is the sole re-openable ref (1-based inclusive lines). `score` is a normalized
    fused rank in [0,1], comparable only within one response. `snippet` centers on the best-matched
    line. The total response is hard-capped at `max_tokens`.
- `index(incremental=true)` → `{ indexed_files, chunks, elapsed_ms, dirty }`

`search` auto-triggers a cheap incremental reindex when the worktree is stale, and reports
`index_stale` honestly.

## Flagged follow-ons (not in the MVP)

- Advanced token-codec knobs (OPQ / global codebook / variable-k) and V6 `searchMultivector` are
  engine-only today; exposing them to the Node addon is a separate sub-project → full recall/footprint parity.
- Per-token MaxSim snippet centering (needs addon to expose hit offsets) → optional `highlights`.
- Management dashboard for embedder config; grammars beyond JS/TS/Python; remote multi-vector tier.

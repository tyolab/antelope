# spoor Node Package Implementation Plan (Plan A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `spoor` Node.js package — an agent-facing local code-search tool (stdio MCP server + CLI) that ingests a repo, chunks it symbol-aware, embeds chunks (two modes), indexes into the Antelope `SegmentIndex` addon, and returns token-budgeted, source-linked hits.

**Architecture:** Five focused modules (ingester → chunker → embedder → index → surface) plus small helpers (manifest, fuse, snippet, config). Two embedding representations behind one identical `search` contract: `single` (standard OpenAI-compatible `/embeddings` → dense `.vec`/HNSW) and `multi` (spoor multi-vector protocol → `.mvec`/`.mvpq` MaxSim via `searchRerank`, with a mean-pooled dense first-stage vector). Pure modules are unit-tested with no native addon; the index layer and end-to-end are integration-tested against the built addon and stub embedding servers.

**Tech Stack:** Node.js ≥18 (native `fetch`, `node:test`, `node:crypto`), `web-tree-sitter` (+ JS/TS/Python wasm grammars), `ignore` (gitignore matching), `@modelcontextprotocol/sdk` (stdio MCP), and the existing Antelope `SegmentIndex` Node-API addon (`nodejs/addon/segment_index.cpp`).

**Reference — real addon surface (confirmed in `nodejs/addon/segment_index.cpp`):**
- `new SegmentIndex(options)` where options may include `dimension`, `metric`, `hnsw`, `rerank:{dimension,quantize}`, `pq:{m,posture,rerankQuant,residentTier}`, `multivectorPq:{m,posture,rerankQuant,residentTier}`, `attributes`.
- `open(dir)`, `close()`, `addDocument(key,text,vector?,multiVectors?,options?)→{generation,docid}`, `updateDocument(...)`, `deleteDocument(key)→bool`.
- `search(text,k,{filter?})→Hit[]`, `searchVectorHnsw(vector,k,opts)`, `searchHybridHnsw(text,vector,k,opts)`, `searchRerank(queryMultiVectors, {text?,vector?,firstStageN?,topK?,filter?})→Hit[]`.
- `buildHnsw()`, `buildPq()`, `buildMultivectorPq()`, `flush()`, `maintain()`, `documentCount()`, `vectorDimension()`.
- `Hit = {generation, docid, key, score, payload?}`.

**Out of scope (flagged follow-ons):** advanced token-codec knobs (OPQ/global-codebook/variable-k) + `searchMultivector` Node bindings; management dashboard; grammars beyond JS/TS/Python; remote multi-vector tier.

---

## File Structure

```
spoor/
├── package.json                # deps, "type":"module", test script
├── src/
│   ├── config.js               # env → {embedUrl, embedModel, embedMode, indexDir, addonPath}
│   ├── addon.js                # resolves the SegmentIndex class from the antelope addon
│   ├── manifest.js             # load/save manifest {files, meta}; dirty detection
│   ├── ingester.js             # walk repo (gitignore-aware), hash+mtime, diff
│   ├── chunker.js              # dispatch: tree-sitter (js/ts/py) | line-window fallback
│   ├── chunker_treesitter.js   # web-tree-sitter symbol extraction
│   ├── chunker_lines.js        # line-window fallback
│   ├── embedder.js             # two-mode client (+ mean-pool helper)
│   ├── fuse.js                 # RRF fusion
│   ├── snippet.js              # best-scoring window
│   ├── index.js                # SpoorIndex: open/upsert/remove/search over the addon
│   ├── reindex.js              # orchestrates ingest→chunk→embed→index; staleness
│   ├── mcp.js                  # stdio MCP server "codesearch"
│   └── cli.js                  # `spoor index|query|install`
├── grammars/                   # tree-sitter-{javascript,typescript,python}.wasm + tree-sitter.wasm
└── test/
    ├── fixtures/               # sample repo + per-language source files
    ├── stub_embed_single.js    # stub OpenAI /embeddings server
    ├── stub_embed_multi.js     # stub /embed_multivector server
    ├── *.test.js
```

---

## Task 1: Scaffold the package

**Files:**
- Create: `spoor/package.json`
- Create: `spoor/src/config.js`
- Test: `spoor/test/config.test.js`

- [ ] **Step 1: Write `spoor/package.json`**

```json
{
  "name": "spoor",
  "version": "0.0.1",
  "type": "module",
  "bin": { "spoor": "src/cli.js" },
  "scripts": { "test": "node --test" },
  "dependencies": {
    "@modelcontextprotocol/sdk": "^1.0.0",
    "ignore": "^5.3.0",
    "web-tree-sitter": "^0.22.0"
  }
}
```

- [ ] **Step 2: Write the failing test** `spoor/test/config.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadConfig } from '../src/config.js';

test('loadConfig reads env with defaults', () => {
  const c = loadConfig({
    SPOOR_INDEX_DIR: '/tmp/ix',
    EMBED_URL: 'http://work2:8900',
    EMBED_MODEL: 'gte-moderncolbert',
    EMBED_MODE: 'multi',
  });
  assert.equal(c.indexDir, '/tmp/ix');
  assert.equal(c.embedUrl, 'http://work2:8900');
  assert.equal(c.embedMode, 'multi');
});

test('loadConfig defaults embedMode to single', () => {
  const c = loadConfig({ SPOOR_INDEX_DIR: '/tmp/ix', EMBED_URL: 'http://x' });
  assert.equal(c.embedMode, 'single');
});

test('loadConfig throws when required env missing', () => {
  assert.throws(() => loadConfig({}), /SPOOR_INDEX_DIR/);
});
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd spoor && node --test test/config.test.js`
Expected: FAIL (`Cannot find module '../src/config.js'`).

- [ ] **Step 4: Write `spoor/src/config.js`**

```javascript
export function loadConfig(env = process.env) {
  const indexDir = env.SPOOR_INDEX_DIR;
  if (!indexDir) throw new Error('SPOOR_INDEX_DIR is required');
  const embedUrl = env.EMBED_URL;
  if (!embedUrl) throw new Error('EMBED_URL is required');
  const embedMode = env.EMBED_MODE === 'multi' ? 'multi' : 'single';
  return {
    indexDir,
    embedUrl: embedUrl.replace(/\/+$/, ''),
    embedModel: env.EMBED_MODEL || (embedMode === 'multi' ? 'gte-moderncolbert' : 'nomic-embed-text'),
    embedMode,
    addonPath: env.ANTELOPE_ADDON || null,
  };
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd spoor && node --test test/config.test.js`
Expected: PASS (3 tests).

- [ ] **Step 6: Commit**

```bash
git add spoor/package.json spoor/src/config.js spoor/test/config.test.js
git commit -m "feat(spoor): scaffold package + config module"
```

---

## Task 2: Manifest (state + dirty detection)

**Files:**
- Create: `spoor/src/manifest.js`
- Test: `spoor/test/manifest.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/manifest.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { emptyManifest, isCompatible, diffFiles } from '../src/manifest.js';

test('emptyManifest has files map and null meta', () => {
  const m = emptyManifest();
  assert.deepEqual(m.files, {});
  assert.equal(m.meta, null);
});

test('isCompatible false when mode/model/dim differ', () => {
  const meta = { mode: 'single', model: 'a', dim: 768 };
  assert.equal(isCompatible(meta, { mode: 'single', model: 'a', dim: 768 }), true);
  assert.equal(isCompatible(meta, { mode: 'multi', model: 'a', dim: 768 }), false);
  assert.equal(isCompatible(meta, { mode: 'single', model: 'b', dim: 768 }), false);
  assert.equal(isCompatible(null, { mode: 'single', model: 'a', dim: 768 }), false);
});

test('diffFiles classifies added/changed/deleted', () => {
  const prev = { 'a.js': { hash: 'h1', mtime: 1 }, 'b.js': { hash: 'h2', mtime: 1 } };
  const cur = { 'a.js': { hash: 'h1', mtime: 1 }, 'b.js': { hash: 'h2b', mtime: 2 }, 'c.js': { hash: 'h3', mtime: 1 } };
  const d = diffFiles(prev, cur);
  assert.deepEqual(d.added, ['c.js']);
  assert.deepEqual(d.changed, ['b.js']);
  assert.deepEqual(d.deleted, []);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/manifest.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/manifest.js`**

```javascript
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { dirname, join } from 'node:path';

export function emptyManifest() { return { files: {}, meta: null }; }

export function isCompatible(meta, want) {
  return !!meta && meta.mode === want.mode && meta.model === want.model && meta.dim === want.dim;
}

export function diffFiles(prev, cur) {
  const added = [], changed = [], deleted = [];
  for (const p of Object.keys(cur)) {
    if (!(p in prev)) added.push(p);
    else if (prev[p].hash !== cur[p].hash) changed.push(p);
  }
  for (const p of Object.keys(prev)) if (!(p in cur)) deleted.push(p);
  return { added, changed, deleted };
}

export async function loadManifest(indexDir) {
  try {
    const raw = await readFile(join(indexDir, 'spoor-manifest.json'), 'utf8');
    return JSON.parse(raw);
  } catch { return emptyManifest(); }
}

export async function saveManifest(indexDir, manifest) {
  const path = join(indexDir, 'spoor-manifest.json');
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, JSON.stringify(manifest));
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/manifest.test.js`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add spoor/src/manifest.js spoor/test/manifest.test.js
git commit -m "feat(spoor): manifest state + dirty/compat detection"
```

---

## Task 3: Ingester (gitignore-aware walk + hash)

**Files:**
- Create: `spoor/src/ingester.js`
- Test: `spoor/test/ingester.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/ingester.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { scanRepo } from '../src/ingester.js';

async function fixture() {
  const root = await mkdtemp(join(tmpdir(), 'spoor-ing-'));
  await writeFile(join(root, '.gitignore'), 'node_modules/\n*.log\n');
  await writeFile(join(root, 'a.js'), 'const a = 1;\n');
  await mkdir(join(root, 'node_modules', 'x'), { recursive: true });
  await writeFile(join(root, 'node_modules', 'x', 'y.js'), 'ignored\n');
  await writeFile(join(root, 'debug.log'), 'ignored\n');
  await mkdir(join(root, 'sub'), { recursive: true });
  await writeFile(join(root, 'sub', 'b.py'), 'x = 1\n');
  return root;
}

test('scanRepo honors gitignore and returns hash+mtime', async () => {
  const root = await fixture();
  const files = await scanRepo(root);
  const paths = Object.keys(files).sort();
  assert.deepEqual(paths, ['a.js', 'sub/b.py']);
  assert.match(files['a.js'].hash, /^[0-9a-f]{16,}$/);
  assert.equal(typeof files['a.js'].mtime, 'number');
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/ingester.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/ingester.js`**

```javascript
import { readdir, readFile, stat } from 'node:fs/promises';
import { join, relative, sep } from 'node:path';
import { createHash } from 'node:crypto';
import ignore from 'ignore';

async function readIgnore(root) {
  const ig = ignore();
  ig.add(['.git/', '.spoor/']);
  for (const f of ['.gitignore', '.spoorignore']) {
    try { ig.add(await readFile(join(root, f), 'utf8')); } catch { /* absent */ }
  }
  return ig;
}

export async function scanRepo(root) {
  const ig = await readIgnore(root);
  const out = {};
  async function walk(dir) {
    const entries = await readdir(dir, { withFileTypes: true });
    for (const e of entries) {
      const abs = join(dir, e.name);
      let rel = relative(root, abs);
      if (sep !== '/') rel = rel.split(sep).join('/');
      const test = e.isDirectory() ? rel + '/' : rel;
      if (ig.ignores(test)) continue;
      if (e.isDirectory()) await walk(abs);
      else if (e.isFile()) {
        const buf = await readFile(abs);
        const st = await stat(abs);
        out[rel] = { hash: createHash('sha1').update(buf).digest('hex'), mtime: Math.floor(st.mtimeMs) };
      }
    }
  }
  await walk(root);
  return out;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/ingester.test.js`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add spoor/src/ingester.js spoor/test/ingester.test.js
git commit -m "feat(spoor): gitignore-aware repo ingester with content hashing"
```

---

## Task 4: Line-window fallback chunker

**Files:**
- Create: `spoor/src/chunker_lines.js`
- Test: `spoor/test/chunker_lines.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/chunker_lines.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chunkLines } from '../src/chunker_lines.js';

test('chunkLines splits into windows with 1-based inclusive spans', () => {
  const src = Array.from({ length: 50 }, (_, i) => `line${i + 1}`).join('\n');
  const chunks = chunkLines('f.txt', src, { window: 20, overlap: 5 });
  assert.equal(chunks[0].path, 'f.txt');
  assert.deepEqual(chunks[0].span, [1, 20]);
  assert.equal(chunks[0].symbol, null);
  // next window starts at 20 - 5 + 1 = 16
  assert.deepEqual(chunks[1].span, [16, 35]);
  // text of chunk 0 is lines 1..20 rejoined
  assert.equal(chunks[0].text.split('\n').length, 20);
});

test('chunkLines handles a file shorter than one window', () => {
  const chunks = chunkLines('s.txt', 'a\nb\nc', { window: 20, overlap: 5 });
  assert.equal(chunks.length, 1);
  assert.deepEqual(chunks[0].span, [1, 3]);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/chunker_lines.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/chunker_lines.js`**

```javascript
export function chunkLines(path, source, { window = 40, overlap = 8 } = {}) {
  const lines = source.split('\n');
  const n = lines.length;
  const step = Math.max(1, window - overlap);
  const chunks = [];
  for (let start = 0; start < n; start += step) {
    const end = Math.min(n, start + window);
    chunks.push({
      path,
      span: [start + 1, end],           // 1-based inclusive
      symbol: null,
      kind: null,
      text: lines.slice(start, end).join('\n'),
    });
    if (end === n) break;
  }
  return chunks;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/chunker_lines.test.js`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add spoor/src/chunker_lines.js spoor/test/chunker_lines.test.js
git commit -m "feat(spoor): line-window fallback chunker"
```

---

## Task 5: Tree-sitter symbol chunker (JS/TS + Python)

**Files:**
- Create: `spoor/src/chunker_treesitter.js`
- Create: `spoor/grammars/` (download `tree-sitter.wasm` + `tree-sitter-{javascript,typescript,python}.wasm`)
- Test: `spoor/test/chunker_treesitter.test.js`

- [ ] **Step 1: Fetch grammar wasm files**

Run:
```bash
cd spoor && mkdir -p grammars && cd grammars
npm pack web-tree-sitter@0.22.0 >/dev/null 2>&1 || true
# tree-sitter.wasm ships inside web-tree-sitter; copy it:
cp ../node_modules/web-tree-sitter/tree-sitter.wasm .
# grammar wasms (prebuilt) from the tree-sitter-wasms package:
npm pack tree-sitter-wasms@0.1.11
tar -xzf tree-sitter-wasms-*.tgz
cp package/out/tree-sitter-javascript.wasm .
cp package/out/tree-sitter-typescript.wasm .
cp package/out/tree-sitter-python.wasm .
rm -rf package tree-sitter-wasms-*.tgz
ls *.wasm
```
Expected: lists `tree-sitter.wasm`, `tree-sitter-javascript.wasm`, `tree-sitter-typescript.wasm`, `tree-sitter-python.wasm`.

- [ ] **Step 2: Write the failing test** `spoor/test/chunker_treesitter.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chunkTreeSitter } from '../src/chunker_treesitter.js';

test('extracts top-level function symbols from JS', async () => {
  const src = [
    'function alpha(a, b) {',
    '  return a + b;',
    '}',
    '',
    'const beta = () => 42;',
  ].join('\n');
  const chunks = await chunkTreeSitter('m.js', src);
  const alpha = chunks.find(c => c.symbol === 'alpha');
  assert.ok(alpha, 'alpha symbol present');
  assert.equal(alpha.kind, 'function');
  assert.deepEqual(alpha.span, [1, 3]);
  assert.match(alpha.text, /return a \+ b/);
});

test('extracts def/class symbols from Python', async () => {
  const src = 'def foo(x):\n    return x\n\nclass Bar:\n    def m(self):\n        return 1\n';
  const chunks = await chunkTreeSitter('m.py', src);
  assert.ok(chunks.find(c => c.symbol === 'foo' && c.kind === 'function'));
  assert.ok(chunks.find(c => c.symbol === 'Bar' && c.kind === 'class'));
});

test('returns null for an unsupported extension', async () => {
  assert.equal(await chunkTreeSitter('x.rb', 'puts 1'), null);
});
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd spoor && node --test test/chunker_treesitter.test.js`
Expected: FAIL (module not found).

- [ ] **Step 4: Write `spoor/src/chunker_treesitter.js`**

```javascript
import Parser from 'web-tree-sitter';
import { fileURLToPath } from 'node:url';
import { dirname, join, extname } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const GRAMMARS = join(HERE, '..', 'grammars');

const LANG_BY_EXT = {
  '.js': 'javascript', '.jsx': 'javascript', '.mjs': 'javascript', '.cjs': 'javascript',
  '.ts': 'typescript', '.tsx': 'typescript',
  '.py': 'python',
};

// node types that name a top-level code unit, mapped to a "kind" + the field holding the name.
const SYMBOL_TYPES = {
  function_declaration: 'function',
  function_definition: 'function',   // python
  method_definition: 'method',
  class_declaration: 'class',
  class_definition: 'class',         // python
  lexical_declaration: 'function',   // const x = () => ... (arrow); resolved below
};

let _init;
const _langCache = new Map();

async function getLanguage(lang) {
  if (!_init) _init = Parser.init({ locateFile: () => join(GRAMMARS, 'tree-sitter.wasm') });
  await _init;
  if (_langCache.has(lang)) return _langCache.get(lang);
  const L = await Parser.Language.load(join(GRAMMARS, `tree-sitter-${lang}.wasm`));
  _langCache.set(lang, L);
  return L;
}

function nameOf(node) {
  const id = node.childForFieldName('name');
  if (id) return id.text;
  // arrow assigned to a const: lexical_declaration > variable_declarator(name, value=arrow_function)
  const decl = node.namedChildren.find(c => c.type === 'variable_declarator');
  if (decl) {
    const nm = decl.childForFieldName('name');
    const val = decl.childForFieldName('value');
    if (nm && val && (val.type === 'arrow_function' || val.type === 'function')) return nm.text;
  }
  return null;
}

export async function chunkTreeSitter(path, source) {
  const lang = LANG_BY_EXT[extname(path).toLowerCase()];
  if (!lang) return null;
  const L = await getLanguage(lang);
  const parser = new Parser();
  parser.setLanguage(L);
  const tree = parser.parse(source);
  const chunks = [];
  const root = tree.rootNode;
  const visit = (node) => {
    for (const child of node.namedChildren) {
      const kind = SYMBOL_TYPES[child.type];
      const name = kind ? nameOf(child) : null;
      if (kind && name) {
        chunks.push({
          path,
          span: [child.startPosition.row + 1, child.endPosition.row + 1],
          symbol: name,
          kind,
          text: source.slice(child.startIndex, child.endIndex),
        });
        // recurse into classes to also capture methods
        if (kind === 'class') visit(child);
      }
    }
  };
  visit(root);
  return chunks;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd spoor && node --test test/chunker_treesitter.test.js`
Expected: PASS (3 tests). If a grammar node-type name differs across grammar versions and a test fails, print `tree.rootNode.toString()` in a scratch script to confirm the type names, then adjust `SYMBOL_TYPES`/`nameOf` — do not weaken the assertions.

- [ ] **Step 6: Commit**

```bash
git add spoor/src/chunker_treesitter.js spoor/grammars spoor/test/chunker_treesitter.test.js
git commit -m "feat(spoor): tree-sitter symbol chunker (js/ts/python)"
```

---

## Task 6: Chunker dispatch

**Files:**
- Create: `spoor/src/chunker.js`
- Test: `spoor/test/chunker.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/chunker.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chunkFile } from '../src/chunker.js';

test('chunkFile uses tree-sitter for .py (real symbol)', async () => {
  const chunks = await chunkFile('m.py', 'def foo(x):\n    return x\n');
  assert.equal(chunks[0].symbol, 'foo');
});

test('chunkFile falls back to line-windows for unknown ext (symbol null)', async () => {
  const src = Array.from({ length: 30 }, (_, i) => `l${i}`).join('\n');
  const chunks = await chunkFile('notes.md', src);
  assert.ok(chunks.length >= 1);
  assert.equal(chunks[0].symbol, null);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/chunker.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/chunker.js`**

```javascript
import { chunkTreeSitter } from './chunker_treesitter.js';
import { chunkLines } from './chunker_lines.js';

export async function chunkFile(path, source) {
  try {
    const ts = await chunkTreeSitter(path, source);
    if (ts && ts.length) return ts;
  } catch { /* fall through to line-windows */ }
  return chunkLines(path, source);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/chunker.test.js`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add spoor/src/chunker.js spoor/test/chunker.test.js
git commit -m "feat(spoor): chunker dispatch (tree-sitter primary, line-window fallback)"
```

---

## Task 7: Embedder client (two modes + mean-pool)

**Files:**
- Create: `spoor/src/embedder.js`
- Create: `spoor/test/stub_embed_single.js`, `spoor/test/stub_embed_multi.js`
- Test: `spoor/test/embedder.test.js`

- [ ] **Step 1: Write stub servers**

`spoor/test/stub_embed_single.js`:
```javascript
import { createServer } from 'node:http';
// Returns a deterministic 4-dim vector per input (length-based) for OpenAI /embeddings.
export function startSingleStub() {
  const server = createServer((req, res) => {
    let body = '';
    req.on('data', c => (body += c));
    req.on('end', () => {
      const { input } = JSON.parse(body);
      const data = input.map((t, i) => ({ embedding: [t.length, i, 1, 0] }));
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ data, model: 'stub', dim: 4 }));
    });
  });
  return new Promise(r => server.listen(0, () => r({ server, url: `http://127.0.0.1:${server.address().port}` })));
}
```

`spoor/test/stub_embed_multi.js`:
```javascript
import { createServer } from 'node:http';
// Returns 2 token-vectors (dim 4) per input for /embed_multivector.
export function startMultiStub() {
  const server = createServer((req, res) => {
    let body = '';
    req.on('data', c => (body += c));
    req.on('end', () => {
      const { inputs } = JSON.parse(body);
      const vectors = inputs.map((t) => [[t.length, 0, 1, 0], [0, t.length, 0, 1]]);
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ vectors, dim: 4, model: 'stub-mv' }));
    });
  });
  return new Promise(r => server.listen(0, () => r({ server, url: `http://127.0.0.1:${server.address().port}` })));
}
```

- [ ] **Step 2: Write the failing test** `spoor/test/embedder.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Embedder, meanPool } from '../src/embedder.js';
import { startSingleStub } from './stub_embed_single.js';
import { startMultiStub } from './stub_embed_multi.js';

test('single mode returns one vector per input with dim', async () => {
  const { server, url } = await startSingleStub();
  const e = new Embedder({ embedUrl: url, embedMode: 'single', embedModel: 'm' });
  const r = await e.embed(['abc', 'de'], 'doc');
  assert.equal(r.dim, 4);
  assert.deepEqual(r.items[0], { vector: [3, 0, 1, 0] });
  server.close();
});

test('multi mode returns per-token vectors and a pooled vector', async () => {
  const { server, url } = await startMultiStub();
  const e = new Embedder({ embedUrl: url, embedMode: 'multi', embedModel: 'm' });
  const r = await e.embed(['abc'], 'doc');
  assert.equal(r.dim, 4);
  assert.equal(r.items[0].tokens.length, 2);
  assert.deepEqual(r.items[0].pooled, meanPool([[3, 0, 1, 0], [0, 3, 0, 1]]));
  server.close();
});

test('meanPool averages component-wise', () => {
  assert.deepEqual(meanPool([[2, 0], [0, 4]]), [1, 2]);
});

test('embed throws a clear error when server unreachable', async () => {
  const e = new Embedder({ embedUrl: 'http://127.0.0.1:1', embedMode: 'single', embedModel: 'm' });
  await assert.rejects(() => e.embed(['x'], 'doc'), /EMBED_URL|embed/i);
});
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd spoor && node --test test/embedder.test.js`
Expected: FAIL (module not found).

- [ ] **Step 4: Write `spoor/src/embedder.js`**

```javascript
export function meanPool(tokens) {
  const dim = tokens[0].length;
  const out = new Array(dim).fill(0);
  for (const t of tokens) for (let i = 0; i < dim; i++) out[i] += t[i];
  for (let i = 0; i < dim; i++) out[i] /= tokens.length;
  return out;
}

export class Embedder {
  constructor({ embedUrl, embedMode, embedModel }) {
    this.url = embedUrl; this.mode = embedMode; this.model = embedModel;
  }

  async embed(texts, role) {
    if (this.mode === 'multi') return this._multi(texts, role);
    return this._single(texts);
  }

  async _single(texts) {
    const body = JSON.stringify({ input: texts, model: this.model });
    const json = await this._post('/v1/embeddings', body);
    const items = json.data.map(d => ({ vector: d.embedding }));
    return { dim: items[0].vector.length, items };
  }

  async _multi(texts, role) {
    const body = JSON.stringify({ inputs: texts, role, model: this.model });
    const json = await this._post('/embed_multivector', body);
    const items = json.vectors.map(tokens => ({ tokens, pooled: meanPool(tokens) }));
    return { dim: json.dim ?? items[0].tokens[0].length, items };
  }

  async _post(path, body) {
    let resp;
    try {
      resp = await fetch(this.url + path, { method: 'POST', headers: { 'content-type': 'application/json' }, body });
    } catch (err) {
      throw new Error(`embed request to EMBED_URL ${this.url}${path} failed: ${err.message}`);
    }
    if (!resp.ok) throw new Error(`embed request to ${this.url}${path} returned ${resp.status}`);
    return resp.json();
  }
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd spoor && node --test test/embedder.test.js`
Expected: PASS (4 tests).

- [ ] **Step 6: Commit**

```bash
git add spoor/src/embedder.js spoor/test/stub_embed_single.js spoor/test/stub_embed_multi.js spoor/test/embedder.test.js
git commit -m "feat(spoor): two-mode embedder client with mean-pool for multi first-stage"
```

---

## Task 8: RRF fusion + snippet

**Files:**
- Create: `spoor/src/fuse.js`, `spoor/src/snippet.js`
- Test: `spoor/test/fuse.test.js`, `spoor/test/snippet.test.js`

- [ ] **Step 1: Write the failing tests**

`spoor/test/fuse.test.js`:
```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { rrf } from '../src/fuse.js';

test('rrf fuses two ranked key lists and normalizes to [0,1]', () => {
  const lexical = ['a', 'b', 'c'];
  const vector = ['b', 'd', 'a'];
  const fused = rrf([lexical, vector], { k: 60 });
  // 'b' ranks high in both → top; scores normalized so best == 1
  assert.equal(fused[0].key, 'b');
  assert.equal(fused[0].score, 1);
  assert.ok(fused.every(f => f.score >= 0 && f.score <= 1));
  const keys = fused.map(f => f.key).sort();
  assert.deepEqual(keys, ['a', 'b', 'c', 'd']);
});
```

`spoor/test/snippet.test.js`:
```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bestWindow, firstMatchLine } from '../src/snippet.js';

test('bestWindow centers on the matched line', () => {
  const text = Array.from({ length: 20 }, (_, i) => `line ${i + 1}`).join('\n');
  const s = bestWindow(text, { hitLine: 10, radius: 2 });
  assert.equal(s, ['line 8', 'line 9', 'line 10', 'line 11', 'line 12'].join('\n'));
});

test('bestWindow falls back to the head when hitLine is null', () => {
  const text = 'a\nb\nc\nd\ne\nf';
  const s = bestWindow(text, { hitLine: null, radius: 2, headLines: 3 });
  assert.equal(s, 'a\nb\nc');
});

test('firstMatchLine returns the 1-based line of the best query-term hit', () => {
  const text = 'def helper():\n    pass\n\ndef login(user):\n    return auth(user)\n';
  // "login" first appears on line 4
  assert.equal(firstMatchLine(text, 'login user'), 4);
});

test('firstMatchLine returns null when no term matches', () => {
  assert.equal(firstMatchLine('a\nb\nc', 'zzz'), null);
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd spoor && node --test test/fuse.test.js test/snippet.test.js`
Expected: FAIL (modules not found).

- [ ] **Step 3: Write `spoor/src/fuse.js`**

```javascript
// Reciprocal Rank Fusion over N ranked lists of keys. Returns [{key, score}] sorted desc,
// score normalized so the top result is 1 (comparable within one response only).
export function rrf(lists, { k = 60 } = {}) {
  const acc = new Map();
  for (const list of lists) {
    list.forEach((key, i) => {
      acc.set(key, (acc.get(key) || 0) + 1 / (k + i + 1));
    });
  }
  const rows = [...acc.entries()].map(([key, s]) => ({ key, score: s }));
  rows.sort((a, b) => b.score - a.score);
  const max = rows.length ? rows[0].score : 1;
  for (const r of rows) r.score = max > 0 ? r.score / max : 0;
  return rows;
}
```

- [ ] **Step 4: Write `spoor/src/snippet.js`**

```javascript
// Return a small window of `text` centered on hitLine (1-based within the chunk),
// or the head if hitLine is null.
export function bestWindow(text, { hitLine, radius = 3, headLines = 6 } = {}) {
  const lines = text.split('\n');
  if (hitLine == null) return lines.slice(0, headLines).join('\n');
  const idx = hitLine - 1;
  const start = Math.max(0, idx - radius);
  const end = Math.min(lines.length, idx + radius + 1);
  return lines.slice(start, end).join('\n');
}

// 1-based line within `text` of the first query-term occurrence (case-insensitive),
// or null if no term matches. Used to center the snippet on the best-matched line
// (both modes) since the addon does not expose per-token MaxSim offsets at MVP.
export function firstMatchLine(text, query) {
  const terms = query.toLowerCase().split(/[^a-z0-9_]+/i).filter(Boolean);
  if (!terms.length) return null;
  const lines = text.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const lc = lines[i].toLowerCase();
    if (terms.some(t => lc.includes(t))) return i + 1;
  }
  return null;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd spoor && node --test test/fuse.test.js test/snippet.test.js`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add spoor/src/fuse.js spoor/src/snippet.js spoor/test/fuse.test.js spoor/test/snippet.test.js
git commit -m "feat(spoor): RRF fusion + best-window snippet helpers"
```

---

## Task 9: Addon resolver

**Files:**
- Create: `spoor/src/addon.js`
- Test: `spoor/test/addon.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/addon.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadSegmentIndex } from '../src/addon.js';

test('loadSegmentIndex returns a constructable class', () => {
  const SegmentIndex = loadSegmentIndex();
  assert.equal(typeof SegmentIndex, 'function');
  const ix = new SegmentIndex({ dimension: 4, metric: 'cosine' });
  assert.equal(typeof ix.open, 'function');
  assert.equal(typeof ix.addDocument, 'function');
  assert.equal(typeof ix.searchRerank, 'function');
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/addon.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/addon.js`**

```javascript
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);

// Resolves the Antelope SegmentIndex class. Override with ANTELOPE_ADDON=/abs/path/to/index.js|.node.
export function loadSegmentIndex(addonPath = process.env.ANTELOPE_ADDON) {
  const candidates = addonPath
    ? [addonPath]
    : ['../../nodejs', '../../nodejs/index.js', 'antelope-node'];
  let mod, lastErr;
  for (const c of candidates) {
    try { mod = require(c); break; } catch (e) { lastErr = e; }
  }
  if (!mod) throw new Error(`could not load Antelope addon (set ANTELOPE_ADDON): ${lastErr?.message}`);
  const SegmentIndex = mod.SegmentIndex || mod.default?.SegmentIndex || mod;
  if (typeof SegmentIndex !== 'function') throw new Error('addon did not export a SegmentIndex class');
  return SegmentIndex;
}
```

- [ ] **Step 4: Verify the addon is built, then run the test**

Run:
```bash
cd /data/tyolab/code/antelope/nodejs && ls build/Release/*.node >/dev/null 2>&1 || ./build.sh
cd /data/tyolab/code/antelope/spoor && node --test test/addon.test.js
```
Expected: PASS. If the addon's entry point isn't one of the candidates, set `ANTELOPE_ADDON` to the correct path and re-run (do not hardcode a machine-specific path in `addon.js`).

- [ ] **Step 5: Commit**

```bash
git add spoor/src/addon.js spoor/test/addon.test.js
git commit -m "feat(spoor): resolver for the Antelope SegmentIndex addon"
```

---

## Task 10: SpoorIndex — open/upsert/remove (both modes)

**Files:**
- Create: `spoor/src/index.js`
- Test: `spoor/test/index_upsert.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/index_upsert.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { SpoorIndex } from '../src/index.js';

async function dir() { return mkdtemp(join(tmpdir(), 'spoor-ix-')); }

test('single mode: upsert then documentCount reflects chunks', async () => {
  const ix = new SpoorIndex({ indexDir: await dir(), mode: 'single', model: 'm', dim: 4 });
  await ix.open();
  await ix.upsert(
    { path: 'a.js', span: [1, 3], symbol: 'alpha', kind: 'function', text: 'function alpha(){}' },
    { vector: [1, 0, 0, 0] },
  );
  await ix.flush();
  assert.equal(ix.raw.documentCount(), 1);
  await ix.close();
});

test('multi mode: upsert stores multiVectors + pooled first-stage vector', async () => {
  const ix = new SpoorIndex({ indexDir: await dir(), mode: 'multi', model: 'm', dim: 4 });
  await ix.open();
  await ix.upsert(
    { path: 'b.py', span: [1, 2], symbol: 'foo', kind: 'function', text: 'def foo(): pass' },
    { tokens: [[1, 0, 0, 0], [0, 1, 0, 0]], pooled: [0.5, 0.5, 0, 0] },
  );
  await ix.flush();
  assert.equal(ix.raw.documentCount(), 1);
  await ix.close();
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/index_upsert.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/index.js` (open/upsert/remove; search added in Task 11)**

```javascript
import { loadSegmentIndex } from './addon.js';

export function chunkKey(chunk) { return `${chunk.path}:${chunk.span[0]}-${chunk.span[1]}`; }

// Build SegmentIndex constructor options for the given representation. residentTier 'none'
// drops resident float vectors → PQ-code search (the RAM differentiator); enabled once pq=true.
function addonOptions({ mode, dim, pq }) {
  const base = { dimension: dim, metric: 'cosine', hnsw: { M: 16, efConstruction: 200, efSearch: 64 } };
  if (mode === 'multi') {
    base.rerank = { dimension: dim, quantize: 'float' };
    if (pq) base.multivectorPq = { m: 0, posture: 'rerank', rerankQuant: 'float', residentTier: 'none' };
  } else if (pq) {
    base.pq = { m: 0, posture: 'rerank', rerankQuant: 'float', residentTier: 'none' };
  }
  return base;
}

export class SpoorIndex {
  constructor({ indexDir, mode, model, dim, pq = false, addonPath }) {
    this.meta = { mode, model, dim };
    this.mode = mode; this.dim = dim; this.pq = pq;
    this.indexDir = indexDir; this.addonPath = addonPath;
    this._keysByPath = new Map();   // path -> Set(key) for incremental remove
  }

  async open() {
    const SegmentIndex = loadSegmentIndex(this.addonPath);
    this.raw = new SegmentIndex(addonOptions({ mode: this.mode, dim: this.dim, pq: this.pq }));
    this.raw.open(this.indexDir);
  }

  async upsert(chunk, embedding) {
    const key = chunkKey(chunk);
    if (this.mode === 'multi') {
      const mv = embedding.tokens.map(t => Float32Array.from(t));
      this.raw.addDocument(key, chunk.text, Float32Array.from(embedding.pooled), mv);
    } else {
      this.raw.addDocument(key, chunk.text, Float32Array.from(embedding.vector));
    }
    if (!this._keysByPath.has(chunk.path)) this._keysByPath.set(chunk.path, new Set());
    this._keysByPath.get(chunk.path).add(key);
  }

  async removePath(path, keys) {
    const set = keys ? new Set(keys) : (this._keysByPath.get(path) || new Set());
    for (const key of set) this.raw.deleteDocument(key);
    this._keysByPath.delete(path);
  }

  async flush() { await this.raw.flush(); }
  async close() { this.raw.close(); }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/index_upsert.test.js`
Expected: PASS (2 tests). Requires the built addon (Task 9).

- [ ] **Step 5: Commit**

```bash
git add spoor/src/index.js spoor/test/index_upsert.test.js
git commit -m "feat(spoor): SpoorIndex open/upsert/remove for single + multi modes"
```

---

## Task 11: SpoorIndex.search — fuse + token budget

**Files:**
- Modify: `spoor/src/index.js` (add `search`)
- Test: `spoor/test/index_search.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/index_search.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { SpoorIndex } from '../src/index.js';

async function dir() { return mkdtemp(join(tmpdir(), 'spoor-se-')); }

function chunk(path, s, e, symbol, text) { return { path, span: [s, e], symbol, kind: 'function', text }; }

test('single mode search returns re-openable, budgeted results', async () => {
  const ix = new SpoorIndex({ indexDir: await dir(), mode: 'single', model: 'm', dim: 4 });
  await ix.open();
  await ix.upsert(chunk('a.js', 1, 3, 'login', 'function login(user){ return auth(user); }'), { vector: [1, 0, 0, 0] });
  await ix.upsert(chunk('b.js', 1, 2, 'logout', 'function logout(){ return 1; }'), { vector: [0, 1, 0, 0] });
  await ix.flush();
  const out = await ix.search('login', { query: 'login', embedding: { vector: [1, 0, 0, 0] }, k: 8, maxTokens: 1500, mode: 'hybrid' });
  assert.ok(out.results.length >= 1);
  const top = out.results[0];
  assert.match(top.path, /\.js$/);
  assert.equal(top.span.length, 2);
  assert.ok(top.score >= 0 && top.score <= 1);
  assert.match(top.snippet, /login/);   // snippet centers on the matched line, not the chunk head
  assert.equal(typeof out.token_estimate, 'number');
  assert.equal(typeof out.truncated, 'boolean');
  await ix.close();
});

test('maxTokens caps the total response and sets truncated', async () => {
  const ix = new SpoorIndex({ indexDir: await dir(), mode: 'single', model: 'm', dim: 4 });
  await ix.open();
  for (let i = 0; i < 20; i++) {
    await ix.upsert(chunk(`f${i}.js`, 1, 40, `sym${i}`, 'x'.repeat(400)), { vector: [i % 4 === 0 ? 1 : 0, 1, 0, 0] });
  }
  await ix.flush();
  const out = await ix.search('x', { query: 'x', embedding: { vector: [1, 1, 0, 0] }, k: 20, maxTokens: 100, mode: 'hybrid' });
  assert.ok(out.token_estimate <= 100);
  assert.equal(out.truncated, true);
  await ix.close();
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/index_search.test.js`
Expected: FAIL (`ix.search is not a function`).

- [ ] **Step 3: Add `search` to `spoor/src/index.js`**

Add these imports at the top of the file:
```javascript
import { rrf } from './fuse.js';
import { bestWindow, firstMatchLine } from './snippet.js';
```

Add inside the `SpoorIndex` class:
```javascript
  // ~4 chars per token heuristic for budgeting the total response.
  static estTokens(str) { return Math.ceil(str.length / 4); }

  _parseKey(key) {
    const i = key.lastIndexOf(':');
    const path = key.slice(0, i);
    const [s, e] = key.slice(i + 1).split('-').map(Number);
    return { path, span: [s, e] };
  }

  // opts: { query, embedding, k, maxTokens, mode: 'hybrid'|'lexical'|'vector', filter }
  async search(text, { query, embedding, k = 8, maxTokens = 1500, mode = 'hybrid', filter }) {
    const N = Math.max(k * 4, 32);
    const lists = [];
    if (mode !== 'vector') {
      lists.push(this.raw.search(text, N, filter ? { filter } : undefined).map(h => h.key));
    }
    if (mode !== 'lexical') {
      lists.push(await this._vectorKeys(text, embedding, N, filter));
    }
    const fused = rrf(lists).slice(0, k);

    const results = [];
    let used = 0, truncated = fused.length < k ? false : fused.length > k;
    for (const row of fused) {
      const { path, span } = this._parseKey(row.key);
      const chunkText = this._textOf(row.key) ?? '';
      const snippet = bestWindow(chunkText, { hitLine: firstMatchLine(chunkText, query) });
      const rec = { path, span, symbol: this._symbolOf(row.key), kind: this._kindOf(row.key), score: row.score, snippet };
      const cost = SpoorIndex.estTokens(JSON.stringify(rec));
      if (used + cost > maxTokens) { truncated = true; break; }
      used += cost; results.push(rec);
    }
    return { results, truncated, token_estimate: used, index_stale: false };
  }

  async _vectorKeys(text, embedding, N, filter) {
    const opts = filter ? { filter } : {};
    if (this.mode === 'multi') {
      const qmv = embedding.tokens.map(t => Float32Array.from(t));
      const hits = this.raw.searchRerank(qmv, { vector: Float32Array.from(embedding.pooled), firstStageN: N * 2, topK: N, ...opts });
      return hits.map(h => h.key);
    }
    return this.raw.searchVectorHnsw(Float32Array.from(embedding.vector), N, opts).map(h => h.key);
  }
```

Also add chunk-text bookkeeping so snippets/symbols are available at search time. In the constructor add:
```javascript
    this._chunkMeta = new Map();  // key -> { text, symbol, kind }
```
In `upsert`, after computing `key`, add:
```javascript
    this._chunkMeta.set(key, { text: chunk.text, symbol: chunk.symbol, kind: chunk.kind });
```
And add these accessors to the class:
```javascript
  _textOf(key) { return this._chunkMeta.get(key)?.text; }
  _symbolOf(key) { return this._chunkMeta.get(key)?.symbol ?? undefined; }
  _kindOf(key) { return this._chunkMeta.get(key)?.kind ?? undefined; }
```

> Note: `_chunkMeta` is the in-session cache. Task 12 persists it to `spoor-chunks.json` and reloads it on open so search works across processes.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/index_search.test.js`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add spoor/src/index.js spoor/test/index_search.test.js
git commit -m "feat(spoor): SpoorIndex.search with RRF fuse + hard token budget"
```

---

## Task 12: Persist chunk metadata across processes

**Files:**
- Modify: `spoor/src/index.js` (persist/reload `_chunkMeta` + `_keysByPath`)
- Test: `spoor/test/index_persist.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/index_persist.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { SpoorIndex } from '../src/index.js';

test('chunk metadata survives close/reopen (new process semantics)', async () => {
  const d = await mkdtemp(join(tmpdir(), 'spoor-pp-'));
  const a = new SpoorIndex({ indexDir: d, mode: 'single', model: 'm', dim: 4 });
  await a.open();
  await a.upsert({ path: 'a.js', span: [1, 3], symbol: 'alpha', kind: 'function', text: 'function alpha(){ return 7; }' }, { vector: [1, 0, 0, 0] });
  await a.flush(); await a.saveSidecar(); await a.close();

  const b = new SpoorIndex({ indexDir: d, mode: 'single', model: 'm', dim: 4 });
  await b.open();
  const out = await b.search('alpha', { query: 'alpha', embedding: { vector: [1, 0, 0, 0] }, k: 4, mode: 'hybrid' });
  assert.equal(out.results[0].symbol, 'alpha');
  assert.match(out.results[0].snippet, /return 7/);
  await b.close();
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/index_persist.test.js`
Expected: FAIL (`b.saveSidecar`/reload missing → symbol undefined).

- [ ] **Step 3: Add sidecar persistence to `spoor/src/index.js`**

Add import:
```javascript
import { readFile, writeFile } from 'node:fs/promises';
import { join as pjoin } from 'node:path';
```
Add methods to the class:
```javascript
  _sidecarPath() { return pjoin(this.indexDir, 'spoor-chunks.json'); }

  async saveSidecar() {
    const obj = { keysByPath: {}, chunkMeta: {} };
    for (const [p, set] of this._keysByPath) obj.keysByPath[p] = [...set];
    for (const [k, v] of this._chunkMeta) obj.chunkMeta[k] = v;
    await writeFile(this._sidecarPath(), JSON.stringify(obj));
  }

  async _loadSidecar() {
    try {
      const obj = JSON.parse(await readFile(this._sidecarPath(), 'utf8'));
      for (const [p, arr] of Object.entries(obj.keysByPath)) this._keysByPath.set(p, new Set(arr));
      for (const [k, v] of Object.entries(obj.chunkMeta)) this._chunkMeta.set(k, v);
    } catch { /* fresh index */ }
  }
```
In `open()`, after `this.raw.open(...)`, add:
```javascript
    await this._loadSidecar();
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/index_persist.test.js`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add spoor/src/index.js spoor/test/index_persist.test.js
git commit -m "feat(spoor): persist chunk metadata sidecar across processes"
```

---

## Task 13: Reindex orchestrator + staleness + PQ flag

**Files:**
- Create: `spoor/src/reindex.js`
- Test: `spoor/test/reindex.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/reindex.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { reindex, isStale } from '../src/reindex.js';
import { startSingleStub } from './stub_embed_single.js';

async function repo() {
  const root = await mkdtemp(join(tmpdir(), 'spoor-rx-'));
  await writeFile(join(root, 'a.py'), 'def alpha():\n    return 1\n');
  return root;
}

test('reindex indexes a repo and is not stale afterward', async () => {
  const { server, url } = await startSingleStub();
  const root = await repo();
  const indexDir = join(root, '.spoor');
  const cfg = { indexDir, embedUrl: url, embedMode: 'single', embedModel: 'm' };
  const r = await reindex(root, cfg);
  assert.ok(r.indexed_files >= 1);
  assert.ok(r.chunks >= 1);
  assert.equal(await isStale(root, cfg), false);

  await writeFile(join(root, 'b.py'), 'def beta():\n    return 2\n');
  assert.equal(await isStale(root, cfg), true);
  server.close();
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/reindex.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/reindex.js`**

```javascript
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { scanRepo } from './ingester.js';
import { chunkFile } from './chunker.js';
import { Embedder } from './embedder.js';
import { SpoorIndex } from './index.js';
import { loadManifest, saveManifest, diffFiles, isCompatible, emptyManifest } from './manifest.js';

async function probeDim(embedder) {
  const r = await embedder.embed(['spoor-dim-probe'], 'doc');
  return r.dim;
}

export async function isStale(root, cfg) {
  const cur = await scanRepo(root);
  const manifest = await loadManifest(cfg.indexDir);
  const d = diffFiles(manifest.files, cur);
  return d.added.length + d.changed.length + d.deleted.length > 0;
}

export async function reindex(root, cfg, { pq = false } = {}) {
  const embedder = new Embedder(cfg);
  const dim = await probeDim(embedder);
  const want = { mode: cfg.embedMode, model: cfg.embedModel, dim };

  const cur = await scanRepo(root);
  let manifest = await loadManifest(cfg.indexDir);
  const full = !isCompatible(manifest.meta, want);   // model/mode/dim change → full rebuild
  if (full) manifest = emptyManifest();
  const { added, changed, deleted } = diffFiles(manifest.files, cur);

  const ix = new SpoorIndex({ indexDir: cfg.indexDir, mode: want.mode, model: want.model, dim, pq });
  await ix.open();

  let chunkCount = 0;
  const dirty = [...added, ...changed];
  for (const p of deleted) await ix.removePath(p);
  for (const p of changed) await ix.removePath(p);   // replace-not-append
  for (const p of dirty) {
    const source = await readFile(join(root, p), 'utf8');
    const chunks = await chunkFile(p, source);
    if (!chunks.length) continue;
    const emb = await embedder.embed(chunks.map(c => c.text), 'doc');
    for (let i = 0; i < chunks.length; i++) await ix.upsert(chunks[i], emb.items[i]);
    chunkCount += chunks.length;
  }
  await ix.flush();
  if (pq) { if (want.mode === 'multi') await ix.raw.buildMultivectorPq(); else await ix.raw.buildPq(); }
  await ix.saveSidecar();
  await ix.close();

  await saveManifest(cfg.indexDir, { files: cur, meta: want });
  return { indexed_files: dirty.length, chunks: chunkCount, elapsed_ms: 0, dirty: dirty.length > 0 };
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/reindex.test.js`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add spoor/src/reindex.js spoor/test/reindex.test.js
git commit -m "feat(spoor): reindex orchestrator, staleness check, PQ build flag"
```

---

## Task 14: Query entry point (auto-reindex on stale)

**Files:**
- Create: `spoor/src/query.js`
- Test: `spoor/test/query.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/query.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { querySearch } from '../src/query.js';
import { startSingleStub } from './stub_embed_single.js';

test('querySearch auto-reindexes a stale worktree and reports index_stale honestly', async () => {
  const { server, url } = await startSingleStub();
  const root = await mkdtemp(join(tmpdir(), 'spoor-q-'));
  await writeFile(join(root, 'a.py'), 'def login():\n    return 1\n');
  const cfg = { indexDir: join(root, '.spoor'), embedUrl: url, embedMode: 'single', embedModel: 'm' };

  const out = await querySearch(root, cfg, { query: 'login', k: 5, maxTokens: 1500, mode: 'hybrid' });
  assert.ok(out.results.length >= 1);
  assert.equal(out.index_stale, true);   // was stale (empty) before this call

  const out2 = await querySearch(root, cfg, { query: 'login', k: 5, maxTokens: 1500, mode: 'hybrid' });
  assert.equal(out2.index_stale, false);  // now fresh
  server.close();
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/query.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/query.js`**

```javascript
import { Embedder } from './embedder.js';
import { SpoorIndex } from './index.js';
import { loadManifest } from './manifest.js';
import { reindex, isStale } from './reindex.js';

export async function querySearch(root, cfg, { query, k = 8, maxTokens = 1500, mode = 'hybrid', filter, pq = false }) {
  const wasStale = await isStale(root, cfg);
  if (wasStale) await reindex(root, cfg, { pq });   // cheap incremental; "just works" on a fresh worktree

  const manifest = await loadManifest(cfg.indexDir);
  const meta = manifest.meta;
  const ix = new SpoorIndex({ indexDir: cfg.indexDir, mode: meta.mode, model: meta.model, dim: meta.dim, pq });
  await ix.open();
  const embedder = new Embedder(cfg);
  const emb = await embedder.embed([query], 'query');
  const out = await ix.search(query, { query, embedding: emb.items[0], k, maxTokens, mode, filter });
  await ix.close();
  out.index_stale = wasStale;   // report honestly even though we auto-reindexed
  return out;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor && node --test test/query.test.js`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add spoor/src/query.js spoor/test/query.test.js
git commit -m "feat(spoor): query entry point with auto-reindex-on-stale"
```

---

## Task 15: MCP server ("codesearch")

**Files:**
- Create: `spoor/src/mcp.js`
- Test: `spoor/test/mcp.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/mcp.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { buildTools } from '../src/mcp.js';
import { startSingleStub } from './stub_embed_single.js';

test('buildTools exposes search + index that operate on the repo', async () => {
  const { server, url } = await startSingleStub();
  const root = await mkdtemp(join(tmpdir(), 'spoor-mcp-'));
  await writeFile(join(root, 'a.py'), 'def handler():\n    return 1\n');
  const cfg = { indexDir: join(root, '.spoor'), embedUrl: url, embedMode: 'single', embedModel: 'm' };
  const tools = buildTools(root, cfg);

  const idx = await tools.index({});
  assert.ok(idx.indexed_files >= 1);

  const res = await tools.search({ query: 'handler', k: 5, max_tokens: 1500, mode: 'hybrid' });
  assert.ok(res.results.length >= 1);
  assert.equal(typeof res.token_estimate, 'number');
  server.close();
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/mcp.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/mcp.js`**

```javascript
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';
import { querySearch } from './query.js';
import { reindex } from './reindex.js';

// Plain callable tools (no transport) so they are unit-testable.
export function buildTools(root, cfg, { pq = false } = {}) {
  return {
    async search({ query, k = 8, paths, mode = 'hybrid', max_tokens = 1500 }) {
      return querySearch(root, cfg, { query, k, maxTokens: max_tokens, mode, pq });
    },
    async index({ paths, incremental = true } = {}) {
      return reindex(root, cfg, { pq });
    },
  };
}

export async function startMcp(root, cfg, opts = {}) {
  const tools = buildTools(root, cfg, opts);
  const server = new McpServer({ name: 'codesearch', version: '0.0.1' });

  server.tool('search',
    'Local code search: hybrid lexical+vector, token-budgeted, source-linked (path:start-end).',
    { query: z.string(), k: z.number().default(8), mode: z.enum(['hybrid', 'lexical', 'vector']).default('hybrid'), max_tokens: z.number().default(1500) },
    async (args) => ({ content: [{ type: 'text', text: JSON.stringify(await tools.search(args)) }] }),
  );
  server.tool('index',
    'Incrementally (re)index the workspace.',
    { incremental: z.boolean().default(true) },
    async (args) => ({ content: [{ type: 'text', text: JSON.stringify(await tools.index(args)) }] }),
  );

  await server.connect(new StdioServerTransport());
}
```

- [ ] **Step 4: Add `zod` to deps and run the test**

Run:
```bash
cd spoor && npm pkg set dependencies.zod="^3.23.0" && npm install >/dev/null 2>&1
node --test test/mcp.test.js
```
Expected: PASS. (The unit test exercises `buildTools`, so it passes without a live stdio transport.)

- [ ] **Step 5: Commit**

```bash
git add spoor/src/mcp.js spoor/package.json spoor/package-lock.json spoor/test/mcp.test.js
git commit -m "feat(spoor): stdio MCP server 'codesearch' (search + index tools)"
```

---

## Task 16: CLI (`spoor index|query|install`)

**Files:**
- Create: `spoor/src/cli.js`
- Test: `spoor/test/cli.test.js`

- [ ] **Step 1: Write the failing test** `spoor/test/cli.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { runCli } from '../src/cli.js';
import { startSingleStub } from './stub_embed_single.js';

test('cli index then query prints JSON results', async () => {
  const { server, url } = await startSingleStub();
  const root = await mkdtemp(join(tmpdir(), 'spoor-cli-'));
  await writeFile(join(root, 'a.py'), 'def search_handler():\n    return 1\n');
  const env = { SPOOR_INDEX_DIR: join(root, '.spoor'), EMBED_URL: url, EMBED_MODE: 'single', EMBED_MODEL: 'm' };

  const out = [];
  const log = (s) => out.push(s);
  await runCli(['index'], { env, cwd: root, log });
  await runCli(['query', 'search_handler', '--k', '3'], { env, cwd: root, log });

  const printed = out.join('\n');
  assert.match(printed, /search_handler|a\.py/);
  server.close();
});

test('cli install writes an mcp server entry to a target config', async () => {
  const root = await mkdtemp(join(tmpdir(), 'spoor-inst-'));
  const cfgPath = join(root, '.mcp.json');
  await writeFile(cfgPath, JSON.stringify({ mcpServers: {} }));
  await runCli(['install', '--config', cfgPath], { env: { SPOOR_INDEX_DIR: join(root, '.spoor'), EMBED_URL: 'http://x' }, cwd: root, log: () => {} });
  const merged = JSON.parse(await readFile(cfgPath, 'utf8'));
  assert.ok(merged.mcpServers.codesearch);
  assert.match(merged.mcpServers.codesearch.command, /node|spoor/);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor && node --test test/cli.test.js`
Expected: FAIL (module not found).

- [ ] **Step 3: Write `spoor/src/cli.js`**

```javascript
#!/usr/bin/env node
import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { loadConfig } from './config.js';
import { reindex } from './reindex.js';
import { querySearch } from './query.js';
import { startMcp } from './mcp.js';

function parseFlags(argv) {
  const flags = {}; const positional = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) { const key = argv[i].slice(2); const val = argv[i + 1]?.startsWith('--') || argv[i + 1] === undefined ? true : argv[++i]; flags[key] = val; }
    else positional.push(argv[i]);
  }
  return { flags, positional };
}

export async function runCli(argv, { env = process.env, cwd = process.cwd(), log = console.log } = {}) {
  const [cmd, ...rest] = argv;
  const { flags, positional } = parseFlags(rest);
  const pq = flags.pq === true;

  if (cmd === 'install') {
    const cfgPath = flags.config;
    const conf = JSON.parse(await readFile(cfgPath, 'utf8'));
    conf.mcpServers = conf.mcpServers || {};
    conf.mcpServers.codesearch = {
      command: 'node',
      args: [fileURLToPath(new URL('./mcp_entry.js', import.meta.url))],
      env: { SPOOR_INDEX_DIR: env.SPOOR_INDEX_DIR, EMBED_URL: env.EMBED_URL, EMBED_MODE: env.EMBED_MODE || 'single', EMBED_MODEL: env.EMBED_MODEL || '' },
    };
    await writeFile(cfgPath, JSON.stringify(conf, null, 2));
    log(`installed 'codesearch' MCP server into ${cfgPath}`);
    return;
  }

  const cfg = loadConfig(env);
  if (cmd === 'index') { log(JSON.stringify(await reindex(cwd, cfg, { pq }))); return; }
  if (cmd === 'query') {
    const query = positional[0];
    const out = await querySearch(cwd, cfg, { query, k: Number(flags.k) || 8, maxTokens: Number(flags['max-tokens']) || 1500, mode: flags.mode || 'hybrid', pq });
    log(JSON.stringify(out, null, 2));
    return;
  }
  if (cmd === 'serve') { await startMcp(cwd, cfg, { pq }); return; }
  throw new Error(`unknown command: ${cmd} (use index|query|serve|install)`);
}

if (import.meta.url === `file://${process.argv[1]}`) {
  runCli(process.argv.slice(2)).catch((e) => { console.error(e.message); process.exit(1); });
}
```

- [ ] **Step 4: Write the MCP entry shim** `spoor/src/mcp_entry.js`

```javascript
#!/usr/bin/env node
import { loadConfig } from './config.js';
import { startMcp } from './mcp.js';
const cfg = loadConfig();
startMcp(process.cwd(), cfg).catch((e) => { console.error(e.message); process.exit(1); });
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd spoor && node --test test/cli.test.js`
Expected: PASS (2 tests).

- [ ] **Step 6: Commit**

```bash
git add spoor/src/cli.js spoor/src/mcp_entry.js spoor/test/cli.test.js
git commit -m "feat(spoor): CLI (index|query|serve|install) + MCP entry shim"
```

---

## Task 17: End-to-end — both modes on a fixture repo

**Files:**
- Create: `spoor/test/e2e.test.js`

- [ ] **Step 1: Write the test** `spoor/test/e2e.test.js`

```javascript
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { querySearch } from '../src/query.js';
import { startSingleStub } from './stub_embed_single.js';
import { startMultiStub } from './stub_embed_multi.js';

async function repo() {
  const root = await mkdtemp(join(tmpdir(), 'spoor-e2e-'));
  await writeFile(join(root, 'auth.py'), 'def login(user):\n    return authenticate(user)\n\ndef logout():\n    return True\n');
  await writeFile(join(root, 'util.js'), 'function retry(fn){ return fn(); }\n');
  return root;
}

for (const [name, start, mode] of [['single', startSingleStub, 'single'], ['multi', startMultiStub, 'multi']]) {
  test(`e2e ${name}: index + search returns re-openable path:start-end`, async () => {
    const { server, url } = await start();
    const root = await repo();
    const cfg = { indexDir: join(root, '.spoor'), embedUrl: url, embedMode: mode, embedModel: 'm' };
    const out = await querySearch(root, cfg, { query: 'login', k: 5, maxTokens: 1500, mode: 'hybrid' });
    assert.ok(out.results.length >= 1);
    const r = out.results[0];
    assert.ok(Number.isInteger(r.span[0]) && Number.isInteger(r.span[1]));
    assert.ok(out.token_estimate <= 1500);
    server.close();
  });
}
```

- [ ] **Step 2: Run the whole suite**

Run: `cd spoor && node --test`
Expected: PASS across all test files (both e2e modes green). Requires the built addon.

- [ ] **Step 3: Commit**

```bash
git add spoor/test/e2e.test.js
git commit -m "test(spoor): end-to-end index+search for single and multi modes"
```

---

## Task 18: README + usage

**Files:**
- Create: `spoor/README.md`

- [ ] **Step 1: Write `spoor/README.md`**

Document: what spoor is; env vars (`SPOOR_INDEX_DIR`, `EMBED_URL`, `EMBED_MODE`, `EMBED_MODEL`, `ANTELOPE_ADDON`); the two modes (single = any OpenAI-compatible `/embeddings`; multi = the ColBERT service in Plan B); CLI (`spoor index`, `spoor query "<q>"`, `spoor serve`, `spoor install --config <.mcp.json>`); the `--pq` flag (residentTier NONE RAM win); the `search`/`index` MCP tool schemas; and the flagged follow-ons (advanced token-codec knobs need addon bindings; dashboard; more grammars).

- [ ] **Step 2: Commit**

```bash
git add spoor/README.md
git commit -m "docs(spoor): README with usage, env, modes, MCP tools"
```

---

## Self-Review Notes (for the implementer)

- **Spec coverage:** ingester (§2/§7), chunker tree-sitter+fallback (§2), embedder two modes + mean-pool (§5), index single/multi + RRF + token budget + PQ flag (§4/§6), freshness/auto-reindex (§7), MCP+CLI (§8), error handling (Task 7 fail-fast, Task 6 fallback), both-mode e2e (§10/§11). ✅
- **Addon calls used:** `addDocument(key,text,vector,multiVectors)`, `search`, `searchVectorHnsw`, `searchRerank(qmv,{vector,firstStageN,topK})`, `deleteDocument`, `flush`, `buildPq`, `buildMultivectorPq`, `documentCount` — all confirmed present in `nodejs/addon/segment_index.cpp`. No use of unbound advanced-codec knobs.
- **Type consistency:** chunk shape `{path,span:[s,e],symbol,kind,text}`, embed items `{vector}` (single) / `{tokens,pooled}` (multi), search result `{path,span,symbol?,kind?,score,snippet}` + envelope `{results,truncated,token_estimate,index_stale}` — consistent across Tasks 4–17.
- **Known verification points (not placeholders — real checks in-task):** tree-sitter grammar node-type names (Task 5 Step 5), addon entry-point path (Task 9 Step 4), MCP SDK `McpServer.tool` signature (Task 15 — if the installed SDK version differs, adjust the registration call; the unit test targets `buildTools`, which is SDK-independent).
- **Open contract point (`kind`):** the shared `search` response schema (co-owned with tyode/Parley) uses `additionalProperties:false`. The result record here includes an optional `kind` (`function`|`method`|`class`). This is proposed to the schema owner; if they decline to add `kind`, remove it from the record built in Task 11 Step 3 (single-line deletion) so output validates. `symbol` and `lang` are otherwise the optional code fields.
- **Snippet centering (contract-aligned):** snippets center on `firstMatchLine` (best lexical-term line) in BOTH modes — the addon's `searchRerank` does not expose per-token MaxSim offsets at MVP, so multi/single snippet precision is equal for now. True per-token centering is a flagged addon follow-on (expose MaxSim hit positions), addable later as an optional `highlights` field without breaking representation-agnosticism.
```

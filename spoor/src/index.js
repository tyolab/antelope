import { loadSegmentIndex } from './addon.js';
import { rrf } from './fuse.js';
import { bestWindow, firstMatchLine } from './snippet.js';
import { readFile, writeFile } from 'node:fs/promises';
import { join as pjoin } from 'node:path';

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
    this._chunkMeta = new Map();    // key -> { text, symbol, kind }
  }

  async open() {
    const SegmentIndex = loadSegmentIndex(this.addonPath);
    this.raw = new SegmentIndex(addonOptions({ mode: this.mode, dim: this.dim, pq: this.pq }));
    this.raw.open(this.indexDir);
    await this._loadSidecar();
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
    this._chunkMeta.set(key, { text: chunk.text, symbol: chunk.symbol, kind: chunk.kind });
  }

  async removePath(path, keys) {
    const set = keys ? new Set(keys) : (this._keysByPath.get(path) || new Set());
    for (const key of set) { this.raw.deleteDocument(key); this._chunkMeta.delete(key); }
    this._keysByPath.delete(path);
  }

  async flush() { await this.raw.flush(); }
  async close() { this.raw.close(); }

  // ---- search (RRF fuse + token budget) ----

  static estTokens(str) { return Math.ceil(str.length / 4); }

  _parseKey(key) {
    const i = key.lastIndexOf(':');
    const path = key.slice(0, i);
    const [s, e] = key.slice(i + 1).split('-').map(Number);
    return { path, span: [s, e] };
  }

  _textOf(key) { return this._chunkMeta.get(key)?.text; }
  _symbolOf(key) { return this._chunkMeta.get(key)?.symbol ?? undefined; }
  _kindOf(key) { return this._chunkMeta.get(key)?.kind ?? undefined; }

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
    let used = 0, truncated = fused.length > k;
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

  // ---- sidecar persistence (chunk metadata + key map) ----

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
}

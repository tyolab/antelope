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

  const ix = new SpoorIndex({ indexDir: cfg.indexDir, mode: want.mode, model: want.model, dim, pq, addonPath: cfg.addonPath });
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

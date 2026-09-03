import { Embedder } from './embedder.js';
import { SpoorIndex } from './index.js';
import { loadManifest } from './manifest.js';
import { reindex, isStale } from './reindex.js';

export async function querySearch(root, cfg, { query, k = 8, maxTokens = 1500, mode = 'hybrid', filter, pq = false }) {
  const wasStale = await isStale(root, cfg);
  if (wasStale) await reindex(root, cfg, { pq });   // cheap incremental; "just works" on a fresh worktree

  const manifest = await loadManifest(cfg.indexDir);
  const meta = manifest.meta;
  const ix = new SpoorIndex({ indexDir: cfg.indexDir, mode: meta.mode, model: meta.model, dim: meta.dim, pq, addonPath: cfg.addonPath });
  await ix.open();
  const embedder = new Embedder(cfg);
  const emb = await embedder.embed([query], 'query');
  const out = await ix.search(query, { query, embedding: emb.items[0], k, maxTokens, mode, filter });
  await ix.close();
  out.index_stale = wasStale;   // report honestly even though we auto-reindexed
  return out;
}

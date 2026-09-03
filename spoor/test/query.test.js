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

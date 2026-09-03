import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile } from 'node:fs/promises';
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

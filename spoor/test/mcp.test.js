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

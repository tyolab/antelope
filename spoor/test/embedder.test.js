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

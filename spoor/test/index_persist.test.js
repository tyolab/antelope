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

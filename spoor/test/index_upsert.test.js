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

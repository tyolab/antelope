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

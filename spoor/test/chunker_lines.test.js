import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chunkLines } from '../src/chunker_lines.js';

test('chunkLines splits into windows with 1-based inclusive spans', () => {
  const src = Array.from({ length: 50 }, (_, i) => `line${i + 1}`).join('\n');
  const chunks = chunkLines('f.txt', src, { window: 20, overlap: 5 });
  assert.equal(chunks[0].path, 'f.txt');
  assert.deepEqual(chunks[0].span, [1, 20]);
  assert.equal(chunks[0].symbol, null);
  // next window starts at 20 - 5 + 1 = 16
  assert.deepEqual(chunks[1].span, [16, 35]);
  // text of chunk 0 is lines 1..20 rejoined
  assert.equal(chunks[0].text.split('\n').length, 20);
});

test('chunkLines handles a file shorter than one window', () => {
  const chunks = chunkLines('s.txt', 'a\nb\nc', { window: 20, overlap: 5 });
  assert.equal(chunks.length, 1);
  assert.deepEqual(chunks[0].span, [1, 3]);
});

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { rrf } from '../src/fuse.js';

test('rrf fuses two ranked key lists and normalizes to [0,1]', () => {
  const lexical = ['a', 'b', 'c'];
  const vector = ['b', 'd', 'a'];
  const fused = rrf([lexical, vector], { k: 60 });
  // 'b' ranks high in both → top; scores normalized so best == 1
  assert.equal(fused[0].key, 'b');
  assert.equal(fused[0].score, 1);
  assert.ok(fused.every(f => f.score >= 0 && f.score <= 1));
  const keys = fused.map(f => f.key).sort();
  assert.deepEqual(keys, ['a', 'b', 'c', 'd']);
});

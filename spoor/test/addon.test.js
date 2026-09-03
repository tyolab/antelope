import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadSegmentIndex } from '../src/addon.js';

test('loadSegmentIndex returns a constructable class', () => {
  const SegmentIndex = loadSegmentIndex();
  assert.equal(typeof SegmentIndex, 'function');
  const ix = new SegmentIndex({ dimension: 4, metric: 'cosine' });
  assert.equal(typeof ix.open, 'function');
  assert.equal(typeof ix.addDocument, 'function');
  assert.equal(typeof ix.searchRerank, 'function');
});

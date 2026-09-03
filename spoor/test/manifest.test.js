import { test } from 'node:test';
import assert from 'node:assert/strict';
import { emptyManifest, isCompatible, diffFiles } from '../src/manifest.js';

test('emptyManifest has files map and null meta', () => {
  const m = emptyManifest();
  assert.deepEqual(m.files, {});
  assert.equal(m.meta, null);
});

test('isCompatible false when mode/model/dim differ', () => {
  const meta = { mode: 'single', model: 'a', dim: 768 };
  assert.equal(isCompatible(meta, { mode: 'single', model: 'a', dim: 768 }), true);
  assert.equal(isCompatible(meta, { mode: 'multi', model: 'a', dim: 768 }), false);
  assert.equal(isCompatible(meta, { mode: 'single', model: 'b', dim: 768 }), false);
  assert.equal(isCompatible(null, { mode: 'single', model: 'a', dim: 768 }), false);
});

test('diffFiles classifies added/changed/deleted', () => {
  const prev = { 'a.js': { hash: 'h1', mtime: 1 }, 'b.js': { hash: 'h2', mtime: 1 } };
  const cur = { 'a.js': { hash: 'h1', mtime: 1 }, 'b.js': { hash: 'h2b', mtime: 2 }, 'c.js': { hash: 'h3', mtime: 1 } };
  const d = diffFiles(prev, cur);
  assert.deepEqual(d.added, ['c.js']);
  assert.deepEqual(d.changed, ['b.js']);
  assert.deepEqual(d.deleted, []);
});

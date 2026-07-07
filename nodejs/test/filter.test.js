const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

// Docs (all share the lexical term "alpha" so lexical search matches every doc,
// leaving the filter to do the narrowing):
//   key  tenant  lang        rank  keep   payload
//   A    acme    [en,fr]      5    true   hello-A
//   B    acme    [en]         2    false  hello-B
//   C    globex  [fr]         4    true   hello-C
//   D    acme    [de,fr]      3    true   hello-D
function buildIndex(dir) {
  const idx = new SegmentIndex({
    dimension: 4,
    metric: 'dot',
    attributes: { tenant: 'string', lang: 'string[]', rank: 'int64', keep: 'bool' }
  });
  idx.open(dir);
  idx.addDocument('A', '<DOC>alpha beta</DOC>', new Float32Array([1.0, 0, 0, 0]), null,
                  { attributes: { tenant: 'acme', lang: ['en', 'fr'], rank: 5, keep: true }, payload: 'hello-A' });
  idx.addDocument('B', '<DOC>alpha beta</DOC>', new Float32Array([0.9, 0.1, 0, 0]), null,
                  { attributes: { tenant: 'acme', lang: ['en'], rank: 2, keep: false }, payload: 'hello-B' });
  idx.addDocument('C', '<DOC>alpha beta</DOC>', new Float32Array([0, 1.0, 0, 0]), null,
                  { attributes: { tenant: 'globex', lang: ['fr'], rank: 4, keep: true }, payload: 'hello-C' });
  idx.addDocument('D', '<DOC>alpha beta</DOC>', new Float32Array([0.8, 0, 0.2, 0]), null,
                  { attributes: { tenant: 'acme', lang: ['de', 'fr'], rank: 3, keep: true }, payload: 'hello-D' });
  return idx;
}

test('filter: eq narrows a vector search to one tenant', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_flt_'));
  const idx = buildIndex(dir);
  await idx.flush();
  const q = new Float32Array([1, 0, 0, 0]);
  const hits = idx.searchVector(q, 10, { filter: { eq: { tenant: 'acme' } } });
  const keys = hits.map(h => h.key).sort();
  assert.deepStrictEqual(keys, ['A', 'B', 'D']);
  assert.ok(!keys.includes('C'), 'globex doc must be excluded');
  idx.close();
});

test('filter: and(eq bool, in string[]) narrows a lexical search', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_flt_'));
  const idx = buildIndex(dir);
  await idx.flush();
  const hits = idx.search('alpha', 10, {
    filter: { and: [ { eq: { keep: true } }, { in: { lang: ['fr'] } } ] }
  });
  const keys = hits.map(h => h.key).sort();
  assert.deepStrictEqual(keys, ['A', 'C', 'D']);  // keep=true AND lang contains fr
  idx.close();
});

test('filter: range on an int64 field', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_flt_'));
  const idx = buildIndex(dir);
  await idx.flush();
  const q = new Float32Array([1, 0, 0, 0]);
  const hits = idx.searchVector(q, 10, { filter: { range: { rank: { gte: 3 } } } });
  const keys = hits.map(h => h.key).sort();
  assert.deepStrictEqual(keys, ['A', 'C', 'D']);  // rank 5,4,3 >= 3; B (rank 2) excluded
  idx.close();
});

test('filter: hits carry a payload Buffer with the right bytes', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_flt_'));
  const idx = buildIndex(dir);
  await idx.flush();
  const q = new Float32Array([1, 0, 0, 0]);
  const hits = idx.searchVector(q, 10, { filter: { eq: { tenant: 'acme' } } });
  assert.ok(hits.length > 0);
  for (const h of hits) {
    assert.ok(Buffer.isBuffer(h.payload), `hit ${h.key} should carry a payload Buffer`);
    assert.strictEqual(h.payload.toString('utf8'), 'hello-' + h.key);
  }
  idx.close();
});

test('filter: malformed predicates throw', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_flt_'));
  const idx = buildIndex(dir);
  await idx.flush();
  const q = new Float32Array([1, 0, 0, 0]);
  // number for a string field
  assert.throws(() => idx.searchVector(q, 10, { filter: { eq: { tenant: 123 } } }), /string/i);
  // range on a string field
  assert.throws(() => idx.searchVector(q, 10, { filter: { range: { tenant: { gte: 1 } } } }), /range/i);
  // unknown field
  assert.throws(() => idx.searchVector(q, 10, { filter: { eq: { nope: 'x' } } }), /unknown/i);
  idx.close();
});

test('filter: an unfiltered search still returns everything', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_flt_'));
  const idx = buildIndex(dir);
  await idx.flush();
  const q = new Float32Array([1, 0, 0, 0]);
  const hits = idx.searchVector(q, 10);
  assert.strictEqual(hits.length, 4);
  idx.close();
});

const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

function denseDocs(idx, dim, n = 40) {
  for (let i = 0; i < n; i++) {
    const v = new Float32Array(dim);
    for (let d = 0; d < dim; d++) v[d] = ((i * 16 + d) % 97) / 50 - 1;
    idx.addDocument('d' + i, '<DOC>apple ' + i + '</DOC>', v);
  }
}

test('pq: replace posture builds and searches', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_pq_'));
  const dim = 8;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', pq: { m: 0, posture: 'replace' } });
  idx.open(dir);
  denseDocs(idx, dim);
  await idx.flush();
  await idx.buildPq();
  const q = new Float32Array(dim); q[0] = 1;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

test('pq: rerank posture + int8 resident tier', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_pqr_'));
  const dim = 8;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', pq: { posture: 'rerank', residentTier: 'int8' } });
  idx.open(dir);
  denseDocs(idx, dim);
  await idx.flush();
  await idx.buildPq();
  const q = new Float32Array(dim); q[0] = 1;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

test('multivectorPq: builds and reranks', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_mvpq_'));
  const dim = 4;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', rerank: { dimension: dim }, multivectorPq: { m: 0 } });
  idx.open(dir);
  for (let i = 0; i < 12; i++) {
    const v = new Float32Array([i + 1, 1, 0, 0]);
    idx.addDocument('d' + i, '<DOC>apple</DOC>', v, [v]);
  }
  await idx.flush();
  await idx.buildMultivectorPq();
  const q = new Float32Array([1, 1, 0, 0]);
  assert.ok(idx.searchRerank([q], { text: 'apple', firstStageN: 20, topK: 5 }).length >= 1);
  idx.close();
});

test('pq: config persists on reopen', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_pqp_'));
  const dim = 8;
  let idx = new SegmentIndex({ dimension: dim, metric: 'cosine', pq: { m: 0, posture: 'replace' } });
  idx.open(dir);
  denseDocs(idx, dim);
  await idx.flush();
  await idx.buildPq();
  idx.close();
  idx = new SegmentIndex({ dimension: dim, metric: 'cosine', pq: { m: 0, posture: 'replace' } });
  idx.open(dir);
  const q = new Float32Array(dim); q[0] = 1;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

test('pq: mutually exclusive with int8 quantize (stays off, no throw)', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_pqx_'));
  const dim = 8;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', quantize: 'int8', pq: { posture: 'replace' } });
  idx.open(dir);
  denseDocs(idx, dim);
  await idx.flush();
  const q = new Float32Array(dim); q[0] = 1;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

test('multivectorPq: conflicts with explicit int8 rerank (stays off)', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_mvx_'));
  const dim = 4;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', rerank: { dimension: dim, quantize: 'int8' }, multivectorPq: { m: 0 } });
  idx.open(dir);
  for (let i = 0; i < 12; i++) {
    const v = new Float32Array([i + 1, 1, 0, 0]);
    idx.addDocument('d' + i, '<DOC>apple</DOC>', v, [v]);
  }
  await idx.flush();
  await assert.rejects(idx.buildMultivectorPq());   // token-PQ not configured (int8 won)
  const q = new Float32Array([1, 1, 0, 0]);
  assert.ok(idx.searchRerank([q], { text: 'apple', firstStageN: 20, topK: 5 }).length >= 1);   // int8 rerank still works
  idx.close();
});

test('pq: bad posture throws', () => {
  assert.throws(() => new SegmentIndex({ dimension: 8, metric: 'cosine', pq: { posture: 'bogus' } }));
});

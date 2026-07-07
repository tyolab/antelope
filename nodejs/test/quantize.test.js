const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

test('quantize: replace mode writes .qvec (no float .vec) and searches', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_q_'));
  const dim = 16;
  const idx = new SegmentIndex({ dimension: dim, metric: 'l2', quantize: 'int8' });
  idx.open(dir);
  for (let i = 0; i < 40; i++) {
    const v = new Float32Array(dim);
    for (let d = 0; d < dim; d++) v[d] = (i * 16 + d) / 100;
    idx.addDocument('d' + i, '<DOC>doc ' + i + '</DOC>', v);
  }
  await idx.flush();
  const files = fs.readdirSync(dir);
  assert.ok(files.some(f => f.endsWith('.qvec')), '.qvec written');
  assert.ok(!files.some(f => f.endsWith('.vec')), 'no float .vec in replace mode');
  const q = new Float32Array(dim);
  for (let d = 0; d < dim; d++) q[d] = (5 * 16 + d) / 100;
  const hits = idx.searchVector(q, 5);
  assert.ok(hits.length >= 1);
  idx.close();
});

test('quantize: buildQuantized() resolves (idempotent on an already-int8 index)', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_qb_'));
  const dim = 16;
  const idx = new SegmentIndex({ dimension: dim, metric: 'l2', quantize: { mode: 'replace' } });
  idx.open(dir);
  for (let i = 0; i < 20; i++) {
    const v = new Float32Array(dim);
    for (let d = 0; d < dim; d++) v[d] = (i * 16 + d) / 100;
    idx.addDocument('d' + i, '<DOC>doc ' + i + '</DOC>', v);
  }
  await idx.flush();
  await idx.buildQuantized();		// all segments already int8 -> no-op success
  assert.ok(fs.readdirSync(dir).some(f => f.endsWith('.qvec')));
  const q = new Float32Array(dim);
  for (let d = 0; d < dim; d++) q[d] = (10 * 16 + d) / 100;
  assert.ok(idx.searchVector(q, 5).length >= 1);
  idx.close();
});

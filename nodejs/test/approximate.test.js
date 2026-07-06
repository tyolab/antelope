const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

test('approximate: buildSignatures + searchVectorApprox returns hits', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_approx_'));
  const dim = 16;
  const idx = new SegmentIndex({ dimension: dim, metric: 'cosine', approximate: { bits: 128, multiplier: 4 } });
  idx.open(dir);
  for (let i = 0; i < 40; i++) {
    const v = new Float32Array(dim);
    for (let d = 0; d < dim; d++) v[d] = ((i + d) % 7) - 3;
    idx.addDocument('k' + i, '<DOC>alpha</DOC>', v);
  }
  await idx.flush();
  await idx.buildSignatures();
  const q = new Float32Array(dim);
  for (let d = 0; d < dim; d++) q[d] = (d % 5) - 2;
  const hits = idx.searchVectorApprox(q, 5);
  assert.strictEqual(hits.length, 5);
  assert.ok(hits[0].key && typeof hits[0].score === 'number');
  idx.close();
});

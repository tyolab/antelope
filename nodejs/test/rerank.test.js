const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

test('rerank: searchRerank reorders vs searchVector', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant_rr_'));
  const idx = new SegmentIndex({ dimension: 4, metric: 'dot', rerank: { dimension: 4, quantize: 'float' } });
  idx.open(dir);
  const qvec = new Float32Array([1,0,0,0]);
  idx.addDocument('A', '<DOC>a</DOC>', new Float32Array([0.9,0.1,0,0]),
                  [new Float32Array([1,0,0,0]), new Float32Array([0,1,0,0])]);
  idx.addDocument('B', '<DOC>b</DOC>', new Float32Array([0.5,0.5,0,0]),
                  [new Float32Array([1,0,0,0]), new Float32Array([0,0,1,0])]);
  await idx.flush();
  const stage1 = idx.searchVector(qvec, 2);
  assert.strictEqual(stage1[0].key, 'A');
  const qmv = [new Float32Array([0,0,1,0])];
  const reranked = idx.searchRerank(qmv, { vector: qvec, firstStageN: 10, topK: 2 });
  assert.strictEqual(reranked[0].key, 'B');		// MaxSim lifts B
  idx.close();
});

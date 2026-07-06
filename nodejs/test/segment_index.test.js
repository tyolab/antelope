'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { SegmentIndex } = require('../build/Release/antelope_segment.node');

function freshDir() { return fs.mkdtempSync(path.join(os.tmpdir(), 'ant-binding-')); }

test('open/close lifecycle and accessors', () => {
	const idx = new SegmentIndex();
	assert.throws(() => idx.documentCount(), /not open/);
	idx.open(freshDir());
	assert.strictEqual(idx.documentCount(), 0);
	assert.strictEqual(idx.vectorDimension(), 0);
	idx.close();
	assert.throws(() => idx.documentCount(), /not open/);
	assert.throws(() => idx.open(freshDir()), /closed/);	// a closed instance is done
});

test('vector options are applied and validated', () => {
	const dir = freshDir();
	const idx = new SegmentIndex({ dimension: 4, metric: 'cosine' });
	idx.open(dir);
	assert.strictEqual(idx.vectorDimension(), 4);
	idx.close();

	// config mismatch on reopen throws
	const wrong = new SegmentIndex({ dimension: 8, metric: 'cosine' });
	assert.throws(() => wrong.open(dir), /open failed/);

	// bad option values throw at construction
	assert.throws(() => new SegmentIndex({ dimension: 4, metric: 'euclideanish' }), /metric/);
	assert.throws(() => new SegmentIndex({ dimension: 0 }), /dimension/);
});

test('open failure leaves instance reusable', () => {
	const idx = new SegmentIndex();
	assert.throws(() => idx.open('/nonexistent-parent-zzz/sub'), /open failed/);
	idx.open(freshDir());		// still usable after a failed open
	assert.strictEqual(idx.documentCount(), 0);
	idx.close();
});

test('add, search, update, delete — lexical', () => {
	const idx = new SegmentIndex();
	idx.open(freshDir());
	const ref = idx.addDocument('doc-1', '<DOC>aardvark zebra</DOC>');
	assert.strictEqual(typeof ref.generation, 'number');
	assert.strictEqual(typeof ref.docid, 'number');
	idx.addDocument('doc-2', '<DOC>zebra quokka</DOC>');

	let hits = idx.search('zebra', 10);
	assert.strictEqual(hits.length, 2);
	assert.ok(hits[0].key === 'doc-1' || hits[0].key === 'doc-2');
	assert.strictEqual(typeof hits[0].score, 'number');

	idx.updateDocument('doc-1', '<DOC>wombat only</DOC>');
	assert.strictEqual(idx.search('aardvark', 10).length, 0);
	assert.strictEqual(idx.search('wombat', 10).length, 1);

	assert.strictEqual(idx.deleteDocument('doc-2'), true);
	assert.strictEqual(idx.deleteDocument('no-such-key'), false);
	assert.strictEqual(idx.search('quokka', 10).length, 0);

	assert.throws(() => idx.addDocument('empty', '<DOC></DOC>'), /rejected/);
	idx.close();
});

test('vectors: typed arrays, plain arrays, hybrid, type errors', () => {
	const idx = new SegmentIndex({ dimension: 4, metric: 'dot' });
	idx.open(freshDir());
	idx.addDocument('doc-a', '<DOC>alpha content</DOC>', Float32Array.from([1, 0, 0, 0]));
	idx.addDocument('doc-b', '<DOC>beta content</DOC>', [0.9, 0.1, 0, 0]);	// plain array accepted
	idx.addDocument('doc-c', '<DOC>gamma lexical only</DOC>');

	const vhits = idx.searchVector(Float32Array.from([1, 0, 0, 0]), 10);
	assert.strictEqual(vhits.length, 2);						// doc-c has no vector
	assert.strictEqual(vhits[0].key, 'doc-a');
	assert.ok(vhits[0].score > vhits[1].score);

	// hybrid: doc-a matches both sides, must rank first
	const hhits = idx.searchHybrid('alpha', Float32Array.from([1, 0, 0, 0]), 3);
	assert.strictEqual(hhits[0].key, 'doc-a');
	// degradations
	assert.strictEqual(idx.searchHybrid(null, Float32Array.from([1, 0, 0, 0]), 3).length, 2);
	assert.strictEqual(idx.searchHybrid('gamma', null, 3).length, 1);
	assert.deepStrictEqual(idx.searchHybrid(null, null, 3), []);

	assert.throws(() => idx.addDocument('bad', '<DOC>x y</DOC>', Float32Array.from([1, 0])), /dimension/);
	assert.throws(() => idx.addDocument('bad', '<DOC>x y</DOC>', 'not a vector'), /TypeError|vector/);
	assert.throws(() => idx.searchVector(Float32Array.from([1, 0]), 5), /dimension/);
	idx.close();

	const plain = new SegmentIndex();
	plain.open(freshDir());
	assert.throws(() => plain.addDocument('k', '<DOC>a b</DOC>', Float32Array.from([1])), /not.*enabled|vector/i);
	assert.deepStrictEqual(plain.searchVector(Float32Array.from([1]), 5), []);
	plain.close();
});

test('cosine zero-vector rejected', () => {
	const idx = new SegmentIndex({ dimension: 3, metric: 'cosine' });
	idx.open(freshDir());
	assert.throws(() => idx.addDocument('z', '<DOC>a b</DOC>', Float32Array.from([0, 0, 0])), /zero/);
	idx.close();
});

test('flush persists; maintain compacts; both are Promises', async () => {
	const dir = freshDir();
	const idx = new SegmentIndex({ mergeFactor: 2, tombstoneRatio: 0.2 });
	idx.open(dir);
	for (let i = 0; i < 8; i++)
		idx.addDocument(`doc-${i}`, `<DOC>common filler${'x'.repeat(i)}</DOC>`);
	await idx.flush();
	for (let i = 0; i < 5; i++)
		assert.strictEqual(idx.deleteDocument(`doc-${i}`), true);
	assert.strictEqual(idx.documentCount(), 3);
	await idx.maintain();				// tombstone ratio 5/8 > 0.2 -> compaction
	assert.strictEqual(idx.documentCount(), 3);
	assert.strictEqual(idx.search('common', 100).length, 3);
	idx.close();

	const reopened = new SegmentIndex();
	reopened.open(dir);
	assert.strictEqual(reopened.documentCount(), 3);
	reopened.close();
});

test('busy-guard: engine calls during maintenance throw', async () => {
	const idx = new SegmentIndex();
	idx.open(freshDir());
	for (let i = 0; i < 50; i++)
		idx.addDocument(`doc-${i}`, `<DOC>word${i} common</DOC>`);
	// The guard is set synchronously inside flush() (state -> MAINTENANCE)
	// BEFORE the AsyncWorker is queued and flush() returns the pending
	// promise.  The state is only restored to OPEN from the worker's
	// OnOK/OnError callbacks, which run on the event loop and therefore
	// cannot execute until this synchronous test function body yields (at
	// the first `await` below).  So the assertions between `idx.flush()`
	// and that first `await` are deterministic regardless of how fast the
	// underlying flush()/maintain() engine call actually completes.
	const pending = idx.flush();		// state -> MAINTENANCE until settled
	assert.throws(() => idx.addDocument('during', '<DOC>x y</DOC>'), /maintenance in progress/);
	assert.throws(() => idx.search('common', 5), /maintenance in progress/);
	assert.throws(() => idx.close(), /maintenance in progress/);
	await assert.rejects(Promise.race([idx.maintain()]), /maintenance in progress/);
	await pending;
	assert.strictEqual(idx.search('common', 100).length, 50);	// usable again
	idx.close();
});

test('durable mode recovers unflushed writes across sessions', async () => {
	const dir = freshDir();
	let idx = new SegmentIndex({ durable: true, dimension: 4, metric: 'dot' });
	idx.open(dir);
	idx.addDocument('doc-1', '<DOC>alpha survivor</DOC>', Float32Array.from([1, 0, 0, 0]));
	idx.close();		// no flush: relaxed mode would lose this

	idx = new SegmentIndex({ durable: true, dimension: 4, metric: 'dot' });
	idx.open(dir);
	assert.strictEqual(idx.documentCount(), 1);
	assert.strictEqual(idx.search('survivor', 5).length, 1);
	assert.strictEqual(idx.searchVector(Float32Array.from([1, 0, 0, 0]), 5).length, 1);
	await idx.flush();
	idx.close();
});

test('globalStats option round-trips', () => {
	const idx = new SegmentIndex({ globalStats: false });
	idx.open(freshDir());
	idx.addDocument('doc-1', '<DOC>alpha</DOC>');
	assert.strictEqual(idx.search('alpha', 5).length, 1);	// still functional
	idx.close();
});

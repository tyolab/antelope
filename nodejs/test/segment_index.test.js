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

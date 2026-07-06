'use strict';
const test = require('node:test');
const assert = require('node:assert');

test('addon loads and reports a version', () => {
	const addon = require('../build/Release/antelope_segment.node');
	assert.strictEqual(typeof addon.version, 'string');
	assert.ok(addon.version.length > 0);
});

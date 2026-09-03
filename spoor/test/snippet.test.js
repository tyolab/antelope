import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bestWindow, firstMatchLine } from '../src/snippet.js';

test('bestWindow centers on the matched line', () => {
  const text = Array.from({ length: 20 }, (_, i) => `line ${i + 1}`).join('\n');
  const s = bestWindow(text, { hitLine: 10, radius: 2 });
  assert.equal(s, ['line 8', 'line 9', 'line 10', 'line 11', 'line 12'].join('\n'));
});

test('bestWindow falls back to the head when hitLine is null', () => {
  const text = 'a\nb\nc\nd\ne\nf';
  const s = bestWindow(text, { hitLine: null, radius: 2, headLines: 3 });
  assert.equal(s, 'a\nb\nc');
});

test('firstMatchLine returns the 1-based line of the best query-term hit', () => {
  const text = 'def helper():\n    pass\n\ndef login(user):\n    return auth(user)\n';
  // "login" first appears on line 4
  assert.equal(firstMatchLine(text, 'login user'), 4);
});

test('firstMatchLine returns null when no term matches', () => {
  assert.equal(firstMatchLine('a\nb\nc', 'zzz'), null);
});

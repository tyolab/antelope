import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chunkTreeSitter } from '../src/chunker_treesitter.js';

test('extracts top-level function symbols from JS', async () => {
  const src = [
    'function alpha(a, b) {',
    '  return a + b;',
    '}',
    '',
    'const beta = () => 42;',
  ].join('\n');
  const chunks = await chunkTreeSitter('m.js', src);
  const alpha = chunks.find(c => c.symbol === 'alpha');
  assert.ok(alpha, 'alpha symbol present');
  assert.equal(alpha.kind, 'function');
  assert.deepEqual(alpha.span, [1, 3]);
  assert.match(alpha.text, /return a \+ b/);
  assert.ok(chunks.find(c => c.symbol === 'beta'), 'beta arrow present');
});

test('extracts def/class symbols and methods from Python', async () => {
  const src = 'def foo(x):\n    return x\n\nclass Bar:\n    def m(self):\n        return 1\n';
  const chunks = await chunkTreeSitter('m.py', src);
  assert.ok(chunks.find(c => c.symbol === 'foo' && c.kind === 'function'));
  assert.ok(chunks.find(c => c.symbol === 'Bar' && c.kind === 'class'));
  assert.ok(chunks.find(c => c.symbol === 'm' && c.kind === 'method'));
});

test('returns null for an unsupported extension', async () => {
  assert.equal(await chunkTreeSitter('x.rb', 'puts 1'), null);
});

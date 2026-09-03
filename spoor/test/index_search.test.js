import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { SpoorIndex } from '../src/index.js';

async function dir() { return mkdtemp(join(tmpdir(), 'spoor-se-')); }

function chunk(path, s, e, symbol, text) { return { path, span: [s, e], symbol, kind: 'function', text }; }

test('single mode search returns re-openable, budgeted results', async () => {
  const ix = new SpoorIndex({ indexDir: await dir(), mode: 'single', model: 'm', dim: 4 });
  await ix.open();
  await ix.upsert(chunk('a.js', 1, 3, 'login', 'function login(user){ return auth(user); }'), { vector: [1, 0, 0, 0] });
  await ix.upsert(chunk('b.js', 1, 2, 'logout', 'function logout(){ return 1; }'), { vector: [0, 1, 0, 0] });
  await ix.flush();
  const out = await ix.search('login', { query: 'login', embedding: { vector: [1, 0, 0, 0] }, k: 8, maxTokens: 1500, mode: 'hybrid' });
  assert.ok(out.results.length >= 1);
  const top = out.results[0];
  assert.match(top.path, /\.js$/);
  assert.equal(top.span.length, 2);
  assert.ok(top.score >= 0 && top.score <= 1);
  assert.match(top.snippet, /login/);   // snippet centers on the matched line, not the chunk head
  assert.equal(typeof out.token_estimate, 'number');
  assert.equal(typeof out.truncated, 'boolean');
  await ix.close();
});

test('maxTokens caps the total response and sets truncated', async () => {
  const ix = new SpoorIndex({ indexDir: await dir(), mode: 'single', model: 'm', dim: 4 });
  await ix.open();
  for (let i = 0; i < 20; i++) {
    await ix.upsert(chunk(`f${i}.js`, 1, 40, `sym${i}`, 'x'.repeat(400)), { vector: [i % 4 === 0 ? 1 : 0, 1, 0, 0] });
  }
  await ix.flush();
  const out = await ix.search('x', { query: 'x', embedding: { vector: [1, 1, 0, 0] }, k: 20, maxTokens: 100, mode: 'hybrid' });
  assert.ok(out.token_estimate <= 100);
  assert.equal(out.truncated, true);
  await ix.close();
});

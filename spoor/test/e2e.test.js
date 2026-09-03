import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { querySearch } from '../src/query.js';
import { startSingleStub } from './stub_embed_single.js';
import { startMultiStub } from './stub_embed_multi.js';

async function repo() {
  const root = await mkdtemp(join(tmpdir(), 'spoor-e2e-'));
  await writeFile(join(root, 'auth.py'), 'def login(user):\n    return authenticate(user)\n\ndef logout():\n    return True\n');
  await writeFile(join(root, 'util.js'), 'function retry(fn){ return fn(); }\n');
  return root;
}

for (const [name, start, mode] of [['single', startSingleStub, 'single'], ['multi', startMultiStub, 'multi']]) {
  test(`e2e ${name}: index + search returns re-openable path:start-end`, async () => {
    const { server, url } = await start();
    const root = await repo();
    const cfg = { indexDir: join(root, '.spoor'), embedUrl: url, embedMode: mode, embedModel: 'm' };
    const out = await querySearch(root, cfg, { query: 'login', k: 5, maxTokens: 1500, mode: 'hybrid' });
    assert.ok(out.results.length >= 1);
    const r = out.results[0];
    assert.ok(Number.isInteger(r.span[0]) && Number.isInteger(r.span[1]));
    assert.ok(out.token_estimate <= 1500);
    server.close();
  });
}

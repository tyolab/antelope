import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { runCli } from '../src/cli.js';
import { startSingleStub } from './stub_embed_single.js';

test('cli index then query prints JSON results', async () => {
  const { server, url } = await startSingleStub();
  const root = await mkdtemp(join(tmpdir(), 'spoor-cli-'));
  await writeFile(join(root, 'a.py'), 'def search_handler():\n    return 1\n');
  const env = { SPOOR_INDEX_DIR: join(root, '.spoor'), EMBED_URL: url, EMBED_MODE: 'single', EMBED_MODEL: 'm' };

  const out = [];
  const log = (s) => out.push(s);
  await runCli(['index'], { env, cwd: root, log });
  await runCli(['query', 'search_handler', '--k', '3'], { env, cwd: root, log });

  const printed = out.join('\n');
  assert.match(printed, /search_handler|a\.py/);
  server.close();
});

test('cli install writes an mcp server entry to a target config', async () => {
  const root = await mkdtemp(join(tmpdir(), 'spoor-inst-'));
  const cfgPath = join(root, '.mcp.json');
  await writeFile(cfgPath, JSON.stringify({ mcpServers: {} }));
  await runCli(['install', '--config', cfgPath], { env: { SPOOR_INDEX_DIR: join(root, '.spoor'), EMBED_URL: 'http://x' }, cwd: root, log: () => {} });
  const merged = JSON.parse(await readFile(cfgPath, 'utf8'));
  assert.ok(merged.mcpServers.codesearch);
  assert.match(merged.mcpServers.codesearch.command, /node|spoor/);
});

#!/usr/bin/env node
import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { loadConfig } from './config.js';
import { reindex } from './reindex.js';
import { querySearch } from './query.js';
import { startMcp } from './mcp.js';

function parseFlags(argv) {
  const flags = {}; const positional = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) { const key = argv[i].slice(2); const val = argv[i + 1]?.startsWith('--') || argv[i + 1] === undefined ? true : argv[++i]; flags[key] = val; }
    else positional.push(argv[i]);
  }
  return { flags, positional };
}

export async function runCli(argv, { env = process.env, cwd = process.cwd(), log = console.log } = {}) {
  const [cmd, ...rest] = argv;
  const { flags, positional } = parseFlags(rest);
  const pq = flags.pq === true;

  if (cmd === 'install') {
    const cfgPath = flags.config;
    const conf = JSON.parse(await readFile(cfgPath, 'utf8'));
    conf.mcpServers = conf.mcpServers || {};
    conf.mcpServers.codesearch = {
      command: 'node',
      args: [fileURLToPath(new URL('./mcp_entry.js', import.meta.url))],
      env: { SPOOR_INDEX_DIR: env.SPOOR_INDEX_DIR, EMBED_URL: env.EMBED_URL, EMBED_MODE: env.EMBED_MODE || 'single', EMBED_MODEL: env.EMBED_MODEL || '' },
    };
    await writeFile(cfgPath, JSON.stringify(conf, null, 2));
    log(`installed 'codesearch' MCP server into ${cfgPath}`);
    return;
  }

  const cfg = loadConfig(env);
  if (cmd === 'index') { log(JSON.stringify(await reindex(cwd, cfg, { pq }))); return; }
  if (cmd === 'query') {
    const query = positional[0];
    const out = await querySearch(cwd, cfg, { query, k: Number(flags.k) || 8, maxTokens: Number(flags['max-tokens']) || 1500, mode: flags.mode || 'hybrid', pq });
    log(JSON.stringify(out, null, 2));
    return;
  }
  if (cmd === 'serve') { await startMcp(cwd, cfg, { pq }); return; }
  throw new Error(`unknown command: ${cmd} (use index|query|serve|install)`);
}

if (import.meta.url === `file://${process.argv[1]}`) {
  runCli(process.argv.slice(2)).catch((e) => { console.error(e.message); process.exit(1); });
}

#!/usr/bin/env node
import { loadConfig } from './config.js';
import { startMcp } from './mcp.js';
const cfg = loadConfig();
startMcp(process.cwd(), cfg).catch((e) => { console.error(e.message); process.exit(1); });

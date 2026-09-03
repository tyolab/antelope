import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadConfig } from '../src/config.js';

test('loadConfig reads env with defaults', () => {
  const c = loadConfig({
    SPOOR_INDEX_DIR: '/tmp/ix',
    EMBED_URL: 'http://work2:8900',
    EMBED_MODEL: 'gte-moderncolbert',
    EMBED_MODE: 'multi',
  });
  assert.equal(c.indexDir, '/tmp/ix');
  assert.equal(c.embedUrl, 'http://work2:8900');
  assert.equal(c.embedMode, 'multi');
});

test('loadConfig defaults embedMode to single', () => {
  const c = loadConfig({ SPOOR_INDEX_DIR: '/tmp/ix', EMBED_URL: 'http://x' });
  assert.equal(c.embedMode, 'single');
});

test('loadConfig throws when required env missing', () => {
  assert.throws(() => loadConfig({}), /SPOOR_INDEX_DIR/);
});

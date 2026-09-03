import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { scanRepo } from '../src/ingester.js';

async function fixture() {
  const root = await mkdtemp(join(tmpdir(), 'spoor-ing-'));
  await writeFile(join(root, '.gitignore'), 'node_modules/\n*.log\n');
  await writeFile(join(root, 'a.js'), 'const a = 1;\n');
  await mkdir(join(root, 'node_modules', 'x'), { recursive: true });
  await writeFile(join(root, 'node_modules', 'x', 'y.js'), 'ignored\n');
  await writeFile(join(root, 'debug.log'), 'ignored\n');
  await mkdir(join(root, 'sub'), { recursive: true });
  await writeFile(join(root, 'sub', 'b.py'), 'x = 1\n');
  return root;
}

test('scanRepo honors gitignore and returns hash+mtime', async () => {
  const root = await fixture();
  const files = await scanRepo(root);
  const paths = Object.keys(files).sort();
  assert.deepEqual(paths, ['a.js', 'sub/b.py']);
  assert.match(files['a.js'].hash, /^[0-9a-f]{16,}$/);
  assert.equal(typeof files['a.js'].mtime, 'number');
});

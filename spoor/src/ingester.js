import { readdir, readFile, stat } from 'node:fs/promises';
import { join, relative, sep } from 'node:path';
import { createHash } from 'node:crypto';
import ignore from 'ignore';

async function readIgnore(root) {
  const ig = ignore();
  ig.add(['.git/', '.spoor/', '.gitignore', '.spoorignore']);
  for (const f of ['.gitignore', '.spoorignore']) {
    try { ig.add(await readFile(join(root, f), 'utf8')); } catch { /* absent */ }
  }
  return ig;
}

export async function scanRepo(root) {
  const ig = await readIgnore(root);
  const out = {};
  async function walk(dir) {
    const entries = await readdir(dir, { withFileTypes: true });
    for (const e of entries) {
      const abs = join(dir, e.name);
      let rel = relative(root, abs);
      if (sep !== '/') rel = rel.split(sep).join('/');
      const test = e.isDirectory() ? rel + '/' : rel;
      if (ig.ignores(test)) continue;
      if (e.isDirectory()) await walk(abs);
      else if (e.isFile()) {
        const buf = await readFile(abs);
        const st = await stat(abs);
        out[rel] = { hash: createHash('sha1').update(buf).digest('hex'), mtime: Math.floor(st.mtimeMs) };
      }
    }
  }
  await walk(root);
  return out;
}

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { dirname, join } from 'node:path';

export function emptyManifest() { return { files: {}, meta: null }; }

export function isCompatible(meta, want) {
  return !!meta && meta.mode === want.mode && meta.model === want.model && meta.dim === want.dim;
}

export function diffFiles(prev, cur) {
  const added = [], changed = [], deleted = [];
  for (const p of Object.keys(cur)) {
    if (!(p in prev)) added.push(p);
    else if (prev[p].hash !== cur[p].hash) changed.push(p);
  }
  for (const p of Object.keys(prev)) if (!(p in cur)) deleted.push(p);
  return { added, changed, deleted };
}

export async function loadManifest(indexDir) {
  try {
    const raw = await readFile(join(indexDir, 'spoor-manifest.json'), 'utf8');
    return JSON.parse(raw);
  } catch { return emptyManifest(); }
}

export async function saveManifest(indexDir, manifest) {
  const path = join(indexDir, 'spoor-manifest.json');
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, JSON.stringify(manifest));
}

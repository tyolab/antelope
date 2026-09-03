import { chunkTreeSitter } from './chunker_treesitter.js';
import { chunkLines } from './chunker_lines.js';

export async function chunkFile(path, source) {
  try {
    const ts = await chunkTreeSitter(path, source);
    if (ts && ts.length) return ts;
  } catch { /* fall through to line-windows */ }
  return chunkLines(path, source);
}

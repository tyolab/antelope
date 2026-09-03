import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);

// Resolves the Antelope SegmentIndex class. Override with ANTELOPE_ADDON=/abs/path/to/antelope_segment.node.
// NOTE: nodejs/index.js is the legacy SWIG entry with a hard Node<=10 guard that throws before
// its lazy SegmentIndex getter — so on modern Node the addon MUST be loaded from the .node directly.
export function loadSegmentIndex(addonPath = process.env.ANTELOPE_ADDON) {
  const candidates = addonPath
    ? [addonPath]
    : [
        '../../nodejs/build/Release/antelope_segment.node',
        '../../nodejs/build/Debug/antelope_segment.node',
        'antelope-search/build/Release/antelope_segment.node',
      ];
  let mod, lastErr;
  for (const c of candidates) {
    try { mod = require(c); break; } catch (e) { lastErr = e; }
  }
  if (!mod) throw new Error(`could not load Antelope addon (set ANTELOPE_ADDON to antelope_segment.node): ${lastErr?.message}`);
  const SegmentIndex = mod.SegmentIndex || mod.default?.SegmentIndex;
  if (typeof SegmentIndex !== 'function') throw new Error('addon did not export a SegmentIndex class');
  return SegmentIndex;
}

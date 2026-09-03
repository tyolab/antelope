// Return a small window of `text` centered on hitLine (1-based within the chunk),
// or the head if hitLine is null.
export function bestWindow(text, { hitLine, radius = 3, headLines = 6 } = {}) {
  const lines = text.split('\n');
  if (hitLine == null) return lines.slice(0, headLines).join('\n');
  const idx = hitLine - 1;
  const start = Math.max(0, idx - radius);
  const end = Math.min(lines.length, idx + radius + 1);
  return lines.slice(start, end).join('\n');
}

// 1-based line within `text` of the first query-term occurrence (case-insensitive),
// or null if no term matches. Used to center the snippet on the best-matched line
// (both modes) since the addon does not expose per-token MaxSim offsets at MVP.
export function firstMatchLine(text, query) {
  const terms = query.toLowerCase().split(/[^a-z0-9_]+/i).filter(Boolean);
  if (!terms.length) return null;
  const lines = text.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const lc = lines[i].toLowerCase();
    if (terms.some(t => lc.includes(t))) return i + 1;
  }
  return null;
}

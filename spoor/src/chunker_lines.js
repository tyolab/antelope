export function chunkLines(path, source, { window = 40, overlap = 8 } = {}) {
  const lines = source.split('\n');
  const n = lines.length;
  const step = Math.max(1, window - overlap);
  const chunks = [];
  for (let start = 0; start < n; start += step) {
    const end = Math.min(n, start + window);
    chunks.push({
      path,
      span: [start + 1, end],           // 1-based inclusive
      symbol: null,
      kind: null,
      text: lines.slice(start, end).join('\n'),
    });
    if (end === n) break;
  }
  return chunks;
}

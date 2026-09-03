// Reciprocal Rank Fusion over N ranked lists of keys. Returns [{key, score}] sorted desc,
// score normalized so the top result is 1 (comparable within one response only).
export function rrf(lists, { k = 60 } = {}) {
  const acc = new Map();
  for (const list of lists) {
    list.forEach((key, i) => {
      acc.set(key, (acc.get(key) || 0) + 1 / (k + i + 1));
    });
  }
  const rows = [...acc.entries()].map(([key, s]) => ({ key, score: s }));
  rows.sort((a, b) => b.score - a.score);
  const max = rows.length ? rows[0].score : 1;
  for (const r of rows) r.score = max > 0 ? r.score / max : 0;
  return rows;
}

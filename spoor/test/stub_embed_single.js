import { createServer } from 'node:http';
// Returns a deterministic 4-dim vector per input (length-based) for OpenAI /embeddings.
export function startSingleStub() {
  const server = createServer((req, res) => {
    let body = '';
    req.on('data', c => (body += c));
    req.on('end', () => {
      const { input } = JSON.parse(body);
      const data = input.map((t, i) => ({ embedding: [t.length, i, 1, 0] }));
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ data, model: 'stub', dim: 4 }));
    });
  });
  return new Promise(r => server.listen(0, () => r({ server, url: `http://127.0.0.1:${server.address().port}` })));
}

import { createServer } from 'node:http';
// Returns 2 token-vectors (dim 4) per input for /embed_multivector.
export function startMultiStub() {
  const server = createServer((req, res) => {
    let body = '';
    req.on('data', c => (body += c));
    req.on('end', () => {
      const { inputs } = JSON.parse(body);
      const vectors = inputs.map((t) => [[t.length, 0, 1, 0], [0, t.length, 0, 1]]);
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ vectors, dim: 4, model: 'stub-mv' }));
    });
  });
  return new Promise(r => server.listen(0, () => r({ server, url: `http://127.0.0.1:${server.address().port}` })));
}

export function meanPool(tokens) {
  const dim = tokens[0].length;
  const out = new Array(dim).fill(0);
  for (const t of tokens) for (let i = 0; i < dim; i++) out[i] += t[i];
  for (let i = 0; i < dim; i++) out[i] /= tokens.length;
  return out;
}

export class Embedder {
  constructor({ embedUrl, embedMode, embedModel }) {
    this.url = embedUrl; this.mode = embedMode; this.model = embedModel;
  }

  async embed(texts, role) {
    if (this.mode === 'multi') return this._multi(texts, role);
    return this._single(texts);
  }

  async _single(texts) {
    const body = JSON.stringify({ input: texts, model: this.model });
    const json = await this._post('/v1/embeddings', body);
    const items = json.data.map(d => ({ vector: d.embedding }));
    return { dim: items[0].vector.length, items };
  }

  async _multi(texts, role) {
    const body = JSON.stringify({ inputs: texts, role, model: this.model });
    const json = await this._post('/embed_multivector', body);
    const items = json.vectors.map(tokens => ({ tokens, pooled: meanPool(tokens) }));
    return { dim: json.dim ?? items[0].tokens[0].length, items };
  }

  async _post(path, body) {
    let resp;
    try {
      resp = await fetch(this.url + path, { method: 'POST', headers: { 'content-type': 'application/json' }, body });
    } catch (err) {
      throw new Error(`embed request to EMBED_URL ${this.url}${path} failed: ${err.message}`);
    }
    if (!resp.ok) throw new Error(`embed request to ${this.url}${path} returned ${resp.status}`);
    return resp.json();
  }
}

# spoor-embed

The reference **multi-vector embedding service** for spoor's `multi` mode — a small FastAPI app
serving a ColBERT-class model (GTE-ModernColBERT-v1) that returns per-token vectors over the spoor
wire protocol. Runs on a GPU box (work2, RTX 3060; watch1 as the lighter fallback).

## Endpoints

- `POST /embed_multivector`
  - Request: `{ "inputs": ["text", ...], "role": "doc"|"query", "model"?: "..." }`
  - Response: `{ "vectors": [ [[f, ...], ...], ... ], "dim": <int>, "model": "..." }` — one list of per-token vectors per input.
- `GET /healthz` → `{ "status": "ok", "model": "...", "dim": <int> }`

This matches exactly what spoor's Node embedder client (`spoor/src/embedder.js`, `multi` mode) sends
and parses. If either side changes field names, both must change together.

## Run

**Model-free (contract/dev, any machine — no GPU):**
```bash
pip install -e '.[dev]'
SPOOR_EMBED_FAKE=1 SPOOR_EMBED_DIM=8 PORT=8900 python -m spoor_embed
curl -s localhost:8900/healthz
```

**Real model (on work2 / watch1):**
```bash
pip install -e '.[gpu,dev]'
SPOOR_EMBED_MODEL=lightonai/GTE-ModernColBERT-v1 PORT=8900 python -m spoor_embed
# one-time model verification:
SPOOR_EMBED_REAL=1 python -m pytest tests/test_pylate_smoke.py -q
```

## Env

| Var | Meaning | Default |
|---|---|---|
| `SPOOR_EMBED_FAKE` | `1` → deterministic FakeEncoder (no GPU) | unset |
| `SPOOR_EMBED_MODEL` | ColBERT model id (PyLate-loadable) | `lightonai/GTE-ModernColBERT-v1` |
| `SPOOR_EMBED_DIM` | FakeEncoder dim | `4` |
| `HOST` / `PORT` | bind address | `0.0.0.0` / `8900` |

## Notes

- `torch`/`pylate` live behind the `[gpu]` extra and lazy imports, so the base package + contract
  tests install and pass anywhere; only the GPU boxes need the heavy deps.
- The model is swappable to any PyLate-loadable ColBERT (e.g. `jina-colbert-v2`). The observed
  per-token `dim` (e.g. 128) is written into spoor's index metadata; changing model/dim invalidates
  the index by design.
- Point spoor at it with `EMBED_MODE=multi EMBED_URL=http://<box>:8900`.

# spoor ColBERT Embedding Service Implementation Plan (Plan B)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the reference multi-vector embedding service that spoor's `multi` mode calls — a small FastAPI app serving a ColBERT-class model (GTE-ModernColBERT-v1) that returns per-token vectors over the spoor wire protocol, runnable on the work2 box (RTX 3060).

**Architecture:** A thin FastAPI service with a pluggable `Encoder` interface. A `PyLateEncoder` wraps the real ColBERT model (PyLate/sentence-transformers); a `FakeEncoder` gives deterministic vectors so the HTTP contract is testable with no GPU/model download. The wire protocol matches exactly what the spoor Node embedder client (Plan A, Task 7) sends and parses.

**Tech Stack:** Python 3.11, FastAPI + uvicorn, PyLate (ColBERT models on top of sentence-transformers), PyTorch (CUDA on work2). Tests use FastAPI's `TestClient` with the `FakeEncoder` (no model, no GPU).

**Wire protocol (must match Plan A):**
- Request: `POST /embed_multivector` `{ "inputs": [str, ...], "role": "doc"|"query", "model"?: str }`
- Response: `{ "vectors": [ [[f, ...], ...], ... ], "dim": int, "model": str }` — one list of per-token vectors per input.
- Health: `GET /healthz` → `{ "status": "ok", "model": str, "dim": int }`.

---

## File Structure

```
spoor-embed/
├── pyproject.toml            # deps: fastapi, uvicorn, pydantic; extra [gpu]: pylate, torch
├── spoor_embed/
│   ├── __init__.py
│   ├── encoder.py            # Encoder ABC + FakeEncoder
│   ├── pylate_encoder.py     # PyLateEncoder (real model; imported lazily)
│   ├── app.py                # FastAPI app factory (encoder injected)
│   └── __main__.py           # uvicorn entrypoint; picks encoder from env
└── tests/
    ├── test_contract.py      # HTTP contract via FakeEncoder
    └── test_pylate_smoke.py  # real-model smoke test, gated by SPOOR_EMBED_REAL=1
```

---

## Task 1: Scaffold + Encoder interface + FakeEncoder

**Files:**
- Create: `spoor-embed/pyproject.toml`, `spoor-embed/spoor_embed/__init__.py`, `spoor-embed/spoor_embed/encoder.py`
- Test: `spoor-embed/tests/test_encoder.py`

- [ ] **Step 1: Write `spoor-embed/pyproject.toml`**

```toml
[project]
name = "spoor-embed"
version = "0.0.1"
requires-python = ">=3.11"
dependencies = ["fastapi>=0.111", "uvicorn>=0.30", "pydantic>=2.7"]

[project.optional-dependencies]
gpu = ["pylate>=1.1.0", "torch>=2.3"]
dev = ["pytest>=8", "httpx>=0.27"]

[tool.setuptools.packages.find]
where = ["."]
include = ["spoor_embed*"]
```

- [ ] **Step 2: Write the failing test** `spoor-embed/tests/test_encoder.py`

```python
from spoor_embed.encoder import FakeEncoder

def test_fake_encoder_returns_per_token_vectors():
    enc = FakeEncoder(dim=4)
    out = enc.encode(["abc", "de"], role="doc")
    assert enc.dim == 4
    assert len(out) == 2                 # one list per input
    assert len(out[0]) == len("abc")     # one vector per character (token stand-in)
    assert len(out[0][0]) == 4           # dim
    # deterministic
    assert enc.encode(["abc"], role="doc") == [out[0]]

def test_fake_encoder_role_changes_vectors():
    enc = FakeEncoder(dim=4)
    doc = enc.encode(["abc"], role="doc")
    qry = enc.encode(["abc"], role="query")
    assert doc != qry                    # asymmetric doc/query encoding
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd spoor-embed && python -m pytest tests/test_encoder.py -q`
Expected: FAIL (`ModuleNotFoundError: spoor_embed`).

- [ ] **Step 4: Write `spoor-embed/spoor_embed/__init__.py`** (empty) and **`spoor-embed/spoor_embed/encoder.py`**

```python
from abc import ABC, abstractmethod


class Encoder(ABC):
    dim: int
    model_name: str

    @abstractmethod
    def encode(self, inputs: list[str], role: str) -> list[list[list[float]]]:
        """Return, per input, a list of per-token vectors."""
        raise NotImplementedError


class FakeEncoder(Encoder):
    """Deterministic encoder for contract tests — no model, no GPU.
    Emits one vector per character; role shifts the vectors so doc != query."""

    def __init__(self, dim: int = 4):
        self.dim = dim
        self.model_name = "fake"

    def encode(self, inputs: list[str], role: str) -> list[list[list[float]]]:
        bias = 0.0 if role == "doc" else 0.5
        out = []
        for text in inputs:
            toks = []
            for i, ch in enumerate(text):
                v = [((ord(ch) + i + j) % 7) / 7.0 + bias for j in range(self.dim)]
                toks.append(v)
            if not toks:  # empty input → single zero vector so downstream never sees []
                toks = [[bias] * self.dim]
            out.append(toks)
        return out
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd spoor-embed && python -m pytest tests/test_encoder.py -q`
Expected: PASS (2 tests).

- [ ] **Step 6: Commit**

```bash
git add spoor-embed/pyproject.toml spoor-embed/spoor_embed/__init__.py spoor-embed/spoor_embed/encoder.py spoor-embed/tests/test_encoder.py
git commit -m "feat(spoor-embed): scaffold + Encoder interface + FakeEncoder"
```

---

## Task 2: FastAPI app + wire protocol (contract test)

**Files:**
- Create: `spoor-embed/spoor_embed/app.py`
- Test: `spoor-embed/tests/test_contract.py`

- [ ] **Step 1: Write the failing test** `spoor-embed/tests/test_contract.py`

```python
from fastapi.testclient import TestClient
from spoor_embed.app import create_app
from spoor_embed.encoder import FakeEncoder


def client():
    return TestClient(create_app(FakeEncoder(dim=4)))


def test_embed_multivector_shape():
    c = client()
    r = c.post("/embed_multivector", json={"inputs": ["abc", "de"], "role": "doc"})
    assert r.status_code == 200
    body = r.json()
    assert body["dim"] == 4
    assert body["model"] == "fake"
    assert len(body["vectors"]) == 2
    assert len(body["vectors"][0]) == 3       # 'abc' → 3 token vectors
    assert len(body["vectors"][0][0]) == 4    # dim


def test_role_defaults_to_doc_and_query_differs():
    c = client()
    d = c.post("/embed_multivector", json={"inputs": ["abc"]}).json()["vectors"]
    q = c.post("/embed_multivector", json={"inputs": ["abc"], "role": "query"}).json()["vectors"]
    assert d != q


def test_healthz():
    c = client()
    r = c.get("/healthz")
    assert r.status_code == 200
    assert r.json() == {"status": "ok", "model": "fake", "dim": 4}


def test_empty_inputs_rejected():
    c = client()
    r = c.post("/embed_multivector", json={"inputs": []})
    assert r.status_code == 422
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor-embed && python -m pytest tests/test_contract.py -q`
Expected: FAIL (`ModuleNotFoundError: spoor_embed.app`).

- [ ] **Step 3: Write `spoor-embed/spoor_embed/app.py`**

```python
from fastapi import FastAPI
from pydantic import BaseModel, Field
from spoor_embed.encoder import Encoder


class EmbedRequest(BaseModel):
    inputs: list[str] = Field(min_length=1)
    role: str = "doc"
    model: str | None = None


class EmbedResponse(BaseModel):
    vectors: list[list[list[float]]]
    dim: int
    model: str


def create_app(encoder: Encoder) -> FastAPI:
    app = FastAPI(title="spoor-embed", version="0.0.1")

    @app.get("/healthz")
    def healthz():
        return {"status": "ok", "model": encoder.model_name, "dim": encoder.dim}

    @app.post("/embed_multivector", response_model=EmbedResponse)
    def embed(req: EmbedRequest):
        role = "query" if req.role == "query" else "doc"
        vectors = encoder.encode(req.inputs, role=role)
        return EmbedResponse(vectors=vectors, dim=encoder.dim, model=encoder.model_name)

    return app
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor-embed && python -m pytest tests/test_contract.py -q`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add spoor-embed/spoor_embed/app.py spoor-embed/tests/test_contract.py
git commit -m "feat(spoor-embed): FastAPI app + /embed_multivector wire protocol"
```

---

## Task 3: PyLateEncoder (real ColBERT model)

**Files:**
- Create: `spoor-embed/spoor_embed/pylate_encoder.py`
- Test: `spoor-embed/tests/test_pylate_smoke.py`

- [ ] **Step 1: Write the gated smoke test** `spoor-embed/tests/test_pylate_smoke.py`

```python
import os
import pytest

REAL = os.environ.get("SPOOR_EMBED_REAL") == "1"


@pytest.mark.skipif(not REAL, reason="set SPOOR_EMBED_REAL=1 to run the real-model smoke test on work2")
def test_pylate_encoder_produces_token_vectors():
    from spoor_embed.pylate_encoder import PyLateEncoder
    enc = PyLateEncoder(model_name="lightonai/GTE-ModernColBERT-v1")
    out = enc.encode(["def login(user): return authenticate(user)"], role="doc")
    assert len(out) == 1
    assert len(out[0]) >= 3          # several token vectors
    assert len(out[0][0]) == enc.dim
    q = enc.encode(["login"], role="query")
    assert len(q[0][0]) == enc.dim
```

- [ ] **Step 2: Run test to verify it is skipped (fails only when REAL=1 and module missing)**

Run: `cd spoor-embed && python -m pytest tests/test_pylate_smoke.py -q`
Expected: SKIPPED (1 skipped) when `SPOOR_EMBED_REAL` is unset. This confirms the gate works.

- [ ] **Step 3: Write `spoor-embed/spoor_embed/pylate_encoder.py`**

```python
from spoor_embed.encoder import Encoder


class PyLateEncoder(Encoder):
    """Wraps a ColBERT-class model via PyLate. Imported lazily so the base
    package (and its contract tests) need neither torch nor pylate installed."""

    def __init__(self, model_name: str = "lightonai/GTE-ModernColBERT-v1", device: str | None = None):
        from pylate import models  # lazy: only when the real encoder is constructed
        self.model_name = model_name
        self._model = models.ColBERT(model_name_or_path=model_name, device=device)
        # PyLate ColBERT exposes the per-token embedding dim via the underlying transformer.
        self.dim = int(self._model.get_sentence_embedding_dimension())

    def encode(self, inputs: list[str], role: str) -> list[list[list[float]]]:
        is_query = role == "query"
        # PyLate returns one (num_tokens, dim) array per input when is_query flag matches usage.
        embeddings = self._model.encode(
            inputs,
            is_query=is_query,
            convert_to_numpy=True,
            show_progress_bar=False,
        )
        return [[[float(x) for x in tok] for tok in doc] for doc in embeddings]
```

- [ ] **Step 4 (on work2 only): install GPU extra and run the real smoke test**

Run:
```bash
cd spoor-embed && pip install -e '.[gpu,dev]'
SPOOR_EMBED_REAL=1 python -m pytest tests/test_pylate_smoke.py -q
```
Expected: PASS (model downloads once, runs on CUDA). If PyLate's `encode`/`is_query` signature differs in the installed version, adjust the call to match `pylate`'s current API (verify with `python -c "from pylate import models; help(models.ColBERT.encode)"`) — keep the return-shape conversion identical. Record the observed `dim` (e.g. 128) for the operator docs.

- [ ] **Step 5: Commit**

```bash
git add spoor-embed/spoor_embed/pylate_encoder.py spoor-embed/tests/test_pylate_smoke.py
git commit -m "feat(spoor-embed): PyLateEncoder wrapping GTE-ModernColBERT-v1"
```

---

## Task 4: Server entrypoint (encoder selected by env)

**Files:**
- Create: `spoor-embed/spoor_embed/__main__.py`
- Test: `spoor-embed/tests/test_main_factory.py`

- [ ] **Step 1: Write the failing test** `spoor-embed/tests/test_main_factory.py`

```python
from spoor_embed.__main__ import build_encoder


def test_build_encoder_fake_by_default(monkeypatch):
    monkeypatch.delenv("SPOOR_EMBED_MODEL", raising=False)
    monkeypatch.setenv("SPOOR_EMBED_FAKE", "1")
    enc = build_encoder()
    assert enc.model_name == "fake"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spoor-embed && python -m pytest tests/test_main_factory.py -q`
Expected: FAIL (module/function not found).

- [ ] **Step 3: Write `spoor-embed/spoor_embed/__main__.py`**

```python
import os
from spoor_embed.encoder import FakeEncoder


def build_encoder():
    if os.environ.get("SPOOR_EMBED_FAKE") == "1":
        return FakeEncoder(dim=int(os.environ.get("SPOOR_EMBED_DIM", "4")))
    from spoor_embed.pylate_encoder import PyLateEncoder  # lazy: needs gpu extra
    return PyLateEncoder(model_name=os.environ.get("SPOOR_EMBED_MODEL", "lightonai/GTE-ModernColBERT-v1"))


def main():
    import uvicorn
    from spoor_embed.app import create_app
    app = create_app(build_encoder())
    uvicorn.run(app, host=os.environ.get("HOST", "0.0.0.0"), port=int(os.environ.get("PORT", "8900")))


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spoor-embed && python -m pytest tests/test_main_factory.py -q`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add spoor-embed/spoor_embed/__main__.py spoor-embed/tests/test_main_factory.py
git commit -m "feat(spoor-embed): env-selected encoder + uvicorn entrypoint"
```

---

## Task 5: Cross-plan integration check (Plan A ⇄ Plan B)

**Files:**
- Create: `spoor-embed/README.md`

- [ ] **Step 1: Start the fake service and point Plan A's `multi` mode at it**

Run:
```bash
# terminal 1: the service with the fake encoder (no GPU needed)
cd spoor-embed && SPOOR_EMBED_FAKE=1 SPOOR_EMBED_DIM=8 PORT=8900 python -m spoor_embed &
sleep 1 && curl -s localhost:8900/healthz
# terminal 2: drive spoor multi mode against it on a tiny repo
cd /tmp && rm -rf zrepo && mkdir zrepo && printf 'def login(u):\n    return 1\n' > zrepo/a.py
cd /data/tyolab/code/antelope/spoor
SPOOR_INDEX_DIR=/tmp/zrepo/.spoor EMBED_URL=http://localhost:8900 EMBED_MODE=multi EMBED_MODEL=fake \
  node src/cli.js index --pq
SPOOR_INDEX_DIR=/tmp/zrepo/.spoor EMBED_URL=http://localhost:8900 EMBED_MODE=multi EMBED_MODEL=fake \
  node src/cli.js query "login" --k 3
```
Expected: `/healthz` returns `{"status":"ok","model":"fake","dim":8}`; `index` prints a JSON summary with `chunks>=1`; `query` prints results containing `a.py` and a `span`. This proves the wire protocol matches end-to-end (Plan A embedder client ⇄ Plan B service) and that the `--pq` (residentTier NONE) path builds.

- [ ] **Step 2: Write `spoor-embed/README.md`**

Document: purpose; endpoints (`/embed_multivector`, `/healthz`) and exact request/response JSON; env (`SPOOR_EMBED_FAKE`, `SPOOR_EMBED_MODEL`, `SPOOR_EMBED_DIM`, `HOST`, `PORT`); how to run on work2 (`pip install -e '.[gpu]'`; `SPOOR_EMBED_MODEL=lightonai/GTE-ModernColBERT-v1 python -m spoor_embed`); the observed `dim` from Task 3; and that watch1 is the lighter fallback host. Note the model choice is swappable (any PyLate-loadable ColBERT).

- [ ] **Step 3: Commit**

```bash
git add spoor-embed/README.md
git commit -m "docs(spoor-embed): operator README + cross-plan integration recipe"
```

---

## Self-Review Notes (for the implementer)

- **Spec coverage:** the multi-vector wire protocol (spec §5) is implemented exactly and cross-checked against Plan A's client (Task 5). The reference server (FastAPI + PyLate + GTE-ModernColBERT-v1) matches spec §5. Health + empty-input validation are contract-tested with the FakeEncoder (no GPU) so CI is model-free.
- **Contract match with Plan A:** request `{inputs, role}` → response `{vectors, dim, model}` mirrors `spoor/src/embedder.js` `_multi()` exactly. If either side changes the field names, both must change together.
- **Verification points (real, in-task — not placeholders):** PyLate `encode(is_query=...)` signature (Task 3 Step 4 — verify against the installed version); observed embedding `dim` recorded for operator docs.
- **GPU isolation:** torch/pylate live behind the `[gpu]` extra and lazy imports, so the base package + contract tests install and pass anywhere; only work2/watch1 need the heavy deps.
```

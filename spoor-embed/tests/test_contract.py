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

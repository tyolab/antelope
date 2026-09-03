from spoor_embed.__main__ import build_encoder


def test_build_encoder_fake_by_default(monkeypatch):
    monkeypatch.delenv("SPOOR_EMBED_MODEL", raising=False)
    monkeypatch.setenv("SPOOR_EMBED_FAKE", "1")
    enc = build_encoder()
    assert enc.model_name == "fake"

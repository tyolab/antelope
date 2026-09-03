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

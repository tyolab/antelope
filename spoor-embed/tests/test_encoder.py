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

import tempfile, pytest, antelope


def _dense_docs(ix, dim, n=40):
    import random
    random.seed(1)
    for i in range(n):
        v = [random.uniform(-1, 1) for _ in range(dim)]
        ix.add_document(f"d{i}", "<DOC>apple</DOC>", vector=v)


def test_pq_replace_build_and_search():
    dim = 8
    with antelope.SegmentIndex(dimension=dim, metric="cosine", pq={"m": 0, "posture": "replace"}) as ix:
        ix.open(tempfile.mkdtemp())
        _dense_docs(ix, dim)
        ix.flush()
        ix.build_pq()
        q = [1.0] + [0.0] * (dim - 1)
        assert len(ix.search_vector(q, 5)) >= 1


def test_pq_rerank_tier_int8():
    dim = 8
    with antelope.SegmentIndex(dimension=dim, metric="cosine",
                               pq={"posture": "rerank", "resident_tier": "int8"}) as ix:
        ix.open(tempfile.mkdtemp())
        _dense_docs(ix, dim)
        ix.flush()
        ix.build_pq()
        assert len(ix.search_vector([1.0] + [0.0] * (dim - 1), 5)) >= 1


def test_multivector_pq_build_and_rerank():
    dim = 4
    with antelope.SegmentIndex(dimension=dim, metric="cosine",
                               rerank={"dimension": dim}, multivector_pq={"m": 0}) as ix:
        ix.open(tempfile.mkdtemp())
        for i in range(12):
            v = [i + 1, 1, 0, 0]
            ix.add_document(f"d{i}", "<DOC>apple</DOC>", vector=v, multi_vectors=[v])
        ix.flush()
        ix.build_multivector_pq()
        q = [1, 1, 0, 0]
        assert len(ix.search_rerank("apple", q, [q], first_stage_n=20, k=5)) >= 1


def test_pq_config_persists_on_reopen():
    dim = 8
    d = tempfile.mkdtemp()
    with antelope.SegmentIndex(dimension=dim, metric="cosine", pq={"m": 0, "posture": "replace"}) as ix:
        ix.open(d)
        _dense_docs(ix, dim)
        ix.flush()
        ix.build_pq()
    with antelope.SegmentIndex(dimension=dim, metric="cosine", pq={"m": 0, "posture": "replace"}) as ix2:
        ix2.open(d)
        assert len(ix2.search_vector([1.0] + [0.0] * (dim - 1), 5)) >= 1


def test_pq_mutually_exclusive_with_int8_quantize_no_throw():
    dim = 8
    with antelope.SegmentIndex(dimension=dim, metric="cosine", quantize="int8", pq={"posture": "replace"}) as ix:
        ix.open(tempfile.mkdtemp())
        _dense_docs(ix, dim)
        ix.flush()
        assert len(ix.search_vector([1.0] + [0.0] * (dim - 1), 5)) >= 1


def test_pq_bad_posture_raises():
    with pytest.raises(ValueError):
        antelope.SegmentIndex(dimension=8, metric="cosine", pq={"posture": "bogus"})


def test_multivector_pq_conflicts_with_explicit_int8_rerank():
    dim = 4
    # explicit int8 .mvec + multivectorPq: token-PQ stays OFF (mutually exclusive), no throw at open;
    # build_multivector_pq() then reports it's not configured, and int8 rerank still works.
    with antelope.SegmentIndex(dimension=dim, metric="cosine",
                               rerank={"dimension": dim, "quantize": "int8"},
                               multivector_pq={"m": 0}) as ix:
        ix.open(tempfile.mkdtemp())
        for i in range(12):
            v = [i + 1, 1, 0, 0]
            ix.add_document(f"d{i}", "<DOC>apple</DOC>", vector=v, multi_vectors=[v])
        ix.flush()
        with pytest.raises(RuntimeError):
            ix.build_multivector_pq()               # token-PQ not configured (int8 won)
        q = [1, 1, 0, 0]
        assert len(ix.search_rerank("apple", q, [q], first_stage_n=20, k=5)) >= 1  # int8 rerank still works

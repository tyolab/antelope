import tempfile, antelope

def test_approx_and_hnsw_modes():
    with antelope.SegmentIndex(dimension=8, metric="cosine",
                               approximate={"bits": 64},
                               hnsw={"M": 16, "ef_construction": 100}) as ix:
        ix.open(tempfile.mkdtemp())
        import random
        random.seed(3)
        for i in range(30):
            v = [random.uniform(-1, 1) for _ in range(8)]
            ix.add_document(f"d{i}", "<DOC>apple</DOC>", vector=v)
        ix.flush()
        q = [random.uniform(-1, 1) for _ in range(8)]
        assert len(ix.search_vector_approx(q, 10)) >= 1
        assert len(ix.search_vector_hnsw(q, 10)) >= 1
        assert len(ix.search_hybrid_approx("apple", q, 10)) >= 1
        assert len(ix.search_hybrid_hnsw("apple", q, 10)) >= 1

def test_builders_run():
    with antelope.SegmentIndex(dimension=4, metric="cosine",
                               approximate={"bits": 32}, hnsw={"M": 8, "ef_construction": 50}) as ix:
        ix.open(tempfile.mkdtemp())
        for i in range(10):
            ix.add_document(f"d{i}", "<DOC>x</DOC>", vector=[i+1, 1, 0, 0])
        ix.flush()
        ix.build_signatures()   # no raise
        ix.build_hnsw()         # no raise

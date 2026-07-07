import tempfile, pytest, antelope

def test_rerank_search():
    with antelope.SegmentIndex(dimension=4, metric="cosine",
                               rerank={"dimension": 4}, quantize="replace") as ix:
        ix.open(tempfile.mkdtemp())
        for i in range(12):
            v = [i + 1, 1, 0, 0]
            ix.add_document(f"d{i}", "<DOC>apple</DOC>", vector=v, multi_vectors=[v])
        ix.flush()
        q = [1, 1, 0, 0]
        hits = ix.search_rerank("apple", q, [q], first_stage_n=20, k=5)
        assert len(hits) >= 1
        ix.build_quantized()   # no raise

def test_rerank_both_none_raises():
    with antelope.SegmentIndex(dimension=4, metric="cosine", rerank={"dimension": 4}) as ix:
        ix.open(tempfile.mkdtemp())
        with pytest.raises(ValueError):
            ix.search_rerank(None, None, [[1, 1, 0, 0]], first_stage_n=10, k=5)

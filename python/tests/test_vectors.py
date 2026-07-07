import array, tempfile, pytest, antelope

def test_vector_and_hybrid():
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("near", "<DOC>x</DOC>", vector=[1, 0, 0, 0])
        ix.add_document("far",  "<DOC>y</DOC>", vector=array.array("f", [0.1, 0, 0, 0]))
        hv = ix.search_vector([1, 0, 0, 0], 2)
        assert len(hv) == 2 and hv[0].key == "near"
        hh = ix.search_hybrid("x", [1, 0, 0, 0], 2)
        assert len(hh) >= 1

def test_numpy_vector_if_available():
    np = pytest.importorskip("numpy")
    with antelope.SegmentIndex(dimension=3, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("d", "<DOC>x</DOC>", vector=np.array([1, 2, 3], dtype="float32"))
        assert ix.search_vector(np.array([1, 2, 3], dtype="float32"), 1)[0].key == "d"

def test_vector_dim_mismatch_raises():
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        with pytest.raises(ValueError):
            ix.add_document("d", "<DOC>x</DOC>", vector=[1, 0, 0])   # 3 != 4

def test_cosine_zero_vector_raises():
    with antelope.SegmentIndex(dimension=4, metric="cosine") as ix:
        ix.open(tempfile.mkdtemp())
        with pytest.raises(ValueError):
            ix.add_document("d", "<DOC>x</DOC>", vector=[0, 0, 0, 0])

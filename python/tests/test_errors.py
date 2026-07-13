"""
Error-mapping regression lock (Task 10, Layer 1).

Verifies the C++ -> Python exception-type contract documented in the
python-binding spec (Sec. 5):

    open() failure (bad/corrupt dir, config mismatch)      -> RuntimeError
    bad constructor options / bad metric / wrong vector
        length / zero cosine vector                        -> ValueError
    malformed/mis-typed filter (unknown field, type
        mismatch, bad predicate)                            -> TypeError
    search_rerank with neither text nor vector              -> ValueError
    any op before open() / after close()                    -> RuntimeError
"""
import tempfile
import pytest
import antelope


def test_bad_metric_raises_valueerror():
    with pytest.raises(ValueError):
        antelope.SegmentIndex(dimension=4, metric="xyz")


def test_open_bad_directory_raises_runtimeerror():
    # A regular FILE cannot be open()ed as an index directory -- manifest/keymap
    # writes underneath it fail, so this reliably exercises the RuntimeError
    # contract (unlike a merely-missing path, which some implementations may
    # create on demand).
    with tempfile.NamedTemporaryFile() as f:
        ix = antelope.SegmentIndex(dimension=4, metric="dot")
        with pytest.raises(RuntimeError):
            ix.open(f.name)


def test_wrong_length_vector_raises_valueerror():
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        with pytest.raises(ValueError):
            ix.add_document("d0", "<DOC>x</DOC>", vector=[1, 0, 0])   # dim=4, gave 3


def test_op_after_close_raises_runtimeerror():
    ix = antelope.SegmentIndex(dimension=4, metric="dot")
    ix.open(tempfile.mkdtemp())
    ix.close()
    with pytest.raises(RuntimeError):
        ix.search("x", 5)


def test_op_before_open_raises_runtimeerror():
    ix = antelope.SegmentIndex(dimension=4, metric="dot")
    with pytest.raises(RuntimeError):
        ix.document_count()


def test_rerank_without_inputs_raises_valueerror():
    with antelope.SegmentIndex(dimension=4, metric="dot", rerank={"dimension": 4}) as ix:
        ix.open(tempfile.mkdtemp())
        with pytest.raises(ValueError):
            ix.search_rerank(None, None, None, 10, 5)


def test_bad_filter_raises_typeerror():
    with antelope.SegmentIndex(dimension=4, metric="dot", attributes={"t": "string"}) as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("d0", "<DOC>alpha</DOC>", vector=[1, 0, 0, 0], attributes={"t": "v"})
        ix.flush()
        with pytest.raises(TypeError):
            ix.search("alpha", 5, filter={"eq": {"unknown_field": "v"}})


def test_multivectors_without_rerank_raises_valueerror():
    import tempfile, pytest, antelope
    # dense vectors enabled, but rerank (multi-vector) support is NOT configured
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        with pytest.raises(ValueError) as e1:
            ix.add_document("d0", "<DOC>x</DOC>", vector=[1, 0, 0, 0], multi_vectors=[[1, 0, 0, 0]])
        assert "rerank is not configured" in str(e1.value)
        with pytest.raises(ValueError) as e2:
            ix.search_rerank("x", [1, 0, 0, 0], [[1, 0, 0, 0]], first_stage_n=10, k=5)
        assert "rerank is not configured" in str(e2.value)

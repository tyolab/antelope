import tempfile, pytest, antelope

def test_open_close_counts():
    ix = antelope.SegmentIndex()
    ix.open(tempfile.mkdtemp())
    assert ix.document_count() == 0
    assert ix.vector_dimension() == 0
    ix.close()

def test_context_manager_and_dim():
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        assert ix.vector_dimension() == 4

def test_op_before_open_raises():
    ix = antelope.SegmentIndex()
    with pytest.raises(RuntimeError):
        ix.document_count()

def test_bad_metric_raises():
    with pytest.raises(ValueError):
        antelope.SegmentIndex(dimension=4, metric="nope")

def test_bad_attribute_schema_raises():
    with pytest.raises(ValueError):
        antelope.SegmentIndex(attributes={"b": "bool[]"})   # multi-valued bool is illegal

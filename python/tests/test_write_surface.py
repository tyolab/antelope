import tempfile, pytest, antelope

def test_update_document_changes_content():
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("d0", "<DOC>alpha</DOC>", vector=[1,0,0,0])
        ix.flush()
        assert [h.key for h in ix.search("alpha", 5)] == ["d0"]
        ix.update_document("d0", "<DOC>beta</DOC>", vector=[0,1,0,0])
        ix.flush()
        assert [h.key for h in ix.search("beta", 5)] == ["d0"]
        assert ix.search("alpha", 5) == []          # old term gone after upsert

def test_update_returns_handle():
    with antelope.SegmentIndex() as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("d0", "<DOC>x</DOC>")
        h = ix.update_document("d0", "<DOC>y</DOC>")
        assert isinstance(h, dict) and "generation" in h and "docid" in h

def test_delete_document():
    with antelope.SegmentIndex() as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("d0", "<DOC>alpha</DOC>")
        ix.add_document("d1", "<DOC>alpha</DOC>")
        ix.flush()
        assert ix.delete_document("d0") is True
        assert ix.delete_document("nope") is False   # unknown key
        ix.flush()
        assert [h.key for h in ix.search("alpha", 5)] == ["d1"]

def test_maintain_runs():
    with antelope.SegmentIndex() as ix:
        ix.open(tempfile.mkdtemp())
        for i in range(5):
            ix.add_document(f"d{i}", "<DOC>alpha</DOC>")
            ix.flush()
        ix.maintain()                                 # should not raise
        assert ix.document_count() == 5

def test_flush_reopen_persists():
    d = tempfile.mkdtemp()
    ix = antelope.SegmentIndex(dimension=4, metric="dot",
                               attributes={"tenant":"string"})
    ix.open(d)
    ix.add_document("d0", "<DOC>alpha</DOC>", vector=[1,0,0,0],
                    attributes={"tenant":"acme"}, payload=b"hello")
    ix.flush()
    ix.close()
    # reopen a fresh handle on the same directory
    ix2 = antelope.SegmentIndex(dimension=4, metric="dot",
                                attributes={"tenant":"string"})
    ix2.open(d)
    hits = ix2.search("alpha", 5, filter={"eq":{"tenant":"acme"}})
    assert [h.key for h in hits] == ["d0"]
    assert hits[0].payload == b"hello"
    ix2.close()

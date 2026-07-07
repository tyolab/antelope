import tempfile, antelope

def test_lexical_add_search():
    with antelope.SegmentIndex() as ix:
        ix.open(tempfile.mkdtemp())
        h = ix.add_document("doc1", "<DOC>alpha beta</DOC>")
        assert h["generation"] >= 1 and h["docid"] >= 0
        ix.add_document("doc2", "<DOC>gamma</DOC>")
        hits = ix.search("alpha", 10)
        assert len(hits) == 1
        assert hits[0].key == "doc1"
        assert isinstance(hits[0].score, float)
        assert hits[0].payload is None
        assert hits[0].generation >= 1 and hits[0].docid >= 0

def test_search_no_match_empty():
    with antelope.SegmentIndex() as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("doc1", "<DOC>alpha</DOC>")
        assert ix.search("zzzznomatch", 10) == []

import tempfile, pytest, antelope

def _idx():
    ix = antelope.SegmentIndex(dimension=4, metric="dot",
            attributes={"tenant":"string","lang":"string[]","rank":"int64","keep":"bool"})
    ix.open(tempfile.mkdtemp())
    ix.add_document("a","<DOC>alpha</DOC>", vector=[1,0,0,0], attributes={"tenant":"acme","lang":["en"],"rank":10,"keep":True})
    ix.add_document("b","<DOC>alpha</DOC>", vector=[1,0,0,0], attributes={"tenant":"beta","lang":["fr"],"rank":20,"keep":False})
    ix.add_document("c","<DOC>alpha</DOC>", vector=[1,0,0,0], attributes={"tenant":"acme","lang":["en","fr"],"rank":30,"keep":True})
    ix.flush()
    return ix

def _keys(hits): return sorted(h.key for h in hits)

def test_eq_string():
    ix = _idx()
    assert _keys(ix.search("alpha", 10, filter={"eq":{"tenant":"acme"}})) == ["a","c"]

def test_eq_bool_and_int():
    ix = _idx()
    assert _keys(ix.search("alpha", 10, filter={"eq":{"keep":True}})) == ["a","c"]
    assert _keys(ix.search("alpha", 10, filter={"eq":{"rank":20}})) == ["b"]

def test_range_int():
    ix = _idx()
    assert _keys(ix.search("alpha", 10, filter={"range":{"rank":{"gte":20}}})) == ["b","c"]
    assert _keys(ix.search("alpha", 10, filter={"range":{"rank":{"gt":10,"lte":30}}})) == ["b","c"]

def test_in_and_multi_contains():
    ix = _idx()
    assert _keys(ix.search("alpha", 10, filter={"in":{"tenant":["beta","gamma"]}})) == ["b"]
    assert _keys(ix.search("alpha", 10, filter={"eq":{"lang":"fr"}})) == ["b","c"]  # CONTAINS on multi

def test_and_or_not():
    ix = _idx()
    assert _keys(ix.search("alpha", 10, filter={"and":[{"eq":{"tenant":"acme"}},{"eq":{"keep":True}}]})) == ["a","c"]
    assert _keys(ix.search("alpha", 10, filter={"or":[{"eq":{"tenant":"beta"}},{"eq":{"rank":10}}]})) == ["a","b"]
    assert _keys(ix.search("alpha", 10, filter={"not":{"eq":{"tenant":"acme"}}})) == ["b"]

def test_filter_on_vector_search():
    ix = _idx()
    assert _keys(ix.search_vector([1,0,0,0], 10, filter={"eq":{"tenant":"acme"}})) == ["a","c"]

def test_bad_filter_raises_typeerror():
    ix = _idx()
    with pytest.raises(TypeError): ix.search("alpha", 10, filter={"eq":{"nope":"x"}})       # unknown field
    with pytest.raises(TypeError): ix.search("alpha", 10, filter={"eq":{"rank":"notint"}})  # wrong type
    with pytest.raises(TypeError): ix.search("alpha", 10, filter={"and":{"eq":{"rank":10}}})# and needs list
    with pytest.raises(TypeError): ix.search("alpha", 10, filter={"bogus":{}})              # unknown op
    with pytest.raises(TypeError): ix.search("alpha", 10, filter={"range":{"tenant":{"gte":1}}}) # range on string

def test_filter_without_schema_raises():
    ix = antelope.SegmentIndex(dimension=4, metric="dot")  # no attributes
    ix.open(tempfile.mkdtemp())
    ix.add_document("x","<DOC>alpha</DOC>", vector=[1,0,0,0]); ix.flush()
    with pytest.raises(TypeError): ix.search("alpha", 10, filter={"eq":{"t":"v"}})

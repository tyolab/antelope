import tempfile, pytest, antelope

def test_attributes_and_payload():
    with antelope.SegmentIndex(dimension=4, metric="dot",
                               attributes={"tenant":"string","lang":"string[]","rank":"int64","keep":"bool"}) as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("d0","<DOC>x</DOC>", vector=[1,0,0,0],
                        attributes={"tenant":"acme","lang":["en","fr"],"rank":5,"keep":True},
                        payload=b"hello")
        ix.add_document("d1","<DOC>y</DOC>", vector=[0,1,0,0])  # no attrs/payload -> fine
        ix.flush()
        hits = ix.search_vector([1,0,0,0], 5)
        top = [h for h in hits if h.key == "d0"][0]
        assert top.payload == b"hello"
        d1 = [h for h in hits if h.key == "d1"][0]
        assert d1.payload is None

def test_str_payload():
    with antelope.SegmentIndex(attributes={"t":"string"}) as ix:
        ix.open(tempfile.mkdtemp())
        ix.add_document("d","<DOC>x</DOC>", attributes={"t":"v"}, payload="plain")
        ix.flush()
        assert ix.search("x", 5)[0].payload == b"plain"

def test_bad_attribute_type_raises():
    with antelope.SegmentIndex(attributes={"tenant":"string"}) as ix:
        ix.open(tempfile.mkdtemp())
        with pytest.raises(TypeError):
            ix.add_document("d","<DOC>x</DOC>", attributes={"tenant":123})   # int for string field
        with pytest.raises(TypeError):
            ix.add_document("d2","<DOC>x</DOC>", attributes={"nope":"v"})    # unknown field

def test_attributes_without_schema_raises():
    with antelope.SegmentIndex() as ix:   # no attributes schema
        ix.open(tempfile.mkdtemp())
        with pytest.raises(ValueError):
            ix.add_document("d","<DOC>x</DOC>", attributes={"t":"v"})

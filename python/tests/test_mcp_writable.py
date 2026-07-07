import asyncio, tempfile, antelope, pytest
from antelope.mcp.server import build_server

MUT = {"add_document","update_document","delete_document","flush","maintain"}

def _seed(d):
    with antelope.SegmentIndex() as ix:
        ix.open(d); ix.add_document("d0","<DOC>apple</DOC>"); ix.flush()

def _tool_names(srv):
    return {t.name for t in srv.mcp._tool_manager.list_tools()}

def test_readonly_hides_mutation_tools():
    d = tempfile.mkdtemp(); _seed(d)
    srv = build_server(index_dir=d, writable=False)
    names = _tool_names(srv)
    assert "search" in names and "document_count" in names and "index_info" in names
    assert MUT.isdisjoint(names)                       # none registered
    with pytest.raises(KeyError):
        srv.call_tool_for_test("add_document", {"key":"x","text":"<DOC>y</DOC>"})

def test_writable_exposes_mutation_tools():
    d = tempfile.mkdtemp(); _seed(d)
    srv = build_server(index_dir=d, writable=True)
    names = _tool_names(srv)
    assert MUT.issubset(names)

def test_writable_add_then_search_reflects():
    d = tempfile.mkdtemp(); _seed(d)
    srv = build_server(index_dir=d, writable=True)
    h = asyncio.run(srv.call_tool_for_test("add_document",
            {"key":"d1","text":"<DOC>banana</DOC>"}))
    assert "generation" in h and "docid" in h
    asyncio.run(srv.call_tool_for_test("flush", {}))
    hits = asyncio.run(srv.call_tool_for_test("search", {"query":"banana","k":5}))
    assert [x["key"] for x in hits] == ["d1"]
    assert asyncio.run(srv.call_tool_for_test("document_count", {})) == 2

def test_writable_delete_and_attributes():
    d = tempfile.mkdtemp()
    with antelope.SegmentIndex(attributes={"tenant":"string"}) as ix:
        ix.open(d); ix.add_document("d0","<DOC>apple</DOC>", attributes={"tenant":"acme"}); ix.flush()
    srv = build_server(index_dir=d, writable=True)
    asyncio.run(srv.call_tool_for_test("add_document",
        {"key":"d1","text":"<DOC>apple</DOC>","attributes":{"tenant":"beta"},"payload":"p1"}))
    asyncio.run(srv.call_tool_for_test("flush", {}))
    ok = asyncio.run(srv.call_tool_for_test("delete_document", {"key":"d0"}))
    assert ok is True
    asyncio.run(srv.call_tool_for_test("flush", {}))
    hits = asyncio.run(srv.call_tool_for_test("search",
        {"query":"apple","k":5,"filter":{"eq":{"tenant":"beta"}}}))
    assert [x["key"] for x in hits] == ["d1"]
    assert hits[0].get("payload") == "p1"

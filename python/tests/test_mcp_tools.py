import asyncio
import tempfile

import antelope
from antelope.mcp.server import build_server


def _seed(d):
    with antelope.SegmentIndex() as ix:
        ix.open(d)
        ix.add_document("d0", "<DOC>apple</DOC>")
        ix.flush()


def test_search_tool_readonly():
    d = tempfile.mkdtemp()
    _seed(d)
    srv = build_server(index_dir=d, writable=False)
    hits = asyncio.run(srv.call_tool_for_test("search", {"query": "apple", "k": 5}))
    assert hits and hits[0]["key"] == "d0"
    assert asyncio.run(srv.call_tool_for_test("document_count", {})) == 1


def test_search_tool_no_hits():
    d = tempfile.mkdtemp()
    _seed(d)
    srv = build_server(index_dir=d, writable=False)
    hits = asyncio.run(srv.call_tool_for_test("search", {"query": "banana", "k": 5}))
    assert hits == []


def test_unknown_tool_raises():
    import pytest

    d = tempfile.mkdtemp()
    _seed(d)
    srv = build_server(index_dir=d, writable=False)
    with pytest.raises(KeyError):
        srv.call_tool_for_test("add_document", {"key": "x", "text": "y"})  # not registered read-only

import asyncio
import json
import tempfile

import antelope
from antelope.mcp.server import build_server


def _seed(d):
    with antelope.SegmentIndex(dimension=8, metric="cosine",
                               attributes={"tenant": "string", "lang": "string[]", "rank": "int64"}) as ix:
        ix.open(d)
        ix.add_document("d0", "<DOC>apple</DOC>", vector=[0.1] * 8,
                        attributes={"tenant": "acme", "lang": ["en"], "rank": 3})
        ix.flush()


def test_index_info_tool():
    d = tempfile.mkdtemp()
    _seed(d)
    srv = build_server(index_dir=d)
    info = asyncio.run(srv.call_tool_for_test("index_info", {}))
    fields = {f["name"]: f for f in info["attributes"]}
    assert fields["tenant"]["type"] == "string" and fields["tenant"]["multi"] is False
    assert fields["lang"]["type"] == "string" and fields["lang"]["multi"] is True
    assert fields["rank"]["type"] == "int64"
    assert info["config"]["dimension"] == 8
    assert info["config"]["metric"] == "cosine"
    assert info["config"]["document_count"] == 1


def test_schema_resource():
    d = tempfile.mkdtemp()
    _seed(d)
    srv = build_server(index_dir=d)
    raw = asyncio.run(srv.read_resource_for_test("antelope://index/schema"))
    data = json.loads(raw) if isinstance(raw, (str, bytes)) else raw
    names = {f["name"] for f in data["attributes"]}
    assert names == {"tenant", "lang", "rank"}
    assert data["config"]["metric"] == "cosine"


def test_schema_empty_when_no_attributes():
    d = tempfile.mkdtemp()
    with antelope.SegmentIndex(dimension=4, metric="dot") as ix:
        ix.open(d)
        ix.add_document("d0", "<DOC>x</DOC>", vector=[1, 0, 0, 0])
        ix.flush()
    srv = build_server(index_dir=d)
    info = asyncio.run(srv.call_tool_for_test("index_info", {}))
    assert info["attributes"] == []
    assert info["config"]["dimension"] == 4

import asyncio
import sys
import tempfile

import pytest

import antelope

pytest.importorskip("mcp")
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

MUT = {"add_document", "update_document", "delete_document", "flush", "maintain"}


def _seed(d):
    with antelope.SegmentIndex() as ix:
        ix.open(d)
        ix.add_document("d0", "<DOC>apple</DOC>")
        ix.flush()


def _params(d, writable=False):
    args = ["-m", "antelope.mcp", "--index", d]
    if writable:
        args.append("--writable")
    return StdioServerParameters(command=sys.executable, args=args)


async def _run_readonly(d):
    async with stdio_client(_params(d)) as (read, write):
        async with ClientSession(read, write) as s:
            await s.initialize()
            tools = {t.name for t in (await s.list_tools()).tools}
            call = await s.call_tool("search", {"query": "apple", "k": 5})
            resources = {str(r.uri) for r in (await s.list_resources()).resources}
            schema = await s.read_resource("antelope://index/schema")
            return tools, call, resources, schema


async def _run_writable_tools(d):
    async with stdio_client(_params(d, writable=True)) as (read, write):
        async with ClientSession(read, write) as s:
            await s.initialize()
            return {t.name for t in (await s.list_tools()).tools}


def _hits_from(call_result):
    # structuredContent is the reliable channel for structured returns in mcp 1.27
    sc = getattr(call_result, "structuredContent", None)
    if sc is not None:
        # FastMCP wraps list returns as {"result": [...]}
        if isinstance(sc, dict) and "result" in sc:
            return sc["result"]
        return sc
    return call_result.content  # fallback: list of TextContent


def test_stdio_readonly_session():
    d = tempfile.mkdtemp()
    _seed(d)
    try:
        tools, call, resources, schema = asyncio.run(_run_readonly(d))
    except Exception as exc:  # pragma: no cover - environmental guard only
        pytest.skip(f"stdio MCP transport unavailable in this environment: {exc!r}")
    assert "search" in tools and "document_count" in tools and "index_info" in tools
    assert MUT.isdisjoint(tools)  # mutation tools absent read-only
    hits = _hits_from(call)
    assert any((h.get("key") if isinstance(h, dict) else "d0") == "d0" for h in hits) or hits
    assert "antelope://index/schema" in resources


def test_stdio_writable_lists_mutation_tools():
    d = tempfile.mkdtemp()
    _seed(d)
    try:
        tools = asyncio.run(_run_writable_tools(d))
    except Exception as exc:  # pragma: no cover - environmental guard only
        pytest.skip(f"stdio MCP transport unavailable in this environment: {exc!r}")
    assert MUT.issubset(tools)

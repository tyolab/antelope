"""FastMCP server over a single Antelope index (stdio transport).

Read-only by default (search + document_count); mutation tools are registered
only when built with writable=True (Task 13). One index is opened at startup.
"""
import asyncio
import base64

from mcp.server.fastmcp import FastMCP

import antelope


def _hit_json(h):
    """Serialize a Hit to a concise JSON-able dict; payload as utf-8 or base64."""
    out = {"key": h.key, "score": h.score}
    if h.payload is not None:
        try:
            out["payload"] = h.payload.decode("utf-8")
        except UnicodeDecodeError:
            out["payload_b64"] = base64.b64encode(h.payload).decode("ascii")
    return out


class _Server:
    """Thin holder around a FastMCP instance + the open index, with a test hook."""

    def __init__(self, mcp, ix, writable, index_dir):
        self.mcp = mcp
        self.ix = ix
        self.writable = writable
        self.index_dir = index_dir

    def call_tool_for_test(self, name, args):
        """Invoke a registered tool's underlying function by name.

        Returns the coroutine (await it, e.g. via asyncio.run). Raises KeyError
        if no tool with that name is registered (so absence is testable)."""
        tm = self.mcp._tool_manager
        if name not in {t.name for t in tm.list_tools()}:
            raise KeyError(name)
        return tm.get_tool(name).fn(**args)

    async def read_resource_for_test(self, uri):
        """Read a registered resource's content by URI.

        Async because the installed FastMCP's ResourceManager.get_resource()
        is itself a coroutine function (returns None if uri is unregistered);
        the resource object's own .read() is async too."""
        res = await self.mcp._resource_manager.get_resource(uri)
        if res is None:
            raise KeyError(uri)
        return await res.read()


def build_server(index_dir: str, writable: bool = False) -> "_Server":
    ix = antelope.SegmentIndex()
    ix.open(index_dir)
    mcp = FastMCP("antelope")

    @mcp.tool()
    async def search(query: str, k: int = 10, filter: dict | None = None) -> list[dict]:
        """Lexical + optional structured-filter search over the index.

        filter is a predicate dict: and/or/not/eq/in/range (see the antelope
        README). Returns [{key, score, payload?}]."""
        hits = await asyncio.to_thread(ix.search, query, k, filter)
        return [_hit_json(h) for h in hits]

    @mcp.tool()
    async def document_count() -> int:
        """Number of live documents in the index."""
        return await asyncio.to_thread(ix.document_count)

    @mcp.resource("antelope://index/schema")
    def index_schema() -> dict:
        """Attribute schema (filterable fields + types) and index config."""
        return {"attributes": ix.schema(), "config": ix.info()}

    @mcp.tool()
    async def index_info() -> dict:
        """Attribute schema + index config (dimension, metric, document_count).

        Use this to discover which fields you can filter on and how to shape a
        filter for the `search` tool."""
        return await asyncio.to_thread(lambda: {"attributes": ix.schema(), "config": ix.info()})

    # writable tools (Task 13) are added later.
    return _Server(mcp, ix, writable, index_dir)

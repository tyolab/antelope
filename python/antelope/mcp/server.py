"""FastMCP server over a single Antelope index (stdio transport).

Read-only by default (search + document_count); mutation tools are registered
only when built with writable=True (Task 13). One index is opened at startup.
"""
import asyncio
import base64
import threading

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
    lock = threading.Lock()

    def _locked(fn, *args):
        """Run fn(*args) inside the worker thread, holding `lock` for its
        duration. The engine keeps a single non-thread-safe per-instance
        result buffer, so every engine-touching call -- across all tool
        handlers -- must be serialized through this helper; the lock is
        acquired inside the to_thread worker (not on the event loop) so
        concurrent asyncio tasks queue on the lock rather than racing the
        engine."""
        with lock:
            return fn(*args)

    @mcp.tool()
    async def search(query: str, k: int = 10, filter: dict | None = None) -> list[dict]:
        """Lexical + optional structured-filter search over the index.

        filter is a predicate dict: and/or/not/eq/in/range (see the antelope
        README). Returns [{key, score, payload?}]."""
        hits = await asyncio.to_thread(_locked, ix.search, query, k, filter)
        return [_hit_json(h) for h in hits]

    @mcp.tool()
    async def document_count() -> int:
        """Number of live documents in the index."""
        return await asyncio.to_thread(_locked, ix.document_count)

    @mcp.resource("antelope://index/schema")
    def index_schema() -> dict:
        """Attribute schema (filterable fields + types) and index config."""
        with lock:
            return {"attributes": ix.schema(), "config": ix.info()}

    @mcp.tool()
    async def index_info() -> dict:
        """Attribute schema + index config (dimension, metric, document_count).

        Use this to discover which fields you can filter on and how to shape a
        filter for the `search` tool."""
        return await asyncio.to_thread(
            _locked, lambda: {"attributes": ix.schema(), "config": ix.info()})

    if writable:
        @mcp.tool()
        async def add_document(key: str, text: str,
                               attributes: dict | None = None,
                               payload: str | None = None) -> dict:
            """Add a lexical document (no vector). Returns its {generation, docid} handle."""
            return await asyncio.to_thread(
                _locked, ix.add_document, key, text, None, None, attributes, payload)

        @mcp.tool()
        async def update_document(key: str, text: str,
                                  attributes: dict | None = None,
                                  payload: str | None = None) -> dict:
            """Upsert a lexical document. Returns its {generation, docid} handle."""
            return await asyncio.to_thread(
                _locked, ix.update_document, key, text, None, None, attributes, payload)

        @mcp.tool()
        async def delete_document(key: str) -> bool:
            """Delete a document by key. Returns True if it existed, False otherwise."""
            return await asyncio.to_thread(_locked, ix.delete_document, key)

        @mcp.tool()
        async def flush() -> None:
            """Flush the live buffer to a durable on-disk segment."""
            await asyncio.to_thread(_locked, ix.flush)

        @mcp.tool()
        async def maintain() -> None:
            """Run the tiered merge/compaction policy to quiescence."""
            await asyncio.to_thread(_locked, ix.maintain)

    return _Server(mcp, ix, writable, index_dir)

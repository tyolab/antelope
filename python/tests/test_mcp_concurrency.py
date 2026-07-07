import asyncio, tempfile, antelope
from antelope.mcp.server import build_server

def _seed(d, n=50):
    with antelope.SegmentIndex() as ix:
        ix.open(d)
        for i in range(n):
            ix.add_document(f"d{i}", "<DOC>apple banana</DOC>")
        ix.flush()

def test_concurrent_searches_are_correct_and_safe():
    d = tempfile.mkdtemp(); _seed(d, 50)
    srv = build_server(index_dir=d, writable=False)
    async def hammer():
        tasks = [srv.call_tool_for_test("search", {"query":"apple","k":50}) for _ in range(64)]
        results = await asyncio.gather(*tasks)
        # every concurrent search must return the full, correct result set (no races/truncation/crash)
        for hits in results:
            assert len(hits) == 50
            assert all(h["key"].startswith("d") for h in hits)
    asyncio.run(hammer())

def test_concurrent_mixed_read_write():
    d = tempfile.mkdtemp(); _seed(d, 20)
    srv = build_server(index_dir=d, writable=True)
    async def mixed():
        async def add(i): return await srv.call_tool_for_test("add_document", {"key":f"n{i}","text":"<DOC>apple</DOC>"})
        async def search(): return await srv.call_tool_for_test("search", {"query":"apple","k":100})
        await asyncio.gather(*([add(i) for i in range(20)] + [search() for _ in range(20)]))
        await srv.call_tool_for_test("flush", {})
        final = await srv.call_tool_for_test("search", {"query":"apple","k":100})
        assert len(final) == 40   # 20 seeded + 20 added
    asyncio.run(mixed())

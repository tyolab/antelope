"""Entry point: antelope-mcp --index <dir> [--writable] (stdio transport)."""
import argparse
import os

from .server import build_server


def main():
    p = argparse.ArgumentParser(prog="antelope-mcp")
    p.add_argument("--index", default=os.environ.get("ANTELOPE_INDEX"),
                   help="index directory (or set ANTELOPE_INDEX)")
    p.add_argument("--writable", action="store_true",
                   help="register mutation tools (add/update/delete/flush/maintain)")
    a = p.parse_args()
    if not a.index:
        p.error("--index (or ANTELOPE_INDEX) is required")
    srv = build_server(a.index, a.writable)
    srv.mcp.run()   # stdio


if __name__ == "__main__":
    main()

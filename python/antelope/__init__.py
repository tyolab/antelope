"""antelope — Python binding for the Antelope/ATIRE hybrid search engine.

Wraps the C++ ``ATIRE_segment_index`` (lexical + vector + hybrid + rerank search,
structured-attribute filtering, incremental add/update/delete) as a first-class,
importable extension. See ``SegmentIndex`` for the full surface.

Example::

    import antelope
    with antelope.SegmentIndex(dimension=4, metric="cosine") as ix:
        ix.open("/path/to/index")
        ix.add_document("doc1", "<DOC>hello world</DOC>", vector=[0.1, 0.2, 0.3, 0.4])
        ix.flush()
        for hit in ix.search("hello", 10):
            print(hit.key, hit.score)
"""
from ._core import SegmentIndex, Hit, _link_check  # noqa: F401

__version__ = "0.1.0"
__all__ = ["SegmentIndex", "Hit", "__version__"]

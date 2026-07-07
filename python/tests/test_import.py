def test_import_and_version():
    import antelope
    assert isinstance(antelope.__version__, str)
    assert antelope._link_check() is True

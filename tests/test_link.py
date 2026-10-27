from ni_wine import link

PREAMBLE = "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n\n"


def section(key: str, *values: str) -> str:
    return f"[{key}] 1700000000\n#time=1dc1234\n" + "".join(v + "\n" for v in values) + "\n"


CRYPTO = "Software\\\\Microsoft\\\\Cryptography"
NI = "Software\\\\Native Instruments"
KONTAKT = "Software\\\\Native Instruments\\\\Kontakt 8"
OTHER = "Software\\\\Other Vendor"


def test_split_and_join_round_trip():
    text = PREAMBLE + section(NI) + section(KONTAKT, '"InstallDir"="C:\\\\K8"')
    preamble, sections = link._split(text)
    assert preamble == PREAMBLE
    assert [key for key, _ in sections] == [NI, KONTAKT]
    assert link._join(preamble, sections) == text


def test_under_matches_subtrees_but_not_prefix_strings():
    assert link._under(KONTAKT, NI)
    assert link._under(NI, NI)
    assert link._under("software\\\\native instruments\\\\x", NI)
    assert not link._under("Software\\\\Native Instrumentsx", NI)
    assert not link._under(OTHER, NI)


def test_merge_replaces_vendor_subtree_and_keeps_the_rest():
    src = PREAMBLE + section(NI) + section(KONTAKT, '"InstallDir"="new"')
    dst = PREAMBLE + section(OTHER, '"keep"="yes"') + section(KONTAKT, '"InstallDir"="old"')
    merged = link._merge(src, dst, [NI])
    assert '"keep"="yes"' in merged
    assert '"InstallDir"="new"' in merged
    assert '"InstallDir"="old"' not in merged
    assert [key for key, _ in link._split(merged)[1]] == [OTHER, NI, KONTAKT]


def test_drop_removes_subtree():
    text = PREAMBLE + section(OTHER) + section(NI) + section(KONTAKT)
    dropped = link._drop(text, [NI])
    assert [key for key, _ in link._split(dropped)[1]] == [OTHER]


def test_machine_guid_read_and_replace():
    text = PREAMBLE + section(CRYPTO, '"MachineGuid"="aaaa-bbbb"')
    assert link.machine_guid(text) == "aaaa-bbbb"
    updated = link._set_machine_guid(text, "cccc-dddd")
    assert link.machine_guid(updated) == "cccc-dddd"
    assert updated.count("MachineGuid") == 1


def test_set_machine_guid_adds_missing_key():
    text = PREAMBLE + section(OTHER)
    assert link.machine_guid(text) is None
    updated = link._set_machine_guid(text, "eeee")
    assert link.machine_guid(updated) == "eeee"
    assert [key for key, _ in link._split(updated)[1]] == [OTHER, CRYPTO]


def test_link_tree_symlinks_missing_entries_and_merges_shared_dirs(tmp_path):
    src = tmp_path / "src"
    dst = tmp_path / "dst"
    (src / "Common Files" / "VST3").mkdir(parents=True)
    (src / "Common Files" / "VST3" / "Kontakt 8.vst3").write_text("")
    (src / "Native Instruments").mkdir()
    (src / "Windows NT").mkdir()
    (dst / "common files").mkdir(parents=True)
    (dst / "common files" / "VST3").mkdir()
    (dst / "common files" / "VST3" / "Other.vst3").write_text("")

    made: list = []
    link._link_tree(src, dst, 1, made)

    assert (dst / "Native Instruments").is_symlink()
    assert not (dst / "Windows NT").exists()
    assert not (dst / "common files").is_symlink()
    assert (dst / "common files" / "VST3" / "Kontakt 8.vst3").is_symlink()
    assert (dst / "common files" / "VST3" / "Other.vst3").is_file()
    assert sorted(p.name for p in made) == ["Kontakt 8.vst3", "Native Instruments"]

import json

from ni_wine import extract


def test_read_idt_skips_the_three_header_lines(tmp_path):
    (tmp_path / "Directory.idt").write_text(
        "Directory\tDirectory_Parent\tDefaultDir\n"
        "s72\tS72\tl255\n"
        "Directory\tDirectory\n"
        "TARGETDIR\t\tSourceDir\n"
        "ProgramFilesFolder\tTARGETDIR\tPROGRA~1|Program Files\n"
        "\n"
    )
    assert extract._read_idt(tmp_path, "Directory") == [
        ["TARGETDIR", "", "SourceDir"],
        ["ProgramFilesFolder", "TARGETDIR", "PROGRA~1|Program Files"],
    ]


def test_infer_root_by_segments_and_content(tmp_path):
    assert extract._infer_root(tmp_path, ["Kontakt 8", "x"]) == (
        "Program Files/Common Files/Native Instruments"
    )
    assert extract._infer_root(tmp_path, ["Documentation"]) == (
        "Program Files/Native Instruments/Kontakt 8"
    )
    assert extract._infer_root(tmp_path, ["Contents", "x64"]) is None

    (tmp_path / "a").mkdir()
    (tmp_path / "a" / "Kontakt 8.vst3").write_text("")
    assert extract._infer_root(tmp_path, []) == "Program Files/Common Files/VST3"
    assert extract._infer_root(tmp_path / "a" / "nothing-here", []) is None


def test_offline_in_finds_direct_and_nested(tmp_path):
    assert extract._offline_in(tmp_path) is None
    nested = tmp_path / "mia1234.tmp" / "OFFLINE"
    nested.mkdir(parents=True)
    assert extract._offline_in(tmp_path) == nested
    direct = tmp_path / "OFFLINE"
    direct.mkdir()
    assert extract._offline_in(tmp_path) == direct


def test_write_product_record(tmp_path):
    extract.write_product_record(tmp_path)
    record = (
        tmp_path / "users/Public/Documents/Native Instruments/installed_products/Kontakt 8.json"
    )
    data = json.loads(record.read_text())
    assert data["InstallDir"] == extract.KONTAKT8_INSTALL_DIR
    assert data["ContentDir"] == extract.KONTAKT8_CONTENT_DIR


def test_plan_execute_copies_without_touching_existing_unless_update(tmp_path):
    offline = tmp_path / "OFFLINE"
    offline.mkdir()
    src = offline / "payload.dll"
    src.write_text("new")
    (offline / "unused.bin").write_text("")
    out = tmp_path / "drive_c"
    dest = out / "Program Files/Native Instruments/Kontakt 8/payload.dll"
    dest.parent.mkdir(parents=True)
    dest.write_text("old")

    plan = extract.Plan([(src, "Program Files/Native Instruments/Kontakt 8")], offline)
    plan.execute(out, update=False)
    assert dest.read_text() == "old"
    plan.execute(out, update=True)
    assert dest.read_text() == "new"
    assert (out / "users/Public/Documents/Native Instruments/installed_products").is_dir()

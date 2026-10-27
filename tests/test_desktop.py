from ni_wine import config, desktop, doctor, launch


def test_recorded_default_reads_the_first_entry(tmp_path):
    mimeapps = tmp_path / "mimeapps.list"
    mimeapps.write_text(
        "[Added Associations]\n"
        "x-scheme-handler/native-access=ignored.desktop\n"
        "\n"
        "[Default Applications]\n"
        "text/plain=editor.desktop\n"
        "x-scheme-handler/native-access=first.desktop;second.desktop;\n"
    )
    assert desktop._recorded_default(mimeapps) == "first.desktop"


def test_recorded_default_without_entry(tmp_path):
    mimeapps = tmp_path / "mimeapps.list"
    mimeapps.write_text("[Default Applications]\ntext/plain=editor.desktop\n")
    assert desktop._recorded_default(mimeapps) is None
    assert desktop._recorded_default(tmp_path / "missing") is None


def test_recorded_scheme_handler_prefers_config_home(monkeypatch, tmp_path):
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    (tmp_path / "config").mkdir()
    (tmp_path / "data" / "applications").mkdir(parents=True)
    (tmp_path / "data" / "applications" / "mimeapps.list").write_text(
        "[Default Applications]\nx-scheme-handler/native-access=old.desktop\n"
    )
    assert desktop.recorded_scheme_handler() == (
        tmp_path / "data" / "applications" / "mimeapps.list",
        "old.desktop",
    )
    (tmp_path / "config" / "mimeapps.list").write_text(
        "[Default Applications]\nx-scheme-handler/native-access=native-access.desktop\n"
    )
    assert desktop.recorded_scheme_handler() == (
        tmp_path / "config" / "mimeapps.list",
        "native-access.desktop",
    )


def test_system_entry_current_compares_bytes(monkeypatch, tmp_path):
    apps = tmp_path / "share" / "applications"
    apps.mkdir(parents=True)
    monkeypatch.setenv("XDG_DATA_DIRS", str(tmp_path / "share"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "home"))
    assert not desktop._system_entry_current()
    entry = apps / config.DESKTOP_FILE_NAME
    entry.write_text(desktop._packaged("native-access.desktop"))
    assert desktop._system_entry_current()
    entry.write_text(entry.read_text() + "X-Extra=1\n")
    assert not desktop._system_entry_current()


def test_ui_scale(monkeypatch):
    monkeypatch.delenv("NI_WINE_SCALE", raising=False)
    monkeypatch.delenv("GDK_SCALE", raising=False)
    assert launch.ui_scale() == (1.0, "neither NI_WINE_SCALE nor GDK_SCALE is set", True)
    monkeypatch.setenv("GDK_SCALE", "2")
    assert launch.ui_scale() == (2.0, "from GDK_SCALE", True)
    monkeypatch.setenv("NI_WINE_SCALE", "1.5")
    assert launch.ui_scale()[:2] == (1.5, "from NI_WINE_SCALE")
    monkeypatch.setenv("NI_WINE_SCALE", "9")
    assert launch.ui_scale()[0] == 1.0
    assert launch.ui_scale()[2] is False
    monkeypatch.setenv("NI_WINE_SCALE", "abc")
    assert launch.ui_scale()[2] is False


def test_check_render():
    assert doctor.Check("wine", True, "/usr/bin/wine").render() == "  [ok] wine: /usr/bin/wine"
    assert doctor.Check("wine", False).render() == "  [!!] wine"
    assert doctor.Check("xdotool", False, "optional", required=False).render() == (
        "  [--] xdotool: optional"
    )

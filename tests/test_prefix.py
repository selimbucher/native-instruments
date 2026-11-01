"""Functions that read a prefix on disk: config, kontakt, daemon, msishim, guitarrig."""

import os

from ni_wine import config, daemon, guitarrig, kontakt, msishim


def test_default_prefix_honours_env(monkeypatch, tmp_path):
    monkeypatch.delenv("NI_WINE_PREFIX", raising=False)
    assert config.default_prefix().name == ".wine-ni"
    monkeypatch.setenv("NI_WINE_PREFIX", str(tmp_path))
    assert config.default_prefix() == tmp_path


def test_xdg_dirs_honour_env(monkeypatch, tmp_path):
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "c"))
    monkeypatch.setenv("XDG_STATE_HOME", str(tmp_path / "s"))
    assert config.cache_dir() == tmp_path / "c" / "ni-wine"
    assert config.state_dir() == tmp_path / "s" / "ni-wine"


def test_wine_user_skips_public(tmp_path):
    users = tmp_path / "drive_c" / "users"
    (users / "Public").mkdir(parents=True)
    (users / "selim").mkdir()
    assert config.wine_user(tmp_path) == "selim"
    assert "selim" in config.na_roaming_dir(tmp_path).parts


def test_win_to_unix(tmp_path):
    assert kontakt.win_to_unix(tmp_path, r"C:\Program Files\x") == (
        tmp_path / "drive_c" / "Program Files/x"
    )
    assert str(kontakt.win_to_unix(tmp_path, r"Z:\home\u\f.msi")) == "/home/u/f.msi"
    assert kontakt.win_to_unix(tmp_path, r"D:\x") == tmp_path / "dosdevices" / "d:" / "x"
    assert str(kontakt.win_to_unix(tmp_path, "relative")) == "relative"


def test_service_registered_reads_system_reg(tmp_path):
    assert not daemon.service_registered(tmp_path)
    (tmp_path / "system.reg").write_text(
        "[System\\\\CurrentControlSet\\\\Services\\\\NTKDaemonService] 1700000000\n"
    )
    assert daemon.service_registered(tmp_path)


def test_prefix_of_reads_the_process_cwd(tmp_path, monkeypatch):
    inside = tmp_path / "drive_c" / "users"
    inside.mkdir(parents=True)
    monkeypatch.chdir(inside)
    assert daemon._prefix_of(os.getpid()) == os.path.realpath(tmp_path)
    monkeypatch.chdir(tmp_path)
    assert daemon._prefix_of(os.getpid()) is None
    assert daemon._prefix_of(2**22 + 12345) is None


def test_msi_file_identification(tmp_path):
    builtin = tmp_path / "builtin.dll"
    builtin.write_bytes(b"MZ" + b"\0" * 62 + b"Wine builtin DLL" + b"\0" * 32)
    shim = tmp_path / "shim.dll"
    shim.write_bytes(b"MZ" + b"\0" * 200 + b"ni-wine-msi-shim/1\0" + b"\0" * 20)
    assert msishim.is_wine_builtin(builtin)
    assert not msishim.is_shim(builtin)
    assert msishim.is_shim(shim)
    assert not msishim.is_wine_builtin(shim)
    assert not msishim.is_shim(tmp_path / "missing.dll")


def test_shim_config_text(monkeypatch, tmp_path):
    monkeypatch.setenv("XDG_STATE_HOME", str(tmp_path))
    text = msishim._cfg_text()
    assert text.startswith("divert=Kontakt 8 Setup PC.msi\r\n")
    assert f"hook={tmp_path}/ni-wine/msi-hook.sh\r\n" in text
    assert "result=Z:" + str(tmp_path).replace("/", "\\") in text
    assert text.endswith("timeout=7200\r\n")


def _guitar_rig_prefix(tmp_path):
    (tmp_path / "drive_c" / "users" / "selim").mkdir(parents=True)
    return tmp_path


def test_guitar_rig_database_states(tmp_path):
    prefix = _guitar_rig_prefix(tmp_path)
    folder = guitarrig.database_dir(prefix, 6)
    assert guitarrig.broken_database(prefix, 6) is None  # never started
    folder.mkdir(parents=True)
    (folder / "gr6db").write_bytes(b"")
    assert guitarrig.broken_database(prefix, 6) == folder  # cut short before the vocabulary
    (folder / "User.kad").write_text(
        "<kore2-attribute-definition><sets/></kore2-attribute-definition>"
    )
    assert guitarrig.broken_database(prefix, 6) == folder  # skeleton vocabulary
    (folder / "gr6db").unlink()
    assert guitarrig.broken_database(prefix, 6) == folder
    (folder / "User.kad").write_text(
        '<set name="CP://Amplifiers"><attribute name="CP://Plex"/></set>'
    )
    assert guitarrig.broken_database(prefix, 6) is None  # full vocabulary, import still to come
    (folder / "gr6db").write_bytes(b"JL\x1a")
    assert guitarrig.broken_database(prefix, 6) is None


def test_guitar_rig_reset_removes_only_broken(tmp_path):
    prefix = _guitar_rig_prefix(tmp_path)
    broken = guitarrig.database_dir(prefix, 6)
    broken.mkdir(parents=True)
    (broken / "gr6db").write_bytes(b"")
    (broken / "User.kad").write_text("<sets/>")
    healthy = guitarrig.database_dir(prefix, 7)
    healthy.mkdir(parents=True)
    (healthy / "gr7db").write_bytes(b"JL\x1a")
    assert guitarrig.reset(prefix) == [6]
    assert not broken.exists()
    assert (healthy / "gr7db").is_file()
    assert guitarrig.reset(prefix) == []


def test_yabridge_groups_first_match_wins(tmp_path):
    vst3 = tmp_path / ".vst3" / "yabridge"
    (vst3 / "Guitar Rig 7.vst3").mkdir(parents=True)
    (vst3 / "Guitar Rig 6.vst3").mkdir()
    (vst3 / "Kontakt 8.vst3").mkdir()
    assert guitarrig.yabridge_groups(tmp_path) == {
        vst3 / "yabridge.toml": {"Guitar Rig 6.vst3": "", "Guitar Rig 7.vst3": ""}
    }
    (vst3 / "yabridge.toml").write_text('["Guitar Rig 7*"]\ngroup = "mine"\n\n["*"]\ngroup = ""\n')
    groups = guitarrig.yabridge_groups(tmp_path)[vst3 / "yabridge.toml"]
    assert groups == {"Guitar Rig 6.vst3": "", "Guitar Rig 7.vst3": "mine"}


def test_group_plugins_appends_only_missing(tmp_path):
    vst = tmp_path / ".vst" / "yabridge"
    vst.mkdir(parents=True)
    (vst / "Guitar Rig 6.so").touch()
    (vst / "Guitar Rig 6.dll").touch()
    vst3 = tmp_path / ".vst3" / "yabridge"
    (vst3 / "Guitar Rig 7.vst3").mkdir(parents=True)
    toml = vst3 / "yabridge.toml"
    toml.write_text('["Guitar Rig 7.vst3"]\ngroup = "mine"')
    assert guitarrig.group_plugins(tmp_path) == [vst / "yabridge.toml"]
    assert toml.read_text() == '["Guitar Rig 7.vst3"]\ngroup = "mine"'
    assert (vst / "yabridge.toml").read_text() == (
        "# ni-wine: Guitar Rig saves presets from one process only\n"
        '["Guitar Rig 6.so"]\ngroup = "guitar-rig-6"\n'
    )
    assert guitarrig.yabridge_groups(tmp_path)[vst / "yabridge.toml"] == {
        "Guitar Rig 6.so": "guitar-rig-6"
    }
    assert guitarrig.group_plugins(tmp_path) == []
    # shadowed by an earlier catch-all: reported, never appended twice
    toml.write_text('["*"]\nhide_daw = true\n')
    assert guitarrig.group_plugins(tmp_path) == [toml]
    assert guitarrig.group_plugins(tmp_path) == []
    assert guitarrig.yabridge_groups(tmp_path)[toml] == {"Guitar Rig 7.vst3": ""}

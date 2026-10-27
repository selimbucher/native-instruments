"""Functions that read a prefix on disk: config, kontakt, daemon, msishim."""

import os

from ni_wine import config, daemon, kontakt, msishim


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

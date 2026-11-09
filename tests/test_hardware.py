from ni_wine import hardware

ENUM = "[System\\\\ControlSet001\\\\Enum\\\\USB\\\\VID_17CC&PID_1600"
SYSTEM_REG = (
    "WINE REGISTRY Version 2\n"
    "\n"
    f"{ENUM}&MI_00\\\\512&325&5&3] 1793712686\n"
    '"Class"="NIUSBDevice"\n'
    '"Driver"="{{E76E9530-E0A0-4B82-A618-D3F60C00DDF4}}\\\\0001"\n'
    '"Service"="{audio}"\n'
    "\n"
    f"{ENUM}&MI_05\\\\512&325&5&3] 1793707531\n"
    '"Class"="NIUSBDevice"\n'
    '"ConfigFlags"=dword:00000000\n'
    '"Driver"="{{E76E9530-E0A0-4B82-A618-D3F60C00DDF4}}\\\\0000"\n'
    '"Service"="{display}"\n'
    "\n"
    f"{ENUM}&MI_05\\\\512&325&5&3\\\\Device Parameters] 1793707531\n"
    "\n"
    "[System\\\\ControlSet001\\\\Services\\\\nirescan] 1793707000\n"
    '"ImagePath"="C:\\\\windows\\\\system32\\\\nirescan.exe"\n'
    "\n"
    "[Software\\\\ni-wine] 1793707000\n"
    '"ni-maschine"="/usr/bin/ni-maschine"\n'
)

AUDIO, DISPLAY = hardware.INTERFACE_IDS[1], hardware.INTERFACE_IDS[0]


def write_reg(tmp_path, audio="nimk3", display="nimk3"):
    (tmp_path / "system.reg").write_text(SYSTEM_REG.format(audio=audio, display=display))


def test_misbound_interfaces_finds_nis_driver(tmp_path):
    write_reg(tmp_path, audio="nimc3usb", display="nimc3usb")
    assert hardware.misbound_interfaces(tmp_path) == {AUDIO: "nimc3usb", DISPLAY: "nimc3usb"}


def test_misbound_interfaces_finds_an_interface_without_driver(tmp_path):
    write_reg(tmp_path, audio="x")
    text = (tmp_path / "system.reg").read_text().replace('"Service"="x"\n', "")
    (tmp_path / "system.reg").write_text(text)
    assert hardware.misbound_interfaces(tmp_path) == {AUDIO: ""}


def test_misbound_interfaces_accepts_nimk3(tmp_path):
    write_reg(tmp_path)
    assert hardware.misbound_interfaces(tmp_path) == {}


def test_misbound_interfaces_skips_never_bound(tmp_path):
    # Wine binds those itself, to nimk3
    text = f'{ENUM}&MI_00\\\\512&325&1&3] 1794009956\n"Class"="NIUSBDevice"\n'
    (tmp_path / "system.reg").write_text(text)
    assert hardware.misbound_interfaces(tmp_path) == {}


def test_unregistered_services_reads_system_reg(tmp_path):
    assert hardware.unregistered_services(tmp_path) == list(hardware.SERVICES)
    write_reg(tmp_path)
    assert hardware.unregistered_services(tmp_path) == ["nimidi"]


def test_daemon_registered_compares_the_path(tmp_path):
    write_reg(tmp_path)
    assert hardware.daemon_registered(tmp_path, "/usr/bin/ni-maschine")
    assert not hardware.daemon_registered(tmp_path, "/usr/local/bin/ni-maschine")


def test_daemon_executable_prefers_the_path_entry(tmp_path, monkeypatch):
    store = tmp_path / "store" / "bin"
    store.mkdir(parents=True)
    (store / hardware.DAEMON).write_text("")
    profile = tmp_path / "profile"
    profile.mkdir()
    (profile / hardware.DAEMON).symlink_to(store / hardware.DAEMON)
    package = tmp_path / "store" / "lib" / "python3" / "site-packages" / "ni_wine" / "hardware.py"
    monkeypatch.setattr(hardware, "__file__", str(package))
    monkeypatch.setattr(hardware.shutil, "which", lambda name: str(profile / name))
    assert hardware.daemon_executable() == str(profile / hardware.DAEMON)
    monkeypatch.setattr(hardware.shutil, "which", lambda name: None)
    assert hardware.daemon_executable() == str(store / hardware.DAEMON)


def test_unit_text_quotes_paths():
    text = hardware.unit_text("/opt/my apps/ni-maschine", hardware.Path("/home/u/my prefix"))
    assert 'ExecStart="/opt/my apps/ni-maschine"\n' in text
    assert 'Environment="NI_WINE_PREFIX=/home/u/my prefix"\n' in text
    assert "WantedBy=default.target\n" in text


def test_outdated_files_compares_with_the_shipped_ones(tmp_path, monkeypatch):
    shipped = tmp_path / "data"
    shipped.mkdir()
    names = (hardware.INF, hardware.DRIVER, *(f"{name}.exe" for name in hardware.SERVICES))
    for name in names:
        (shipped / name).write_bytes(name.encode())
    monkeypatch.setattr(hardware, "_source", lambda name: shipped / name)
    prefix = tmp_path / "prefix"
    assert len(hardware.outdated_files(prefix)) == len(names)

    for name, target in hardware._targets(prefix).items():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(name.encode())
    assert hardware.outdated_files(prefix) == []

    hardware._targets(prefix)[hardware.DRIVER].write_bytes(b"old")
    assert hardware.outdated_files(prefix) == [hardware.DRIVER]


def write_hidraw(root, name, hid_id, hid_name):
    device = root / "sys" / name / "device"
    device.mkdir(parents=True)
    (device / "uevent").write_text(f"DRIVER=hid-generic\nHID_ID={hid_id}\nHID_NAME={hid_name}\n")


def test_connected_controllers_lists_ni_devices_with_access(tmp_path):
    write_hidraw(tmp_path, "hidraw0", "0003:0000046D:0000C548", "Logitech USB Receiver")
    write_hidraw(tmp_path, "hidraw1", "0003:000017CC:00001600", "Maschine MK3")
    write_hidraw(tmp_path, "hidraw2", "0003:000017CC:00001700", "Maschine Mikro MK3")
    (tmp_path / "dev").mkdir()
    (tmp_path / "dev" / "hidraw1").touch()
    found = hardware.connected_controllers(tmp_path / "sys", tmp_path / "dev")
    assert found == {
        f"Maschine MK3 ({tmp_path}/dev/hidraw1)": True,
        f"Maschine Mikro MK3 ({tmp_path}/dev/hidraw2)": False,
    }

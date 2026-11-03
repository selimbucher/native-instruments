from ni_wine import hardware

ENUM = "[System\\\\ControlSet001\\\\Enum\\\\USB\\\\VID_17CC&PID_1600"
SYSTEM_REG = (
    "WINE REGISTRY Version 2\n"
    "\n"
    f"{ENUM}&MI_00\\\\512&325&5&3] 1793712686\n"
    '"Class"="NIUSBDevice"\n'
    '"Service"="nimc3usb"\n'
    "\n"
    f"{ENUM}&MI_05\\\\512&325&5&3] 1793707531\n"
    '"Class"="NIUSBDevice"\n'
    '"ConfigFlags"=dword:00000000\n'
    '"Service"="{service}"\n'
    "\n"
    f"{ENUM}&MI_05\\\\512&325&5&3\\\\Device Parameters] 1793707531\n"
    "\n"
    "[System\\\\ControlSet001\\\\Services\\\\nirescan] 1793707000\n"
    '"ImagePath"="C:\\\\windows\\\\system32\\\\nirescan.exe"\n'
)


def test_foreign_displays_finds_nis_driver(tmp_path):
    (tmp_path / "system.reg").write_text(SYSTEM_REG.format(service="nimc3usb"))
    assert hardware.foreign_displays(tmp_path) == ["nimc3usb"]


def test_foreign_displays_accepts_nimk3_and_unbound(tmp_path):
    (tmp_path / "system.reg").write_text(SYSTEM_REG.format(service="nimk3"))
    assert hardware.foreign_displays(tmp_path) == []
    (tmp_path / "system.reg").write_text(
        SYSTEM_REG.format(service="x").replace('"Service"="x"\n', "")
    )
    assert hardware.foreign_displays(tmp_path) == []


def test_service_registered_reads_system_reg(tmp_path):
    assert not hardware.service_registered(tmp_path)
    (tmp_path / "system.reg").write_text(SYSTEM_REG.format(service="nimk3"))
    assert hardware.service_registered(tmp_path)


def test_outdated_files_compares_with_the_shipped_ones(tmp_path, monkeypatch):
    shipped = tmp_path / "data"
    shipped.mkdir()
    for name in (hardware.INF, hardware.DRIVER, "nirescan.exe"):
        (shipped / name).write_bytes(name.encode())
    monkeypatch.setattr(hardware, "_source", lambda name: shipped / name)
    prefix = tmp_path / "prefix"
    assert len(hardware.outdated_files(prefix)) == 3

    for name, target in hardware._targets(prefix).items():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(name.encode())
    assert hardware.outdated_files(prefix) == []

    hardware._targets(prefix)[hardware.DRIVER].write_bytes(b"old")
    assert hardware.outdated_files(prefix) == [hardware.DRIVER]

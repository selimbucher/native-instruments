"""What ni-wine does when Native Access installs Kontakt 8.

Kontakt's own installer hangs under Wine (see shim/msi_shim.c).  Native
Access still drives the install like for any other product: its daemon
downloads and runs the installer, and when the installer reaches its MSI
step the msi shim hands the work to `apply_installer` below, which lays the
files out from the payload the installer already extracted.  Native Access
then sees an ordinary successful install.
"""

from __future__ import annotations

import contextlib
import io
import traceback
from datetime import datetime
from pathlib import Path

from . import config, msishim
from .extract import KONTAKT8_REGISTRY, plan_installer_dir, write_product_record
from .util import die, guarded_rmtree, info
from .wine import Wine


def _remove_kontakt_files(prefix: Path, *, include_product_json: bool) -> None:
    for rel in config.KONTAKT8_PREFIX_PATHS:
        guarded_rmtree(config.drive_c(prefix) / rel)
    if include_product_json:
        (config.drive_c(prefix) / config.KONTAKT8_PRODUCT_JSON).unlink(missing_ok=True)


# --- via Native Access ------------------------------------------------------


def win_to_unix(prefix: Path, win_path: str) -> Path:
    """Map a Windows path from inside the prefix to the host filesystem."""
    drive, sep, rest = win_path.partition(":")
    if not sep or len(drive) != 1:
        return Path(win_path)
    rest = rest.replace("\\", "/").lstrip("/")
    letter = drive.lower()
    if letter == "c":
        return config.drive_c(prefix) / rest
    if letter == "z":
        return Path("/") / rest
    return prefix / "dosdevices" / f"{letter}:" / rest


def apply_installer(prefix: Path, package: str) -> int:
    """Called by the msi shim (through the hook script) with the MSI path.

    Lays the payload out and reports the outcome in the result file the
    shim waits for.  Never raises: the installer must get an answer.
    """
    result = msishim.result_file()
    log = msishim.hook_log()
    result.parent.mkdir(parents=True, exist_ok=True)

    def note(text: str) -> None:
        stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        with open(log, "a", encoding="utf-8") as handle:
            handle.write(f"{stamp} {text}\n")

    note(f"apply-installer {package}")
    captured = io.StringIO()
    try:
        with contextlib.redirect_stdout(captured), contextlib.redirect_stderr(captured):
            msi = win_to_unix(prefix, package)
            plan = plan_installer_dir(msi)  # validates the payload first
            note(f"payload verified ({len(plan.copy_list)} files); replacing Kontakt 8")
            _remove_kontakt_files(prefix, include_product_json=False)
            plan.execute(config.drive_c(prefix), update=True)
            _write_registry(prefix)
    except SystemExit:  # die() inside the helpers; its message is in `captured`
        lines = [line for line in captured.getvalue().splitlines() if line.strip()]
        message = lines[-1].removeprefix("error: ") if lines else "failed"
        return _apply_failed(prefix, note, result, message)
    except Exception as exc:  # noqa: BLE001 — anything else is still an answer
        note("".join(traceback.format_exception(exc)).strip())
        return _apply_failed(prefix, note, result, str(exc))
    for line in captured.getvalue().splitlines():
        if line.strip():
            note(line)
    note("OK")
    result.write_text("OK\n")
    return 0


REGISTRY_KEY = r"HKLM\Software\Native Instruments\Kontakt 8"


def registry_complete(prefix: Path) -> bool:
    """Are the product's registry values there?  (Reads system.reg, which
    Wine flushes with a short delay after a write.)"""
    text = Wine(prefix).system_reg_text()
    section = text.split("[Software\\\\Native Instruments\\\\Kontakt 8]", 1)
    if len(section) < 2:
        return False
    body = section[1].split("\n[", 1)[0]
    return all(f'"{name}"=' in body for name, _ in KONTAKT8_REGISTRY)


def _write_registry(prefix: Path) -> None:
    """Replay the MSI's Registry table rows for the product.

    Without them the daemon's product scan finds Kontakt right after the
    install (the install record is enough for that) but drops it on the
    next full refresh, which evaluates the registry, and Native Access
    offers to install it again.
    """
    wine = Wine(prefix)
    for name, value in KONTAKT8_REGISTRY:
        if not wine.reg_add(REGISTRY_KEY, name, value, kill_on_failure=False):
            die(f"could not write {REGISTRY_KEY}\\{name}")
    info("registered Kontakt 8 in the prefix registry")


def repair_registry(prefix: Path) -> bool:
    """Write the registry values for an installed Kontakt that lacks them
    (installs made by earlier ni-wine versions).  Returns True if written."""
    if not config.kontakt8_exe(prefix).is_file() or registry_complete(prefix):
        return False
    write_product_record(config.drive_c(prefix))
    _write_registry(prefix)
    return True


def _apply_failed(prefix: Path, note, result: Path, message: str) -> int:
    """Report a failed apply.  The installer and the daemon ignore MSI and
    exit codes; Native Access only believes the product's install record,
    so drop it — the product shows as not installed and can be retried."""
    note(f"ERR {message}")
    (config.drive_c(prefix) / config.KONTAKT8_PRODUCT_JSON).unlink(missing_ok=True)
    Wine(prefix).reg_delete(REGISTRY_KEY)  # the daemon believes these too
    result.write_text(f"ERR: {message}\n")
    return 1

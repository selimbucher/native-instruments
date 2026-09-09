"""Install, update, and uninstall Kontakt 8 in the Wine prefix.

Kontakt's own installer hangs under Wine (see shim/msi_shim.c), so the
files are laid out by ni-wine from the installer's payload (extract.py).
Two ways to get there:

- Through Native Access, like any other product: click Install/Update in
  NA, its daemon downloads and runs the installer, and when the installer
  reaches its MSI step the msi shim hands the work to `apply_installer`
  below.  Native Access then sees an ordinary successful install.  This
  needs the hook installed in the prefix (`ni launch` does that).
- From a file or URL: `ni kontakt8 install|update <zip|exe|url>` unpacks
  the installer with 7z and lays it out directly, no Native Access involved.
"""

from __future__ import annotations

import contextlib
import io
import traceback
from datetime import datetime
from pathlib import Path

from . import config, msishim
from .extract import plan_installer, plan_installer_dir
from .util import die, download, guarded_rmtree, info
from .wine import Wine, foreign_prefix_users


def _guard_prefix_users(prefix: Path) -> None:
    """Refuse to overwrite Kontakt's files while a DAW has them loaded."""
    hosts = foreign_prefix_users(prefix)
    if hosts:
        die(
            f"{len(hosts)} plugin host(s) (yabridge) are using {prefix} — "
            "Kontakt's files may be in use.  Close your DAW first, then try again."
        )


def _remove_kontakt_files(prefix: Path, *, include_product_json: bool) -> None:
    for rel in config.KONTAKT8_PREFIX_PATHS:
        guarded_rmtree(config.drive_c(prefix) / rel)
    if include_product_json:
        (config.drive_c(prefix) / config.KONTAKT8_PRODUCT_JSON).unlink(missing_ok=True)


# --- via Native Access ------------------------------------------------------


def _via_native_access(prefix: Path, verb: str) -> None:
    from .launch import run_launch

    wine = Wine(prefix)
    problem = msishim.ensure(wine, prefix)
    if problem:
        die(problem + " — pass the installer file or URL instead")
    print()
    info(f"Native Access will open now.  Click {verb} on Kontakt 8 there.")
    info("Native Access downloads and runs NI's installer; ni-wine steps in at")
    info("its MSI step and lays the files out, so it completes like any other")
    info("product.  Keep your DAW closed while it runs.")
    print()
    raise SystemExit(run_launch(prefix))


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
            _guard_prefix_users(prefix)
            plan = plan_installer_dir(msi)  # validates the payload first
            note(f"payload verified ({len(plan.copy_list)} files); replacing Kontakt 8")
            _remove_kontakt_files(prefix, include_product_json=False)
            plan.execute(config.drive_c(prefix), update=True)
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


def _apply_failed(prefix: Path, note, result: Path, message: str) -> int:
    """Report a failed apply.  The installer and the daemon ignore MSI and
    exit codes; Native Access only believes the product's install record,
    so drop it — the product shows as not installed and can be retried."""
    note(f"ERR {message}")
    (config.drive_c(prefix) / config.KONTAKT8_PRODUCT_JSON).unlink(missing_ok=True)
    result.write_text(f"ERR: {message}\n")
    return 1


# --- from a file or URL ------------------------------------------------------


def _obtain_installer(source: str) -> tuple[Path, bool]:
    """Return (installer path, owned-by-ni-wine)."""
    local = Path(source).expanduser()
    if local.is_file():
        info(f"using {local}")
        return local, False
    if "://" not in source:
        die(f"{source} is neither a file nor a URL")
    info("downloading Kontakt 8...")
    name = source.rsplit("/", 1)[-1].split("?")[0] or config.KONTAKT8_ZIP_NAME
    return download(source, config.cache_dir() / name, label="Kontakt 8 installer"), True


def install(prefix: Path, source: str | None = None) -> None:
    if config.kontakt8_exe(prefix).is_file():
        info("Kontakt 8 is already installed — use `ni kontakt8 update` to update")
        return
    if source is None:
        _via_native_access(prefix, "Install")
    installer, owned = _obtain_installer(source)
    plan = plan_installer(installer)
    _guard_prefix_users(prefix)
    plan.execute(config.drive_c(prefix), update=False)
    if owned:
        installer.unlink(missing_ok=True)
    info("Kontakt 8 installed")


def update(prefix: Path, source: str | None = None) -> None:
    if source is None:
        _via_native_access(prefix, "Update")
    installer, owned = _obtain_installer(source)
    plan = plan_installer(installer)  # verified before anything is removed
    _guard_prefix_users(prefix)
    info("removing old Kontakt 8 files...")
    _remove_kontakt_files(prefix, include_product_json=False)
    plan.execute(config.drive_c(prefix), update=True)
    if owned:
        installer.unlink(missing_ok=True)
    info("Kontakt 8 updated")


def uninstall(prefix: Path) -> None:
    _guard_prefix_users(prefix)
    info("removing Kontakt 8 files from the Wine prefix...")
    _remove_kontakt_files(prefix, include_product_json=True)
    info("Kontakt 8 uninstalled")

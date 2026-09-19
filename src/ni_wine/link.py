"""Make the prefix's products usable from a DAW that runs in its own prefix.

A Windows DAW under Wine loads plugins into its
own prefix.  Pointing it at ~/.wine-ni over Z: finds the plugin files but
not the products: an NI plugin decides activation from state that lives in
the prefix it runs in, so Kontakt comes up as "Kontakt 8 Demo".  That state:

- Common Files\\Native Instruments\\Service Center\\*.xml (which products
  use RAS3 licensing), then installed_products\\*.json, then one signed
  Native Access\\ras3\\<upid>.jwt per product
- HKLM\\Software\\Native Instruments (install and content locations)
- the JWT's hardware profile, which names the prefix's MachineGuid

`link` gives the DAW's prefix all of it: symlinks at the same C:\\ paths
(installs, plugins, libraries; nothing is copied), the vendors' registry
keys, and this prefix's MachineGuid.  To NI both prefixes are then the
same machine, and Native Access in this prefix stays the one place that
installs, updates and activates.

The DAW prefix's registry is edited as text while its Wine session is
down, so no second Wine build ever boots it.
"""

from __future__ import annotations

import json
import re
import shutil
import time
from pathlib import Path

from . import config
from .util import die, info, warn
from .wine import prefix_in_use, session_wine

# Directory trees merged into the DAW prefix.  Entries it lacks become
# symlinks; directories both have (Common Files, VST3, ...) are merged into.
_ROOTS = ("Program Files", "Program Files (x86)", "ProgramData", "users/Public/Documents")
_MAX_DEPTH = 4
# Windows' and Wine's own directories, installer caches, IPC scratch space,
# and ni-wine's PowerShell wrapper (Native Access's installers only).
_SKIP = re.compile(
    r"microsoft.*|windows.*|internet explorer|system|package cache|boost_interprocess"
    r"|temp|powershell|\{[0-9a-f-]{36}\}",
    re.IGNORECASE,
)
# Folders every vendor installs into: always real directories in the DAW
# prefix, so that what gets installed there later stays there.
_SHARED = {
    "common files", "vst2", "vst3", "clap", "vstplugins", "steinberg", "avid", "audio", "plug-ins",
}

# Registry subtrees, spelled as in Wine's .reg files (keys relative to HKLM
# in system.reg, to HKCU in user.reg).  Native Access also installs iZotope
# and Plugin Alliance products.
_VENDORS = ("Native Instruments", "iZotope", "iZotope, Inc.", "Plugin Alliance")
_MACHINE_KEYS = tuple(rf"Software\\{v}" for v in _VENDORS) + tuple(
    rf"Software\\Wow6432Node\\{v}" for v in _VENDORS
)
_USER_KEYS = tuple(rf"Software\\{v}" for v in _VENDORS)
_CRYPTO_KEY = r"Software\\Microsoft\\Cryptography"

_HEADER = re.compile(r"^\[(.*)\] \d+$")
_GUID_VALUE = re.compile(r'^"MachineGuid"="([^"]*)"$', re.MULTILINE)


# --- registry files ---------------------------------------------------------


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="surrogateescape")


def _write(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + ".ni-wine-tmp")
    tmp.write_text(text, encoding="utf-8", errors="surrogateescape")
    tmp.replace(path)


def _split(text: str) -> tuple[str, list[tuple[str, str]]]:
    """A registry file as (preamble, [(key, section text), ...])."""
    preamble: list[str] = []
    sections: list[tuple[str, list[str]]] = []
    for line in text.splitlines(keepends=True):
        match = _HEADER.match(line.rstrip("\r\n"))
        if match:
            sections.append((match.group(1), [line]))
        elif sections:
            sections[-1][1].append(line)
        else:
            preamble.append(line)
    return "".join(preamble), [(key, "".join(lines)) for key, lines in sections]


def _join(preamble: str, sections: list[tuple[str, str]]) -> str:
    return preamble + "".join(text.rstrip("\n") + "\n\n" for _, text in sections)


def _under(key: str, subtree: str) -> bool:
    # iZotope names some keys "iZotope, Inc./<product>".
    key, subtree = key.lower(), subtree.lower()
    return key == subtree or key.startswith((subtree + "\\\\", subtree + "/"))


def _present(sections: list[tuple[str, str]], subtree: str) -> bool:
    return any(_under(key, subtree) for key, _ in sections)


def _merge(src: str, dst: str, subtrees: list[str]) -> str:
    """*dst* with its *subtrees* replaced by *src*'s."""
    _, src_sections = _split(src)
    preamble, dst_sections = _split(dst)
    kept = [s for s in dst_sections if not any(_under(s[0], t) for t in subtrees)]
    taken = [s for s in src_sections if any(_under(s[0], t) for t in subtrees)]
    return _join(preamble, kept + taken)


def _drop(text: str, subtrees: list[str]) -> str:
    preamble, sections = _split(text)
    return _join(preamble, [s for s in sections if not any(_under(s[0], t) for t in subtrees)])


def machine_guid(system_reg: str) -> str | None:
    for key, section in _split(system_reg)[1]:
        if key.lower() == _CRYPTO_KEY.lower():
            match = _GUID_VALUE.search(section)
            return match.group(1) if match else None
    return None


def _set_machine_guid(system_reg: str, guid: str) -> str:
    preamble, sections = _split(system_reg)
    line = f'"MachineGuid"="{guid}"'
    for i, (key, section) in enumerate(sections):
        if key.lower() == _CRYPTO_KEY.lower():
            if _GUID_VALUE.search(section):
                section = _GUID_VALUE.sub(line, section, count=1)
            else:
                section = section.rstrip("\n") + "\n" + line + "\n"
            sections[i] = (key, section)
            return _join(preamble, sections)
    sections.append((_CRYPTO_KEY, f"[{_CRYPTO_KEY}] {int(time.time())}\n{line}\n"))
    return _join(preamble, sections)


# --- directory trees ----------------------------------------------------------


def _link_tree(src: Path, dst: Path, depth: int, made: list[Path]) -> None:
    # Wine resolves paths case-insensitively; so must the merge.
    existing = {entry.name.lower(): entry for entry in dst.iterdir()}
    for entry in sorted(src.iterdir()):
        if _SKIP.fullmatch(entry.name):
            continue
        target = existing.get(entry.name.lower())
        if target is None and entry.name.lower() in _SHARED and entry.is_dir():
            target = dst / entry.name
            target.mkdir()
        if target is None:
            link = dst / entry.name
            link.symlink_to(entry)
            made.append(link)
        elif (
            depth < _MAX_DEPTH
            and not target.is_symlink()
            and target.is_dir()
            and not entry.is_symlink()
            and entry.is_dir()
        ):
            _link_tree(entry, target, depth + 1, made)


def _our_links(target_c: Path, source_c: Path):
    """Symlinks in *target_c*'s merged roots that point into *source_c*."""
    for root in _ROOTS:
        yield from _links_below(target_c / root, source_c, 1)


def _links_below(dst: Path, source_c: Path, depth: int):
    try:
        entries = list(dst.iterdir())
    except OSError:
        return
    for entry in entries:
        if entry.is_symlink():
            target = Path(entry.readlink())
            if target == source_c or source_c in target.parents:
                yield entry
        elif depth < _MAX_DEPTH and entry.is_dir():
            yield from _links_below(entry, source_c, depth + 1)


# --- state ------------------------------------------------------------------


def _state_file() -> Path:
    return config.state_dir() / "links.json"


def _load_state() -> dict:
    try:
        return json.loads(_state_file().read_text())
    except (OSError, json.JSONDecodeError):
        return {}


def _save_state(state: dict) -> None:
    _state_file().parent.mkdir(parents=True, exist_ok=True)
    _state_file().write_text(json.dumps(state, indent=2) + "\n")


def linked_prefixes(prefix: Path) -> list[Path]:
    return [Path(p) for p in _load_state().get(str(prefix.resolve()), {})]


# --- commands ---------------------------------------------------------------


def _busy(prefix: Path) -> bool:
    return session_wine(prefix) is not None or prefix_in_use(prefix)


def _check_prefixes(prefix: Path, target: Path) -> None:
    if not (config.drive_c(prefix) / "users/Public/Documents/Native Instruments").is_dir():
        die(f"{prefix} has no Native Instruments products — install some in Native Access first")
    if not (target / "system.reg").is_file() or not config.drive_c(target).is_dir():
        die(f"no Wine prefix at {target}")
    if target.resolve() == prefix.resolve():
        die("that is the Native Access prefix itself")
    if _busy(target):
        die(f"{target} is in use — close the DAW (and anything else running in it) first")


def sync(prefix: Path, target: Path, *, first: bool = False) -> None:
    """Bring *target* up to date with *prefix*'s products (idempotent)."""
    source_c = config.drive_c(prefix).resolve()
    target_c = config.drive_c(target)

    made: list[Path] = []
    for root in _ROOTS:
        src = source_c / root
        if src.is_dir():
            dst = target_c / root
            dst.mkdir(parents=True, exist_ok=True)
            _link_tree(src, dst, 1, made)
    gone = [link for link in _our_links(target_c, source_c) if not link.exists()]
    for link in gone:
        link.unlink()

    src_system = _read(prefix / "system.reg")
    src_user = _read(prefix / "user.reg")
    dst_system = _read(target / "system.reg")
    dst_user = _read(target / "user.reg")
    _, src_system_sections = _split(src_system)
    _, src_user_sections = _split(src_user)
    _, dst_system_sections = _split(dst_system)
    _, dst_user_sections = _split(dst_user)

    guid = machine_guid(src_system)
    if not guid:
        die(f"{prefix} has no MachineGuid — is it a Wine prefix?")

    if first:
        # Kept for `unlink`: the DAW prefix's own MachineGuid, and which
        # vendor keys it did not have before.
        for name in ("system.reg", "user.reg"):
            backup = target / f"{name}.before-ni-link"
            if not backup.exists():
                shutil.copy2(target / name, backup)
        state = _load_state()
        entry = state.setdefault(str(prefix.resolve()), {}).setdefault(str(target.resolve()), {})
        if "machine_guid" not in entry:
            entry["machine_guid"] = machine_guid(dst_system)
            entry["machine_keys"] = [
                k for k in _MACHINE_KEYS if not _present(dst_system_sections, k)
            ]
            entry["user_keys"] = [k for k in _USER_KEYS if not _present(dst_user_sections, k)]
        _save_state(state)

    # HKLM follows the Native Access prefix on every sync.  HKCU holds the
    # user's settings, which the DAW prefix keeps once it has them.
    machine = [k for k in _MACHINE_KEYS if _present(src_system_sections, k)]
    user = [
        k for k in _USER_KEYS
        if _present(src_user_sections, k) and not _present(dst_user_sections, k)
    ]
    new_system = _set_machine_guid(_merge(src_system, dst_system, machine), guid)
    if new_system != dst_system:
        _write(target / "system.reg", new_system)
    if user:
        _write(target / "user.reg", _merge(src_user, dst_user, user))

    info(
        f"{target}: {len(made)} new link(s)"
        + (f", {len(gone)} removed" if gone else "")
        + f", registry and MachineGuid {guid} synced"
    )


def run_link(prefix: Path, target: Path, *, assume_yes: bool = False) -> None:
    target = target.expanduser()
    _check_prefixes(prefix, target)

    first = str(target.resolve()) not in _load_state().get(str(prefix.resolve()), {})
    if first and not assume_yes:
        print()
        print(f"This makes the products installed in {prefix} available in {target}.")
        print(f"{target} gets this prefix's MachineGuid, so software activated")
        print("there may ask to be activated again. `ni unlink` restores it.")
        print()
        try:
            answer = input("Continue? [y/N] ")
        except EOFError:
            answer = ""
        if answer.strip().lower() not in ("y", "yes"):
            info("aborted")
            return

    sync(prefix, target, first=first)
    info("done — rescan plugins in the DAW")


def run_unlink(prefix: Path, target: Path) -> None:
    target = target.expanduser()
    state = _load_state()
    links = state.get(str(prefix.resolve()), {})
    entry = links.get(str(target.resolve()))
    if entry is None:
        die(f"{target} is not linked to {prefix}")
    if not (target / "system.reg").is_file():
        del links[str(target.resolve())]
        _save_state(state)
        info(f"{target} no longer exists; forgot it")
        return
    if _busy(target):
        die(f"{target} is in use — close the DAW (and anything else running in it) first")

    source_c = config.drive_c(prefix).resolve()
    links_removed = 0
    for link in list(_our_links(config.drive_c(target), source_c)):
        link.unlink()
        links_removed += 1

    system = _drop(_read(target / "system.reg"), entry.get("machine_keys", []))
    if entry.get("machine_guid"):
        system = _set_machine_guid(system, entry["machine_guid"])
    _write(target / "system.reg", system)
    _write(target / "user.reg", _drop(_read(target / "user.reg"), entry.get("user_keys", [])))

    del links[str(target.resolve())]
    if not links:
        state.pop(str(prefix.resolve()), None)
    _save_state(state)
    info(f"{target}: {links_removed} link(s) removed, own MachineGuid restored")


def sync_all(prefix: Path) -> None:
    """Re-sync every linked prefix; called after Native Access closes."""
    targets = linked_prefixes(prefix)
    if not targets:
        return
    # wineserver writes the registry to disk every 30 s; what the daemon
    # recorded for a fresh install may not be in system.reg yet.
    info("updating linked prefixes...")
    time.sleep(31)
    for target in targets:
        if not (target / "system.reg").is_file():
            warn(f"linked prefix {target} is gone — `ni unlink {target}` to forget it")
        elif _busy(target):
            warn(f"{target} is in use — close the DAW and run `ni link {target}`")
        else:
            sync(prefix, target)

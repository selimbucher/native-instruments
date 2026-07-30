"""Small shared helpers: logging, downloads, safe deletion."""

from __future__ import annotations

import email.utils
import os
import shutil
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import NoReturn

from . import config


def info(msg: str) -> None:
    print(f"==> {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"warning: {msg}", file=sys.stderr, flush=True)


def die(msg: str, code: int = 1) -> NoReturn:
    print(f"error: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(code)


def which_first(*names: str) -> str | None:
    """First of *names* found on PATH."""
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    return None


def download(url: str, dest: Path, label: str | None = None) -> Path:
    """Download *url* to *dest* with a progress bar.

    If *dest* already exists, an If-Modified-Since request avoids
    re-downloading an unchanged file (same behaviour as `curl -z`).
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    # No custom User-Agent: Akamai (fronting native-instruments.com) 403s
    # unrecognized UA strings but lets urllib's default through.
    request = urllib.request.Request(url)
    if dest.exists():
        mtime = dest.stat().st_mtime
        request.add_header(
            "If-Modified-Since", email.utils.formatdate(mtime, usegmt=True)
        )

    try:
        response = urllib.request.urlopen(request, timeout=60)
    except urllib.error.HTTPError as exc:
        if exc.code == 304:
            info(f"{label or dest.name}: cached copy is up to date")
            return dest
        raise

    total = int(response.headers.get("Content-Length") or 0)
    part = dest.with_suffix(dest.suffix + ".part")
    received = 0
    show_bar = sys.stderr.isatty()
    with open(part, "wb") as out:
        while chunk := response.read(1024 * 256):
            out.write(chunk)
            received += len(chunk)
            if show_bar:
                if total:
                    pct = received * 100 // total
                    bar = "#" * (pct // 4)
                    sys.stderr.write(f"\r    [{bar:<25}] {pct:3d}%  {received // 2**20} MiB")
                else:
                    sys.stderr.write(f"\r    {received // 2**20} MiB")
                sys.stderr.flush()
    if show_bar:
        sys.stderr.write("\n")

    # Preserve the server's Last-Modified so future If-Modified-Since checks work.
    last_modified = response.headers.get("Last-Modified")
    part.replace(dest)
    if last_modified:
        parsed = email.utils.parsedate_to_datetime(last_modified)
        stamp = parsed.timestamp()
        os.utime(dest, (stamp, stamp))
    return dest


def guarded_rmtree(path: Path) -> None:
    """Delete a directory tree, refusing implausible targets.

    Only paths under the user's home that are at least two components deep
    and are not the home directory itself are accepted.  This is a backstop
    against bugs of the "rm -rf $UNSET_VAR/" family.
    """
    resolved = path.expanduser().resolve()
    home = Path.home().resolve()
    if resolved == home or resolved == Path("/"):
        raise RuntimeError(f"refusing to delete {resolved}")
    if home not in resolved.parents and not str(resolved).startswith(
        str(config.cache_dir().resolve())
    ):
        raise RuntimeError(f"refusing to delete {resolved}: outside home")
    if len(resolved.parts) < 3:
        raise RuntimeError(f"refusing to delete {resolved}: path too shallow")
    shutil.rmtree(resolved, ignore_errors=True)
    if resolved.exists():
        # A partial wipe (e.g. files still held open by a live process) must
        # not pass silently: setup on top of prefix remnants breaks Wine.
        raise RuntimeError(f"failed to fully delete {resolved}")

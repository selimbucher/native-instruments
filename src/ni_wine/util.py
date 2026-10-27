from __future__ import annotations

import email.utils
import os
import shutil
import subprocess
import sys
import textwrap
import urllib.error
import urllib.request
from pathlib import Path
from typing import NoReturn

from . import config


def info(msg: str) -> None:
    print(f"==> {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"warning: {msg}", file=sys.stderr, flush=True)


def _wrap(msg: str, width: int = 72) -> str:
    """Wrap long lines, keep existing line breaks and indentation, never split words."""
    lines = []
    for line in msg.splitlines():
        indent = line[: len(line) - len(line.lstrip())]
        lines.append(
            textwrap.fill(
                line,
                width,
                subsequent_indent=indent,
                break_long_words=False,
                break_on_hyphens=False,
            )
            if len(line) > width
            else line
        )
    return "\n".join(lines)


def _error_dialog(msg: str) -> None:
    tool = which_first("zenity", "yad")
    if not tool:
        return
    # otherwise the dialog gets as wide as the longest line
    msg = _wrap(msg)
    if tool.endswith("zenity"):
        args = [tool, "--error", "--no-markup", "--title=ni-wine", f"--text={msg}"]
    else:
        # --name/--class/--window-icon: taskbars show it as Native Access.
        # yad unescapes --text even with --no-markup, so double the backslashes.
        text = msg.replace("\\", "\\\\")
        args = [
            tool,
            "--image=dialog-error",
            "--no-markup",
            "--title=ni-wine",
            "--text-width=72",
            f"--name={config.APP_ID}",
            f"--class={config.APP_ID}",
            f"--window-icon={config.APP_ID}",
            f"--text={text}",
            "--button=Close",
        ]
    try:
        subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    except (OSError, subprocess.TimeoutExpired):
        pass


def die(msg: str, code: int = 1) -> NoReturn:
    print(f"error: {msg}", file=sys.stderr, flush=True)
    # started from the .desktop entry (NI_WINE_GUI=1) there is no console
    if os.environ.get("NI_WINE_GUI"):
        _error_dialog(msg)
    raise SystemExit(code)


def which_first(*names: str) -> str | None:
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    return None


def download(url: str, dest: Path, label: str | None = None) -> Path:
    """Download url to dest with a progress bar, If-Modified-Since like curl -z."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    # no custom User-Agent: Akamai in front of native-instruments.com 403s
    # unknown UA strings but lets urllib's default through
    request = urllib.request.Request(url)
    if dest.exists():
        mtime = dest.stat().st_mtime
        request.add_header("If-Modified-Since", email.utils.formatdate(mtime, usegmt=True))

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

    # keep the server's Last-Modified for the next If-Modified-Since
    last_modified = response.headers.get("Last-Modified")
    part.replace(dest)
    if last_modified:
        parsed = email.utils.parsedate_to_datetime(last_modified)
        stamp = parsed.timestamp()
        os.utime(dest, (stamp, stamp))
    return dest


def guarded_rmtree(path: Path, *, prefix: Path) -> None:
    """Delete the prefix or a tree inside it. Refuses /, $HOME, shallow paths, anything outside the
    prefix and a prefix without drive_c, a backstop against rm -rf $UNSET/ bugs.
    """
    resolved = path.expanduser().resolve()
    root = prefix.expanduser().resolve()
    if not resolved.is_relative_to(root):
        raise RuntimeError(f"refusing to delete {resolved}: outside the prefix {root}")
    if root in (Path.home().resolve(), Path("/")) or len(root.parts) < 3:
        raise RuntimeError(f"refusing to delete {resolved}: implausible prefix {root}")
    if not resolved.exists():
        return
    if not config.drive_c(root).is_dir():
        raise RuntimeError(f"refusing to delete {resolved}: {root} is not a Wine prefix")
    shutil.rmtree(resolved, ignore_errors=True)
    if resolved.exists():
        # partial wipe (files still held open), setup on top of remnants breaks Wine
        raise RuntimeError(f"failed to fully delete {resolved}")

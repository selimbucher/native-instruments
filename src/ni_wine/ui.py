"""Progress reporting: a GTK dialog via yad/zenity when available, else console."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
from types import TracebackType

from .util import info, warn

class Progress:
    """Context manager that shows setup progress.

    Usage:
        with Progress(title="Native Instruments Setup", enabled=True) as ui:
            ui.step("Initializing Wine prefix...", 5)
    """

    def __init__(self, title: str, enabled: bool = True) -> None:
        self._title = title
        self._enabled = enabled and bool(
            os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
        )
        self._proc: subprocess.Popen | None = None
        self._css_path: str | None = None

    def __enter__(self) -> "Progress":
        if not self._enabled:
            return self
        if shutil.which("yad"):
            # Follows the GTK theme, light or dark.  The one override: GTK
            # themes dim the bar's text, and that is where the step shows.
            css = tempfile.NamedTemporaryFile(
                "w", suffix=".css", delete=False, prefix="ni-wine-"
            )
            css.write("progressbar text { color: @theme_fg_color; }\n")
            css.close()
            self._css_path = css.name
            cmd = [
                "yad",
                "--progress",
                f"--title={self._title}",
                "--text=<b>Setting up Native Access</b>",
                "--align=left",
                "--auto-close",
                "--center",
                "--width=420",
                "--no-buttons",
                "--borders=20",
                f"--gtkrc={css.name}",
            ]
        elif shutil.which("zenity"):
            cmd = [
                "zenity",
                "--progress",
                f"--title={self._title}",
                "--text=Native Access Setup",
                "--percentage=0",
                "--auto-close",
                "--no-cancel",
                "--width=480",
            ]
        else:
            return self
        try:
            self._proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
        except OSError:
            self._proc = None
            return self
        # A dialog that rejects an option dies at once; say so instead of
        # silently reporting to nobody.
        time.sleep(0.3)
        if self._proc.poll() is not None:
            err = (self._proc.stderr.read() if self._proc.stderr else "").strip()
            warn(f"{cmd[0]} progress dialog failed ({err or 'exited'}); "
                 "reporting progress on the console")
            self._forget_dialog()
        return self

    def step(self, message: str, percent: int) -> None:
        info(message)
        if self._proc and self._proc.stdin:
            try:
                self._proc.stdin.write(f"# {message}\n{percent}\n")
                self._proc.stdin.flush()
            except (BrokenPipeError, OSError):
                # Dialog gone (closed by the user, or yad/zenity exited):
                # keep going on the console.  Dropping the pipe explicitly
                # keeps Python from re-flushing it at exit and printing a
                # "Exception ignored ... BrokenPipeError" traceback.
                self._forget_dialog()

    def _forget_dialog(self) -> None:
        proc, self._proc = self._proc, None
        if proc is None:
            return
        if proc.stdin:
            try:
                proc.stdin.close()
            except OSError:
                pass
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self._forget_dialog()
        if self._css_path:
            try:
                os.unlink(self._css_path)
            except OSError:
                pass

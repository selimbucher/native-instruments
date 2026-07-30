"""Progress reporting: a GTK dialog via yad/zenity when available, else console."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from types import TracebackType

from .util import info

_YAD_CSS = """\
window, .dialog {
  background-color: #1a1a1a;
  color: #ffffff;
}
label {
  color: #ffffff;
  margin-bottom: 8px;
}
progressbar trough {
  background-color: #333333;
  border-radius: 4px;
}
progressbar progress {
  background-color: #ffffff;
  border-radius: 4px;
}
.dialog-action-area {
  margin: 0;
  padding: 0;
}
"""


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
            css = tempfile.NamedTemporaryFile(
                "w", suffix=".css", delete=False, prefix="ni-wine-"
            )
            css.write(_YAD_CSS)
            css.close()
            self._css_path = css.name
            cmd = [
                "yad",
                "--progress",
                f"--title={self._title}",
                "--text=Native Access Setup",
                "--percentage=0",
                "--auto-close",
                "--center",
                "--width=480",
                "--no-buttons",
                "--borders=16",
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
                stderr=subprocess.DEVNULL,
                text=True,
            )
        except OSError:
            self._proc = None
        return self

    def step(self, message: str, percent: int) -> None:
        info(message)
        if self._proc and self._proc.stdin:
            try:
                self._proc.stdin.write(f"# {message}\n{percent}\n")
                self._proc.stdin.flush()
            except (BrokenPipeError, OSError):
                # Dialog was closed by the user; keep going on the console.
                self._proc = None

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        if self._proc:
            try:
                if self._proc.stdin:
                    self._proc.stdin.close()
                self._proc.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                self._proc.kill()
        if self._css_path:
            try:
                os.unlink(self._css_path)
            except OSError:
                pass

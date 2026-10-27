"""Progress reporting: a GTK dialog via yad/zenity when available, else console."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
from types import TracebackType

from . import config
from .util import info, warn


class Progress:
    def __init__(self, title: str, enabled: bool = True) -> None:
        self._title = title
        self._enabled = enabled and bool(
            os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
        )
        self._proc: subprocess.Popen | None = None
        self._css_path: str | None = None

    def __enter__(self) -> Progress:
        if not self._enabled:
            return self
        if shutil.which("yad"):
            # GTK themes dim the progress bar text, which is where the step shows
            css = tempfile.NamedTemporaryFile("w", suffix=".css", delete=False, prefix="ni-wine-")
            css.write("progressbar text { color: @theme_fg_color; }\n")
            css.close()
            self._css_path = css.name
            cmd = [
                "yad",
                "--progress",
                f"--title={self._title}",
                "--text=<b>Setting up Native Access</b>",
                "--align=left",
                # without this the label's minimum height is computed wrapped
                # and the window ends up twice as tall as its content
                "--text-width=40",
                # window class, so taskbars show the Native Access icon, not yad's
                f"--name={config.APP_ID}",
                f"--class={config.APP_ID}",
                f"--window-icon={config.APP_ID}",
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
        # a yad that rejects an option exits right away, notice that
        time.sleep(0.3)
        if self._proc.poll() is not None:
            err = (self._proc.stderr.read() if self._proc.stderr else "").strip()
            warn(
                f"{cmd[0]} progress dialog failed ({err or 'exited'}); "
                "reporting progress on the console"
            )
            self._forget_dialog()
        return self

    def step(self, message: str, percent: int) -> None:
        info(message)
        if self._proc and self._proc.stdin:
            try:
                self._proc.stdin.write(f"# {message}\n{percent}\n")
                self._proc.stdin.flush()
            except (BrokenPipeError, OSError):
                # dialog gone (closed or crashed), continue on the console.
                # Closing the pipe here avoids a BrokenPipeError traceback at exit.
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

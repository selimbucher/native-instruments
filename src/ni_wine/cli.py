"""Command-line interface: the `ni` and `native-access` entry points."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import __version__, config

_EPILOG = """\
examples:
  ni launch          start Native Access (first run sets the prefix up);
                     `native-access` is the same command
  ni doctor --fix    diagnose and repair common problems

Kontakt 8 is installed, updated and removed in Native Access like every
other product.

environment:
  NI_WINE_PREFIX   Wine prefix location (default: ~/.wine-ni)
  WINE             wine binary to use (default: first on PATH)
  NI_WINE_DEBUG    set to keep Wine debug output when launching
"""


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ni",
        description="Native Instruments software under Wine — setup, launch, and product management.",
        epilog=_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "-V", "--version", action="version", version=f"ni-wine {__version__}"
    )
    parser.add_argument(
        "--prefix",
        type=Path,
        default=None,
        metavar="PATH",
        help="Wine prefix to operate on (default: ~/.wine-ni or $NI_WINE_PREFIX)",
    )

    commands = parser.add_subparsers(dest="command", metavar="<command>", required=True)

    setup = commands.add_parser(
        "setup", help="create the Wine prefix and install Native Access"
    )
    setup.add_argument(
        "--no-ui", action="store_true", help="print progress to the console only"
    )

    launch = commands.add_parser(
        "launch", help="start Native Access (sets the prefix up first if needed)"
    )
    launch.add_argument(
        "url",
        nargs="?",
        default=None,
        help="native-access:// URL to forward (the browser login callback)",
    )

    reinstall = commands.add_parser(
        "reinstall", help="wipe the Wine prefix and set everything up again"
    )
    reinstall.add_argument(
        "--yes", action="store_true", help="skip the confirmation prompt"
    )

    # Internal: what the hook script calls from inside Wine.  Unlisted.
    apply = commands.add_parser("apply-installer")
    apply.add_argument("package")

    commands.add_parser(
        "fix-msvcp140",
        help="replace Wine's msvcp140 stubs with the real VC++ runtime DLLs",
    )

    doctor = commands.add_parser(
        "doctor", help="check dependencies, prefix health, and login-URL wiring"
    )
    doctor.add_argument(
        "--fix", action="store_true", help="repair everything repairable"
    )

    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    prefix = (args.prefix or config.default_prefix()).expanduser()

    # Imports are deferred so `ni --help` stays instant.
    if args.command == "setup":
        from .setup_cmd import run_setup

        run_setup(prefix, ui=not args.no_ui)
        return 0
    if args.command == "launch":
        from .launch import run_launch

        return run_launch(prefix, url=args.url)
    if args.command == "reinstall":
        from .launch import run_reinstall

        run_reinstall(prefix, assume_yes=args.yes)
        return 0
    if args.command == "apply-installer":
        from .kontakt import apply_installer

        return apply_installer(prefix, args.package)
    if args.command == "fix-msvcp140":
        from .msvcp140 import fix_msvcp140
        from .wine import Wine

        fix_msvcp140(Wine(prefix))
        return 0
    if args.command == "doctor":
        from .doctor import run_doctor

        return run_doctor(prefix, fix=args.fix)
    raise AssertionError(f"unhandled command {args.command!r}")


def native_access_main(argv: list[str] | None = None) -> int:
    """Entry point for `native-access`: the desktop entry, the browser login
    callback, and the command a user types.  Identical to `ni launch`.
    """
    parser = argparse.ArgumentParser(
        prog="native-access",
        description="Start Native Access under Wine (the same as `ni launch`).",
    )
    parser.add_argument(
        "-V", "--version", action="version", version=f"ni-wine {__version__}"
    )
    parser.add_argument(
        "--prefix",
        type=Path,
        default=None,
        metavar="PATH",
        help="Wine prefix to use (default: ~/.wine-ni or $NI_WINE_PREFIX)",
    )
    parser.add_argument("--reinstall", action="store_true", help=argparse.SUPPRESS)  # old spelling of `ni reinstall`
    parser.add_argument("url", nargs="?", default=None, help="native-access:// callback URL")
    args = parser.parse_args(argv)

    ni_args = ["--prefix", str(args.prefix)] if args.prefix else []
    if args.reinstall:
        return main([*ni_args, "reinstall"])
    return main([*ni_args, "launch", *([args.url] if args.url else [])])


def entry() -> None:
    sys.exit(main())


def native_access_entry() -> None:
    sys.exit(native_access_main())


if __name__ == "__main__":
    entry()

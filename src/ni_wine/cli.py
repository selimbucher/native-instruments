"""Command-line interface: the `ni` and `native-access` entry points."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import __version__, config

_EPILOG = """\
examples:
  ni setup                       first-time setup (Wine prefix + Native Access)
  ni launch                      start Native Access (also: `native-access`)
  ni kontakt8 install            install Kontakt 8 (opens browser to fetch the download)
  ni kontakt8 update <url>       update Kontakt 8 from a known installer URL
  ni doctor --fix                diagnose and repair common problems

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
        "launch", help="launch Native Access (runs setup first if needed)"
    )
    launch.add_argument(
        "url",
        nargs="?",
        default=None,
        help="native-access:// URL to forward (used by the browser login callback)",
    )

    reinstall = commands.add_parser(
        "reinstall", help="wipe the Wine prefix and set everything up again"
    )
    reinstall.add_argument(
        "--yes", action="store_true", help="skip the confirmation prompt"
    )

    kontakt8 = commands.add_parser("kontakt8", help="manage the Kontakt 8 install")
    kontakt8_commands = kontakt8.add_subparsers(
        dest="kontakt8_command", metavar="<action>", required=True
    )
    k8_install = kontakt8_commands.add_parser("install", help="install Kontakt 8")
    k8_install.add_argument(
        "url", nargs="?", default=None, help="installer zip URL (else captured via browser)"
    )
    k8_update = kontakt8_commands.add_parser("update", help="update Kontakt 8")
    k8_update.add_argument(
        "url", nargs="?", default=None, help="installer zip URL (else captured via browser)"
    )
    kontakt8_commands.add_parser("uninstall", help="remove Kontakt 8 from the prefix")

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
    if args.command == "kontakt8":
        from . import kontakt
        from .launch import ensure_prefix_exists

        ensure_prefix_exists(prefix)
        if args.kontakt8_command == "install":
            kontakt.install(prefix, args.url)
        elif args.kontakt8_command == "update":
            kontakt.update(prefix, args.url)
        else:
            kontakt.uninstall(prefix)
        return 0
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
    """Entry point for the `native-access` desktop launcher.

    Accepts an optional native-access:// URL (browser login callback) and
    keeps the old `--reinstall` flag working.
    """
    parser = argparse.ArgumentParser(
        prog="native-access",
        description="Launch Native Access under Wine (shortcut for `ni launch`).",
    )
    parser.add_argument(
        "-V", "--version", action="version", version=f"ni-wine {__version__}"
    )
    parser.add_argument("--reinstall", action="store_true", help="wipe the prefix and set up again")
    parser.add_argument("url", nargs="?", default=None, help="native-access:// callback URL")
    args = parser.parse_args(argv)

    prefix = config.default_prefix()
    if args.reinstall:
        from .launch import run_reinstall

        run_reinstall(prefix)
        return 0
    from .launch import run_launch

    return run_launch(prefix, url=args.url)


def entry() -> None:
    sys.exit(main())


def native_access_entry() -> None:
    sys.exit(native_access_main())


if __name__ == "__main__":
    entry()

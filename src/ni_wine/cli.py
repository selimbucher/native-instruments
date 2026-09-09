"""Command-line interface: the `ni` and `native-access` entry points."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import __version__, config

_EPILOG = """\
`native-access` starts Native Access (and sets the prefix up the first time);
`ni` is for maintenance.

examples:
  ni setup                       redo the first-time setup (Wine prefix + Native Access)
  ni kontakt8 update             update Kontakt 8 through Native Access
  ni kontakt8 update <file|url>  update Kontakt 8 from a downloaded installer
  ni kontakt8 hook status        state of the Kontakt installer hook (msi shim)
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

    # Unlisted alias of `native-access`, kept for scripts and old desktop entries.
    launch = commands.add_parser("launch")
    launch.add_argument("url", nargs="?", default=None)

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
    source_help = (
        "installer zip/exe: a local file or URL (default: through Native Access — "
        "click Install/Update there)"
    )
    k8_install = kontakt8_commands.add_parser("install", help="install Kontakt 8")
    k8_install.add_argument("source", nargs="?", default=None, metavar="file|url", help=source_help)
    k8_update = kontakt8_commands.add_parser("update", help="update Kontakt 8")
    k8_update.add_argument("source", nargs="?", default=None, metavar="file|url", help=source_help)
    kontakt8_commands.add_parser("uninstall", help="remove Kontakt 8 from the prefix")
    hook = kontakt8_commands.add_parser(
        "hook",
        help="the installer hook (msi shim) that lets Native Access install Kontakt",
    )
    hook.add_argument(
        "hook_action",
        choices=("status", "install", "remove"),
        metavar="status|install|remove",
        help="show, install/refresh, or remove the hook (remove = stock Wine msi)",
    )
    # Internal: what the hook script calls from inside Wine.
    apply = kontakt8_commands.add_parser("apply-installer")  # unlisted on purpose
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


def _kontakt8(args: argparse.Namespace, prefix: Path) -> int:
    from . import kontakt, msishim
    from .launch import ensure_prefix_exists
    from .util import die, info
    from .wine import Wine

    ensure_prefix_exists(prefix)
    action = args.kontakt8_command
    if action == "install":
        kontakt.install(prefix, args.source)
    elif action == "update":
        kontakt.update(prefix, args.source)
    elif action == "uninstall":
        kontakt.uninstall(prefix)
    elif action == "apply-installer":
        return kontakt.apply_installer(prefix, args.package)
    elif action == "hook":
        wine = Wine(prefix)
        if args.hook_action == "install":
            problem = msishim.ensure(wine, prefix)
            if problem:
                die(problem)
        elif args.hook_action == "remove":
            msishim.remove(wine, prefix)
        info(f"Kontakt installer hook: {msishim.describe(prefix, wine)}")
    return 0


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
        return _kontakt8(args, prefix)
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
    """Entry point for `native-access`: the desktop entry, the browser
    login callback, and the command a user types.  Sets the prefix up on
    first use, then launches Native Access.
    """
    parser = argparse.ArgumentParser(
        prog="native-access",
        description="Start Native Access under Wine (first run sets everything up).",
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
    parser.add_argument("--reinstall", action="store_true", help="wipe the prefix and set up again")
    parser.add_argument("url", nargs="?", default=None, help="native-access:// callback URL")
    args = parser.parse_args(argv)

    prefix = (args.prefix or config.default_prefix()).expanduser()
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

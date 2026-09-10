# Native Access & Kontakt 8 on Linux

Run [Native Access](https://www.native-instruments.com/en/specials/native-access-2/)
and Native Instruments products under Wine. Kontakt 8 included: its
installer hangs in Wine's MSI engine, so ni-wine takes over that one step
and Native Access installs, updates and activates Kontakt like any other
product.

![Native Access running under Wine on Linux](docs/screenshot.png)

**This repository does NOT contain, grant access to, or distribute any
software from Native Instruments in any way.** It only provides scripts and
instructions for installing software you have legitimately obtained from
Native Instruments and you need your own account to download and use
plugins and instruments.

## Install

The CLI is pure Python ≥ 3.11 with no pip packages. Runtime dependencies:

| dependency | purpose |
|---|---|
| wine (staging recommended, WoW64 fine) | runs everything |
| winetricks | vcrun2022 + PowerShell during setup |
| cabextract | msvcp140 fix |
| 7z (`7zip` package; binary `7z`/`7zz`/`7za`) | reads the Kontakt installer |
| msitools (`msidump`) | reads the Kontakt installer |
| procps (`pgrep`) | process checks |
| Xvfb | hides installer windows during setup |
| zenity or yad | setup progress dialog |
| xdotool (optional) | repositions off-screen windows on X11 |

Building from source needs a 32-bit mingw-w64 compiler as well (see
[Kontakt 8](#kontakt-8) for what it compiles).

### Debian / Ubuntu

```sh
sudo apt install winetricks cabextract 7zip msitools xvfb zenity procps pipx gcc-mingw-w64-i686
# Debian 12's wine (8.0) is too old, use the WineHQ repo (winehq-staging).
# Debian keeps winetricks in "contrib"; enable that component.
pipx install git+https://github.com/selimbucher/native-instruments
```

### Arch

Install [`ni-wine` from the AUR](https://aur.archlinux.org/packages/ni-wine),
all dependencies are pulled in automatically:

```sh
yay -S ni-wine
# recommended: the staging Wine build (provides `wine`, new-WoW64, no multilib)
sudo pacman -S wine-staging
```

### NixOS

```sh
nix profile install github:selimbucher/native-instruments
# or add the flake's packages.x86_64-linux.default to your system config
```

## Usage

Start **Native Access** from your app launcher, or run `native-access`.
The first start creates the Wine prefix (`~/.wine-ni`) and installs Native
Access, which takes a few minutes. Log in, install products.

Upgrading ni-wine needs nothing else. The next start adapts an existing
prefix.

Everything else is under `ni`:

```
usage: ni [-h] [-V] [--prefix PATH] <command> ...

  launch [url]     start Native Access (first run sets the prefix up)
  setup            redo the first-time setup (e.g. with `--no-ui`)
  reinstall        wipe the Wine prefix and set everything up again
  doctor [--fix]   check dependencies, prefix health, login-URL wiring
  hook <action>    status | install | remove the Kontakt installer hook
  fix-msvcp140     replace Wine's msvcp140 stubs with the real DLLs
```

`native-access` is `ni launch` under the name the desktop entry and the
login callback use. Every command supports `--help`.

Environment: `NI_WINE_PREFIX` (prefix location, default `~/.wine-ni`),
`WINE` (wine binary override), `NI_WINE_DEBUG` (keep Wine debug output).

## Kontakt 8

Install, update and remove it in Native Access. Close your DAW first:
ni-wine will not replace Kontakt's files while yabridge hosts are using the
prefix, and Native Access then shows Kontakt as not installed until you
try again with the DAW closed.

Kontakt's installer (InstallAware) opens its MSI as a database, rewrites
the component table at runtime and calls `MsiInstallProduct` once. Wine's
MSI engine never returns from that call. Instead of tracing that bug in
Wine, ni-wine puts a small forwarding `msi.dll` into the prefix's
`syswow64` and registers it for the Kontakt installer process only. Every
MSI call passes through to Wine's own msi except that one, which ni-wine
answers by copying the payload the installer already extracted and writing
the registry values the MSI would have written. The installer, and Native
Access, see a normal successful install.

The DLL is about 300 lines of C, built from source at package time and
never checked in. `shim/README.md` explains what it does and how to verify
the binary. `ni hook remove` restores the stock prefix.

## Troubleshooting

`ni doctor` checks the usual suspects and `ni doctor --fix` repairs what it
can. Logs:

- Native Access and its daemon:
  `~/.wine-ni/drive_c/users/Public/Documents/Native Instruments/Logs/`
- Kontakt installs: `~/.local/state/ni-wine/msi-shim.log` and
  `msi-hook.log`
- The Activate button and other URL opens from inside the prefix:
  `~/.local/state/ni-wine/open-url.log`

### Kontakt opens as "Kontakt 8 Demo"

Activation is done by Native Access's daemon, a few seconds after an
install finishes, and stored as a signed licence file per product under
`users/Public/Documents/Native Instruments/Native Access/ras3/` in the
prefix. Kontakt checks that file itself at start. No daemon, no network
and no particular Wine build is needed once it exists, and it survives
reboots and Kontakt updates.

If Kontakt shows the demo dialog anyway:

- You opened it within seconds of the install. Close and reopen it.
- The prefix was recreated. The licence is bound to the prefix's machine
  identity, so open Native Access once; the daemon activates again.
- A daemon from an earlier, deleted prefix is still running. Native
  Access talks to whichever daemon holds the localhost ports, so a
  leftover one answers for the new prefix with the old machine identity:
  you look logged in, but the licences it issues do not match.
  `native-access` refuses to start in that state and names the process;
  `ni doctor` reports it too. Stop it and start Native Access again.

The dialog's **Activate** button opens Native Access's "Add Serial"
dialog, as it does on Windows. It does not re-activate anything.

### Native Access says Kontakt is not installed

After a refresh, on a Kontakt installed by an older ni-wine: the daemon
judges installs by registry values that only the MSI wrote. Starting
`native-access` once writes them. During an install: your DAW was open,
see above.

### "Please grant permission to Native Access to install dependencies"

Native Access shows this when its NTK daemon (a Windows service) isn't
running at startup and its own attempt to reinstall it fails. NA elevates
that install through `powershell.exe Start-Process ... -Verb runAs`, and
the `profile.ps1` from the winetricks PowerShell wrapper replaces
`Start-Process` with a shim that rejects the quoted path NA passes.
Replacing `elevate.exe` or setting `EnableLUA` does nothing; NA doesn't
use either for this.

ni-wine installs its own `profile.ps1` (the original is kept as
`profile.ps1.winetricks`), installs the daemon during setup and starts the
service before every launch. If the daemon can't be started, `ni launch`
stops with an explanation instead of starting NA into the stuck screen.
Always start NA via `native-access`, not `wine "Native Access.exe"`.

### Native Access spins forever

It has no offline mode. ni-wine checks the connection first and says so.
Installed instruments and plugins work offline.

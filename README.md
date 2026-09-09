# Native Access & Kontakt 8 on Linux

Run [Native Access](https://www.native-instruments.com/en/specials/native-access-2/)
and Native Instruments products under Wine on Linux — including Kontakt 8,
whose official installer does not work under Wine on its own.

![Native Access running under Wine on Linux](docs/screenshot.png)

- **One-command setup**: a dedicated Wine prefix (`~/.wine-ni`) with the
  Wine tweaks Native Access needs, its NTK daemon installed and kept
  running, and the PowerShell profile that lets NA's own dependency
  install work.
- **Kontakt 8 through Native Access**: click Install or Update in Native
  Access like on Windows. Kontakt's installer hangs in Wine's MSI engine;
  ni-wine steps in at exactly that point (a small forwarding `msi.dll`,
  see below) and lays the files out itself, and the installer finishes
  normally. Native Access shows a successful install and stays in sync.

**This repository does NOT contain, grant access to, or distribute any
software from Native Instruments in any way.** It only provides scripts and
instructions for installing software you have legitimately obtained from
Native Instruments; you need your own account to download and use their
plugins and instruments.

## Install

Runtime dependencies (the CLI itself is pure Python ≥ 3.11 with no pip
packages). Building from source additionally needs a 32-bit mingw-w64
compiler for the msi shim.

| dependency | purpose |
|---|---|
| wine (staging recommended, WoW64 fine) | runs everything |
| winetricks | vcrun2022 + PowerShell during setup |
| cabextract | msvcp140 fix |
| 7z (`7zip` package; binary `7z`/`7zz`/`7za`) | reads the MSI inside the Kontakt installer |
| msitools (`msidump`) | reads the Kontakt installer's MSI tables |
| procps (`pgrep`) | process checks |
| Xvfb | hides installer windows during setup |
| zenity or yad | graphical setup progress |
| xdotool (optional) | repositions off-screen windows on X11 desktops |

### Debian / Ubuntu

```sh
sudo apt install winetricks cabextract 7zip msitools xvfb zenity procps pipx gcc-mingw-w64-i686
# Debian 12's wine (8.0) is too old — use the WineHQ repo (winehq-staging).
# Debian keeps winetricks in "contrib"; enable that component.
pipx install git+https://github.com/selimbucher/native-instruments
```

The pip build compiles the msi shim with `i686-w64-mingw32-gcc` (the
`gcc-mingw-w64-i686` package above) and refuses to build without it.

### Arch

Install [`ni-wine` from the AUR](https://aur.archlinux.org/packages/ni-wine)
— all dependencies are pulled in automatically:

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

That is all. Start **Native Access** from your app launcher (or run
`native-access`): the first start creates the Wine prefix and installs
Native Access, then log in and install products, Kontakt 8 included. The
commands below are for scripting and repair; none of them is a required
step.

## Usage

```
usage: ni [-h] [-V] [--prefix PATH] <command> ...

  launch [url]     start Native Access (first run sets the prefix up)
  setup            redo the first-time setup (e.g. with `--no-ui`)
  reinstall        wipe the Wine prefix and set everything up again
  doctor [--fix]   check dependencies, prefix health, login-URL wiring
  hook <action>    status | install | remove the Kontakt installer hook
  fix-msvcp140     replace Wine's msvcp140 stubs with the real DLLs
```

`native-access` is the same command as `ni launch` under the name the
desktop entry and the login callback use. Every command supports `--help`.

### Kontakt 8

Install, update and remove it in Native Access, like every other product.
Keep your DAW closed while it runs: the files being replaced may be in use
by bridged plugins, and ni-wine refuses to overwrite them while yabridge
hosts use the prefix (Native Access then shows Kontakt as not installed;
close the DAW and click again).

Why the extra machinery: Kontakt's InstallAware installer opens its MSI as
a database, rewrites the component table at runtime and then hands the
package to `MsiInstallProduct`; Wine's MSI engine never returns from that
call. ni-wine installs a small forwarding `msi.dll` into the prefix's
`syswow64` that is loaded only by the Kontakt installer process (a
per-application DllOverride). It passes every call through to Wine's real
msi except that one, which it answers by laying the files out from the
payload the installer already extracted. Read `shim/README.md` for what the
DLL does and how to verify it; it is built from source at package time and
`ni hook remove` puts the prefix back to stock.

Environment: `NI_WINE_PREFIX` (prefix location, default `~/.wine-ni`),
`WINE` (wine binary override), `NI_WINE_DEBUG` (keep Wine debug output).

## Activation

Products are activated by Native Access's daemon, which stores a signed
licence file per product under the prefix's
`users/Public/Documents/Native Instruments/Native Access/ras3/`. Kontakt
validates that file itself at launch — no daemon, no Native Access and no
network are needed once it exists, and it does not matter which Wine build
loads the plugin. The licence is bound to the prefix's machine identity
(`MachineGuid`): recreating the prefix means activating again in Native
Access, while updating Kontakt does not touch it.

Activation of a freshly installed product happens a few seconds after
Native Access reports the install done. Kontakt reads its licence only at
start, so if you open it within those seconds it shows the demo dialog;
close it and open it again.

The demo dialog's **Activate** button works like on Windows: it opens
Native Access's "Add Serial" dialog (starting Native Access if needed).
Native Access's installer registers the `native-access://` URL scheme under
a broken name when run under Wine, and Wine only honours machine-wide
scheme registrations, so ni-wine registers the scheme itself (in HKLM,
pointing at `~/.local/state/ni-wine/open-url.sh`, which runs `ni launch`
with the URL).

Every Native Access talks to *a* daemon through fixed localhost ports, not
to the one of its own prefix. A daemon left running from a deleted or
replaced prefix would therefore answer for the new one: you appear logged
in, but products get activated for the old prefix's machine identity and
show up as demo. `native-access` refuses to start while such a daemon runs
and names it; `ni doctor` reports it too.

## Offline behavior

Native Access has no offline mode. ni-wine detects the situation and tells
you up front instead of letting the app spin. Installed instruments and
plugins keep working offline.

## Troubleshooting

`ni doctor` diagnoses the common failure modes; `ni doctor --fix` repairs
the repairable ones. Native Access's own logs live at
`~/.wine-ni/drive_c/users/Public/Documents/Native Instruments/Logs/`; the
Kontakt installer hook logs to `~/.local/state/ni-wine/msi-shim.log` and
`msi-hook.log`; URL opens from inside the prefix (Kontakt's Activate
button) log to `open-url.log` next to them.

### "Please grant permission to Native Access to install dependencies"

Native Access shows this when its NTK daemon (a Windows service) isn't
running at startup and its own attempt to reinstall it fails. NA elevates
that install through `powershell.exe Start-Process … -Verb runAs`; the
`profile.ps1` shipped with the winetricks PowerShell wrapper replaces
`Start-Process` with a shim that rejects the quoted path NA passes, so the
install fails before anything runs. (Replacing `elevate.exe` or setting
`EnableLUA` does nothing — NA doesn't use either for this.) The same
profile also routes `Get-CimInstance` to a WMI shim that needs .NET 4.8,
which hangs the Native Access *installer* on its "already running?" check.

ni-wine installs its own `profile.ps1` (the original is kept as
`profile.ps1.winetricks`), installs the daemon during setup, and starts the
service before every launch; if the daemon can't be brought up, `ni launch`
stops with an explanation instead of starting NA into the stuck screen.
Existing prefixes get the profile on the next `ni launch` or
`ni doctor --fix`. Always start NA via `native-access` / `ni launch`, not
`wine "Native Access.exe"`, and run `ni doctor` if you see the screen anyway.

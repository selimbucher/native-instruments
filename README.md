# ni-wine

Run [Native Access](https://www.native-instruments.com/en/specials/native-access-2/)
and Native Instruments products under Wine on Linux — on any distro.

![Native Access running under Wine on Linux](docs/screenshot.png)

A single stdlib-only Python CLI replaces the pile of shell scripts this
project started as. It knows the Wine-specific traps and fixes them for you:

- **One-command setup**: dedicated Wine prefix (`~/.wine-ni`), vcrun2022,
  PowerShell, Native Access, NTKDaemon, and the msvcp140 DLL fix — installer
  windows hidden on a virtual display, dialogs auto-dismissed.
- **No floating tray window**: Wine's standalone systray window (useless on
  Hyprland & friends) is disabled via the registry, matched to your Wine
  version's mechanism.
- **Working browser login**: registers the `native-access://` URL scheme on
  both the Wine side (NA's installer registers a broken literal
  `${product.uri.scheme}`) and the Linux side (`x-scheme-handler` +
  `xdg-mime`), so the login callback actually reaches the app. `ni doctor
  --fix` can additionally pre-authorize the NI login origins in your
  Chromium-family browser so it never prompts.
- **Faster launches**: pre-starts NTKDaemon (otherwise NA silently
  *reinstalls the daemon on every launch* — Wine's `wmic` can't answer its
  version probe), and cleans up stale self-update downloads (~370 MB).
- **Kontakt 8 without the broken installer**: extracts the payload from the
  official installer by replaying its MSI file tables, and can fetch the
  authenticated download URL for you via a throwaway browser extension.

## Install

Runtime dependencies (the CLI itself is pure Python ≥ 3.11, no pip packages):

| dependency | required | purpose |
|---|---|---|
| wine (staging recommended, WoW64 fine) | yes | runs everything |
| winetricks | yes | vcrun2022 + PowerShell during setup |
| cabextract | yes | msvcp140 fix |
| 7z (`7zip` package; binary `7z`/`7zz`/`7za`) | yes | Kontakt installer extraction |
| msitools (`msidump`) | yes | Kontakt installer extraction |
| procps (`pgrep`) | yes | process checks |
| Xvfb | no | hides installer windows during setup |
| xdotool | no | auto-dismisses installer dialogs |
| zenity or yad | no | graphical setup progress |
| a Chromium-family browser | no | captures NI download URLs |

### Debian / Ubuntu

```sh
sudo apt install winetricks cabextract 7zip msitools xvfb xdotool zenity procps pipx
# Debian 12's wine (8.0) is too old — use the WineHQ repo (winehq-staging).
# Debian keeps winetricks in "contrib"; enable that component.
pipx install git+https://github.com/<you>/native-instruments
```

### Fedora

```sh
# Fedora's wine already carries staging patches.
sudo dnf install wine winetricks cabextract 7zip msitools xorg-x11-server-Xvfb xdotool zenity procps-ng pipx
pipx install git+https://github.com/<you>/native-instruments
```

### Arch

```sh
# Arch's wine/wine-staging are new-WoW64 builds — no multilib needed.
sudo pacman -S wine-staging winetricks cabextract 7zip msitools xorg-server-xvfb xdotool zenity python-pipx
pipx install git+https://github.com/<you>/native-instruments
```

### openSUSE Tumbleweed

```sh
sudo zypper install wine-staging winetricks cabextract 7zip msitools xorg-x11-server-Xvfb xdotool zenity python3-pipx
pipx install git+https://github.com/<you>/native-instruments
```

### NixOS

```sh
nix profile install github:<you>/native-instruments
# or add the flake's packages.x86_64-linux.default to your system config
```

The Nix package wraps all required tools into `PATH`; on other distros the
CLI probes the system. After installing, run `ni doctor` to verify, and
`ni doctor --fix` to finish desktop integration (launcher entry, icon, URL
handler).

## Usage

```
usage: ni [-h] [-V] [--prefix PATH] <command> ...

  setup                 create the Wine prefix and install Native Access
  launch [url]          launch Native Access (runs setup first if needed)
  reinstall             wipe the Wine prefix and set everything up again
  kontakt8 install [url]   install Kontakt 8
  kontakt8 update [url]    update Kontakt 8
  kontakt8 uninstall       remove Kontakt 8 from the prefix
  fix-msvcp140          replace Wine's msvcp140 stubs with the real DLLs
  doctor [--fix]        check dependencies, prefix health, login-URL wiring
```

`native-access` (the desktop launcher) is equivalent to `ni launch`.
Every command supports `--help`.

For `kontakt8 install`/`update` without a URL, a browser window opens on the
NI downloads page; log in and the download link is captured automatically
(the browser uses its own profile under `~/.local/state/ni-wine`, so your
login is remembered for next time).

Environment: `NI_WINE_PREFIX` (prefix location, default `~/.wine-ni`),
`WINE` (wine binary override), `NI_WINE_DEBUG` (keep Wine debug output).

## Offline behavior

Native Access has **no offline mode by design** — its product list requires
a live `api.native-instruments.com` call, so offline it will load to
"Loading products failed". That is NA architecture, not a Wine or ni-wine
bug. ni-wine detects the situation and tells you up front instead of
letting the app spin. Installed instruments and plugins keep working
offline (activations are cached locally).

## Troubleshooting

`ni doctor` diagnoses the common failure modes; `ni doctor --fix` repairs
the repairable ones. Native Access's own logs live at
`~/.wine-ni/drive_c/users/Public/Documents/Native Instruments/Logs/`.

If a browser login ends in the "Open Native Access?" prompt being denied,
just click the login button on the page again — a fresh click re-triggers
the prompt (automatic retries are blocked by the browser's popup
protection, which can look like "it never asks again").

## Migrating from the old flake

The old `ni-setup`, `ni-install --kontakt8/--update-kontakt8/...` commands
map to `ni setup`, `ni kontakt8 install/update/uninstall`, and
`ni fix-msvcp140`. Your existing prefix is picked up unchanged; the first
`ni launch` applies the new tray/login fixes to it automatically.

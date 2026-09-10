# Native Access & Kontakt 8 on Linux

[Native Access](https://www.native-instruments.com/en/specials/native-access-2/)
under Wine, set up with one command. Products install from Native Access
as on Windows, Kontakt 8 included.

![Native Access running under Wine on Linux](docs/screenshot.png)

**This repository does NOT contain, grant access to, or distribute any
software from Native Instruments in any way.** It only provides scripts and
instructions for installing software you have legitimately obtained from
Native Instruments and you need your own account to download and use
plugins and instruments.

## Install

### Arch

[`ni-wine` on the AUR](https://aur.archlinux.org/packages/ni-wine):

```sh
yay -S ni-wine
# recommended: the staging Wine build (provides `wine`, new-WoW64, no multilib)
sudo pacman -S wine-staging
```

### NixOS

As a flake input, with home-manager:

```nix
inputs.ni-wine.url = "github:selimbucher/native-instruments";
# ...
home.packages = [ inputs.ni-wine.packages.${pkgs.system}.default ];
```

Or without: `nix profile install github:selimbucher/native-instruments`.

### Debian / Ubuntu

```sh
sudo apt install winetricks cabextract 7zip msitools xvfb zenity procps pipx gcc-mingw-w64-i686
# Debian 12's wine (8.0) is too old, use the WineHQ repo (winehq-staging).
# Debian keeps winetricks in "contrib"; enable that component.
pipx install git+https://github.com/selimbucher/native-instruments
```

## Usage

Start **Native Access** from your app launcher or run `native-access`. The
first start sets up the Wine prefix at `~/.wine-ni` and installs Native
Access into it. Installed plugins land in that prefix.

Do not launch with something like `wine "Native Access.exe"`.

The cli:

```
usage: ni [-h] [-V] [--prefix PATH] <command> ...

  launch [url]     start Native Access (same as native-access)
  setup            redo the first-time setup (e.g. with `--no-ui`)
  reinstall        wipe the Wine prefix and set everything up again
  doctor [--fix]   check dependencies, prefix health, login-URL wiring
  fix-msvcp140     replace Wine's msvcp140 stubs with the real DLLs
```

Every command takes `--help`. Environment: `NI_WINE_PREFIX` (prefix
location, default `~/.wine-ni`), `WINE` (wine binary override),
`NI_WINE_DEBUG` (keep Wine debug output).

Kontakt 8's installer does not run under Wine's MSI engine; ni-wine steps
in at that point with a small `msi.dll` built from `shim/`, see
`shim/README.md` if you want to know what it does.

If something goes wrong, `ni doctor --fix`. Native Access logs to
`~/.wine-ni/drive_c/users/Public/Documents/Native Instruments/Logs/`,
ni-wine to `~/.local/state/ni-wine/`.

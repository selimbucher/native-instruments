# Native Access 2 & Kontakt 8 on Linux

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

```sh
yay -S ni-wine
```

### NixOS

As a flake input, with home-manager:

```nix
inputs.ni-wine.url = "github:selimbucher/native-instruments";
# ...
home.packages = [ inputs.ni-wine.packages.${pkgs.system}.default ];
```

Or without: `nix profile install github:selimbucher/native-instruments`.

### Fedora

```sh
sudo dnf copr enable selimbucher/ni-wine
sudo dnf install ni-wine
```

### Ubuntu (24.04 / 26.04)

```sh
sudo dpkg --add-architecture i386   # 24.04 only
wget -qO- https://dl.winehq.org/wine-builds/winehq.key | sudo gpg --dearmor -o /etc/apt/keyrings/winehq-archive.key
source /etc/os-release
sudo wget -NP /etc/apt/sources.list.d/ https://dl.winehq.org/wine-builds/ubuntu/dists/$UBUNTU_CODENAME/winehq-$UBUNTU_CODENAME.sources
sudo add-apt-repository ppa:selimbucher/ni-wine
sudo apt install ni-wine
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
  link PREFIX      use the installed products from a DAW's own prefix
  unlink PREFIX    undo `link`
  fix-msvcp140     replace Wine's msvcp140 stubs with the real DLLs
```

Every command takes `--help`. Environment: `NI_WINE_PREFIX` (prefix
location, default `~/.wine-ni`), `WINE` (wine binary override),
`NI_WINE_DEBUG` (keep Wine debug output).

Kontakt 8's installer does not run under Wine's MSI engine; ni-wine steps
in at that point with a small `msi.dll` built from `shim/`, see
`shim/README.md` if you want to know what it does.

### Linux DAWs (yabridge)

Use yabridge's development build, which has the fixes for Wine 10 and
newer. Its last release (5.1.1) needs Wine 9.21, which Native Access
doesn't run on.

- Arch: `yay -S yabridge-wine10-git yabridgectl-wine10-git`
- Fedora: `sudo dnf copr enable ycollet/audinux && sudo dnf install yabridge`
- NixOS: the `inputs.ni-wine.overlays.yabridge` overlay builds it against
  ni-wine's Wine (also set `inputs.ni-wine.inputs.nixpkgs.follows = "nixpkgs"`)
- Others: the [nightly build](https://nightly.link/robbert-vdh/yabridge/workflows/build/master)

Then:

```sh
yabridgectl add "$HOME/.wine-ni/drive_c/Program Files/Common Files/VST3"
yabridgectl sync
```

### Windows DAWs under Wine

To use the plugins in a DAW that runs in its own Wine prefix, close the
DAW and link its prefix:

```sh
ni link /path/to/daw-prefix
```

Then rescan plugins in the DAW. The DAW may ask to be activated again.
`ni unlink` undoes the link.

## Troubleshooting

If something goes wrong, `ni doctor --fix`. Native Access logs to
`~/.wine-ni/drive_c/users/Public/Documents/Native Instruments/Logs/`,
ni-wine to `~/.local/state/ni-wine/`. If that doesn't fix it,
[open an issue](https://github.com/selimbucher/native-instruments/issues)
with the `ni doctor` output.

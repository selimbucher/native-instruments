# msi shim

A 32-bit `msi.dll` that stands in for Wine's own msi inside the Kontakt
installer process, so that Native Access can install and update Kontakt
like any other product.

## Why

Kontakt's InstallAware installer opens its MSI as a database, rewrites the
component table at runtime and then calls `MsiInstallProductA` once. Under
Wine that call never returns: Wine's MSI engine wedges on the rewritten
table (a `msiexec.exe -Embedding` server sits blocked forever). Every other
NI installer is small enough to go through unaffected.

ni-wine already knows how to lay Kontakt's files out from the installer's
payload by replaying the MSI's Directory/Component/File tables. The shim
just connects that to the moment the installer would have run the MSI.

## What it does

- Exports the same 296 functions as Wine's `msi.dll` (list in
  `msi_exports.txt`, taken from Wine's `msi.spec`).
- 294 of them are a single `jmp` into Wine's real msi, loaded from
  `msi_wine.dll` in the same directory. No logic, no arguments touched.
- `MsiInstallProductA`/`W` check the package path against the `divert=`
  lines of `msi_shim.cfg`. No match: call Wine's real function. Match: run
  the configured hook script (`start.exe /unix <hook> <package>`), wait for
  the result file it writes, and return `ERROR_SUCCESS` for `OK` or
  `ERROR_INSTALL_FAILURE` (1603) otherwise. A hook that fails makes the
  installer report a failed install; nothing else is affected.
- The DLL only loads in a process whose `DllOverrides` says `msi=native`.
  ni-wine registers that for `Kontakt 8 Setup PC.exe` only; every other
  process keeps Wine's builtin (Wine prefers builtins by default and finds
  them outside the prefix).

The C is about 250 lines with no CRT, no network, no crypto. Imports are
`kernel32` and `user32` (for `wsprintfA` in the log line).

## Verify it yourself

Build it (any 32-bit mingw-w64 toolchain):

    make            # → msi_shim32.dll

Look at what came out:

    winedump -j export msi_shim32.dll      # 296 exports, original ordinals
    winedump -j import msi_shim32.dll      # KERNEL32.dll, USER32.dll
    objdump -d -M intel msi_shim32.dll | grep -A1 '<_MsiCloseHandle>:'
                                            # jmp DWORD PTR ds:0x...  (5 bytes)

`gen_forwarders.py` is the only place export names come from; `stubs32.S`
and `msi.def` are regenerated from `msi_exports.txt` on every build.

Packages build the DLL from these sources at package time (see
`flake.nix` and `packaging/aur/PKGBUILD`); no binary is checked in.

## Config file

`msi_shim.cfg` next to the DLL, written by ni-wine (`msishim.py`):

    divert=Kontakt 8 Setup PC.msi
    hook=/home/<user>/.local/state/ni-wine/msi-hook.sh
    result=Z:\home\<user>\.local\state\ni-wine\msi-hook.result
    log=Z:\home\<user>\.local\state\ni-wine\msi-shim.log
    timeout=7200

No config file means nothing is ever diverted.

## Kill switch

    ni kontakt8 hook remove

restores Wine's `msi.dll`, deletes `msi_wine.dll` and the config, and drops
the registry override. Deleting `msi_shim.cfg` alone also disarms it. A Wine
upgrade that rewrites the prefix's builtins removes the shim by itself;
`ni launch` puts it back.

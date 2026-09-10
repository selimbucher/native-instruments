# Builds msi_shim32.dll, ni-wine's forwarding msi.dll for the Kontakt installer.
# Needs a 32-bit mingw-w64 cross compiler (Arch: mingw-w64-gcc; Nix: the
# pkgsCross.mingw32 stdenv, see flake.nix).  No CRT is linked: the DLL uses
# kernel32 and user32 (wsprintfA) only.

CC = i686-w64-mingw32-gcc
PYTHON ?= python3
# Not CFLAGS: the DLL has no CRT, so the host's flags (makepkg exports LTO,
# stack protection, fortify) must not reach it.
SHIM_CFLAGS = -O2 -Wall -Wextra -fno-builtin -ffreestanding -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns -fno-lto

all: msi_shim32.dll

exports.h stubs32.S msi.def: gen_forwarders.py msi_exports.txt
	$(PYTHON) gen_forwarders.py

msi_shim.o: msi_shim.c exports.h
	$(CC) -m32 $(SHIM_CFLAGS) -c $< -o $@

stubs32.o: stubs32.S
	$(CC) -m32 -c $< -o $@

msi_shim32.dll: msi.def msi_shim.o stubs32.o
	$(CC) -m32 -shared -fno-lto -o $@ msi.def msi_shim.o stubs32.o \
	    -nostdlib -e _DllMain@12 -Wl,--enable-stdcall-fixup -lkernel32 -luser32

clean:
	rm -f exports.h stubs32.S msi.def *.o msi_shim32.dll

.PHONY: all clean

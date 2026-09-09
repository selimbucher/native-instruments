# Builds msi_shim32.dll, ni-wine's forwarding msi.dll for the Kontakt installer.
# Needs a 32-bit mingw-w64 cross compiler (Arch: mingw-w64-gcc; Nix: the
# pkgsCross.mingw32 stdenv, see flake.nix).  No CRT is linked: the DLL uses
# kernel32 and user32 (wsprintfA) only.

CC = i686-w64-mingw32-gcc
PYTHON ?= python3
CFLAGS ?= -O2 -Wall -Wextra -fno-builtin -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns

all: msi_shim32.dll

exports.h stubs32.S msi.def: gen_forwarders.py msi_exports.txt
	$(PYTHON) gen_forwarders.py

msi_shim.o: msi_shim.c exports.h
	$(CC) -m32 $(CFLAGS) -c $< -o $@

stubs32.o: stubs32.S
	$(CC) -m32 -c $< -o $@

msi_shim32.dll: msi.def msi_shim.o stubs32.o
	$(CC) -m32 -shared -o $@ msi.def msi_shim.o stubs32.o \
	    -nostdlib -e _DllMain@12 -Wl,--enable-stdcall-fixup -lkernel32 -luser32

clean:
	rm -f exports.h stubs32.S msi.def *.o msi_shim32.dll

.PHONY: all clean

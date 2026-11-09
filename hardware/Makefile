# Builds ni-wine's NI controller support for 64-bit Wine prefixes (README.md).
# Needs a 64-bit mingw-w64 cross compiler (Arch: mingw-w64-gcc; Debian:
# gcc-mingw-w64-x86-64; Fedora: mingw64-gcc; Nix: pkgsCross.mingwW64, see
# flake.nix). No CRT is linked.

CC = x86_64-w64-mingw32-gcc
# Not CFLAGS: nothing links a CRT, so the host's flags (makepkg exports LTO,
# stack protection, fortify) must not reach these.
HW_CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -fno-builtin -ffreestanding \
    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-lto
# mingw-w64 keeps the driver headers in ddk/ next to the Win32 ones
HASH := \#
DDK = $(patsubst %/ntddk.h,%,$(filter %/ddk/ntddk.h,\
    $(shell echo '$(HASH)include <ddk/ntddk.h>' | $(CC) -xc -M -MG - 2>/dev/null)))

FILES = nimk3.sys nirescan.exe nimidi.exe nidrvinst.exe bmidilib2.dll maschine-mk3.inf
DATADIR = ../src/ni_wine/data

all: nimk3.sys nirescan.exe nimidi.exe nidrvinst.exe bmidilib2.dll

nimk3.sys: nimk3.c nimidi.h
	$(CC) $(HW_CFLAGS) -I$(DDK) -shared -nostdlib -nostartfiles \
	    -Wl,--subsystem,native -Wl,--entry,DriverEntry -o $@ $< -lntoskrnl -lhal -lgcc

nirescan.exe: nirescan.c
	$(CC) $(HW_CFLAGS) -nostdlib -nostartfiles -Wl,--subsystem,windows -Wl,--entry,entry \
	    -o $@ $< -lkernel32 -ladvapi32 -luser32 -lcfgmgr32 -lshlwapi

nimidi.exe: nimidi.c nimidi.h
	$(CC) $(HW_CFLAGS) -nostdlib -nostartfiles -Wl,--subsystem,windows -Wl,--entry,entry \
	    -o $@ $< -lkernel32 -ladvapi32 -luser32 -lcfgmgr32 -lshlwapi -lws2_32 -lgcc

bmidilib2.dll: bmidi.c nimidi.h
	$(CC) $(HW_CFLAGS) -shared -nostdlib -nostartfiles -Wl,--entry,DllMain -o $@ $< -lkernel32

nidrvinst.exe: nidrvinst.c
	$(CC) $(HW_CFLAGS) -nostdlib -nostartfiles -Wl,--subsystem,console -Wl,--entry,entry \
	    -o $@ $< -lkernel32 -lshell32 -lnewdev

install: all
	install -m644 $(FILES) $(DATADIR)

clean:
	rm -f nimk3.sys nirescan.exe nimidi.exe nidrvinst.exe bmidilib2.dll

.PHONY: all install clean

# NI controller support

Windows binaries ni-wine installs into the prefix so that NI's hardware
services see controllers. All of them are 64-bit, link no CRT and are built
from these sources by the packages.

## nirescan.exe

An auto-start service. NIHostIntegrationAgent and NIHardwareService
enumerate devices once when they start and learn about later ones only from
`SERVICE_CONTROL_DEVICEEVENT`. Wine starts them before winebus and wineusb
have created any device and never sends services device events, so no
controller is ever found. nirescan waits for PlugPlay, watches HID and MK3
device interfaces with NI's vendor id and restarts both services once the
devices are up (one restart per burst of arrivals). On a Wine that does send
service device events it does nothing.

Maschine doesn't reconnect to a restarted NIHostIntegrationAgent: after a
controller is plugged in while Maschine runs, Maschine (or the plugin) has to
be restarted. Removals don't restart anything.

## nimk3.sys + maschine-mk3.inf

A WDM driver for two Maschine MK3 interfaces, in place of NI's
`nimc3usb.sys`, a KMDF driver Wine can't load. It attaches to the wineusb
interface devices, registers the device interface
`{64352AA7-9DFD-4FA8-B6F0-474AF23A78E9}` on both and handles the IOCTLs the
services send: kind, version, descriptors, control transfers and the event
queue. Pads and buttons are plain HID and need no driver.

- Display interface (`USB\VID_17CC&PID_1600&MI_05`, kind 3): bulk writes to
  the two displays.
- Audio interface (`USB\VID_17CC&PID_1600&MI_00`, kind 0): only the virtual
  "Maschine MK3 Ctrl MIDI" port. In MIDI mode NIHostIntegrationAgent sends
  to it with IOCTL `0x80892040` (`{1, length, bytes}`) and gets what a DAW
  sends as event type 7 with the same payload. The driver also registers
  the interface in `nimidi.h` for nimidi.exe. No USB I/O there but
  descriptor reads, snd-usb-audio keeps the interface.

NI's installers put `nimc3usb.inf` into `C:\windows\inf` without the files
it copies. Wine installs it for any MK3 interface it hasn't bound yet and
retries the missing copy forever, which blocks every later device. So
`maschine-mk3.inf` also binds the other interfaces NI's INF lists, with no
driver (firmware updates can't work under Wine).
Wine binds a device once and keeps the binding. Between equally ranked INFs
it takes the first in name order, and `maschine-mk3.inf` sorts first.

## nimidi.exe

An auto-start service that relays virtual MIDI ports to ni-maschine
(`../maschine`), which has the ALSA ports: those of the drivers (interface
`GUID_DEVINTERFACE_NIWINE_MIDI` in `nimidi.h`) and those of bmidilib2.dll's
clients. While a port exists it starts `ni-maschine --attach N` (the Unix
path ni-wine writes to `HKLM\Software\ni-wine\ni-maschine`), which connects
to 127.0.0.1:N and hands the connection to the running ni-maschine. A
controller's port it announces tells ni-maschine to leave that controller to
NI's agent. Wine has no AF_UNIX sockets and can't give a service's Unix child
pipes as stdio, hence TCP. Only a peer of the same Unix user is accepted
(`/proc/net/tcp`).

## bmidilib2.dll

Replaces Bome's library in `system32`. NI's services make their virtual MIDI
ports with it, among them the Maschine Mikro MK3's, and Bome's bus driver
doesn't load in Wine. It has the functions NI's services import and sends
the ports to nimidi.exe over the pipe in `nimidi.h`, one client per process.

## nidrvinst.exe

Rebinds devices that NI's INF got before ni-wine's was there:

    nidrvinst USB\VID_17CC&PID_1600&MI_05 C:\windows\inf\maschine-mk3.inf

## Building

    make            # nimk3.sys nirescan.exe nimidi.exe nidrvinst.exe bmidilib2.dll
    make install    # copies them and the INF into ../src/ni_wine/data

Check the imports (the driver must import ntoskrnl.exe and hal.dll only,
ntdll or kernel32 make Wine load it as a DLL):

    x86_64-w64-mingw32-objdump -p nimk3.sys | grep 'DLL Name'

## Debugging

    WINEDEBUG=warn+debugstr    # nimk3.sys (DbgPrint)
    WINEDEBUG=warn+seh         # nirescan.exe, nimidi.exe (OutputDebugString)

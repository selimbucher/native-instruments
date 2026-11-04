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

A WDM driver for the Maschine MK3 display interface (`USB\VID_17CC&PID_1600&MI_05`),
in place of NI's `nimc3usb.sys`, a KMDF driver Wine can't load. It attaches
to the wineusb interface device, registers the device interface
`{64352AA7-9DFD-4FA8-B6F0-474AF23A78E9}` and handles the IOCTLs the services
send: kind, version, descriptors, control transfers, bulk writes to the two
displays and the event queue. Pads and buttons are plain HID and need no
driver.

NI's installers put `nimc3usb.inf` into `C:\windows\inf` without the files
it copies. Wine installs it for any MK3 interface it hasn't bound yet and
retries the missing copy forever, which blocks every later device. So
`maschine-mk3.inf` also binds the other interfaces NI's INF lists, with no
driver (audio and MIDI are ALSA's, firmware updates can't work under Wine).
Wine binds a device once and keeps the binding. Between equally ranked INFs
it takes the first in name order, and `maschine-mk3.inf` sorts first.

## nidrvinst.exe

Rebinds devices that NI's INF got before ni-wine's was there:

    nidrvinst USB\VID_17CC&PID_1600&MI_05 C:\windows\inf\maschine-mk3.inf

## Building

    make            # nimk3.sys nirescan.exe nidrvinst.exe
    make install    # copies them and the INF into ../src/ni_wine/data

Check the imports (the driver must import ntoskrnl.exe only, anything else
makes Wine load it as a DLL):

    x86_64-w64-mingw32-objdump -p nimk3.sys | grep 'DLL Name'

## Debugging

    WINEDEBUG=warn+debugstr    # nimk3.sys (DbgPrint)
    WINEDEBUG=warn+seh         # nirescan.exe (OutputDebugString)

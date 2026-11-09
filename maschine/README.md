# ni-maschine

MIDI mode of the Maschine MK3 and Maschine Mikro MK3 without NI's software,
natively: what NIHostIntegrationAgent 1.18.0 does in MIDI mode. A user service
(`ni-maschine.service`, written and started by `ni`) that waits for the
controllers (udev events) and then, per controller:

- owns an ALSA port of the client `Native Instruments`: `Maschine MK3 Ctrl
  MIDI`, `Maschine Mikro MK3`
- maps pads, buttons, knobs, encoder, touch strip, footswitch and pedal to
  MIDI with the Controller Editor template the agent keeps in the prefix,
  `drive_c/ProgramData/Native Instruments/Controller Editor/MaschineMK3.ncs`
  or `MaschineMikroMK3.ncs` (re-read when it changes, NI's default template if
  there is none)
- drives the LEDs and the displays: on the MK3 the KNOBS, MONITOR, PAGES,
  TEMPLATES and Shift screens, on the Mikro a MIDI MODE banner, as the agent
  has them

SHIFT + CHANNEL (MK3) or SHIFT + MASCHINE (Mikro) toggles MIDI mode, kept in
`~/.local/state/ni-wine/maschine-mode` and `maschine-mikro-mode`. GROUP + pad
selects the Mikro's pad page.

## Hand-off to NI's agent

nimidi.exe in a running prefix connects (`ni-maschine --attach PORT`) and
announces the MIDI ports NI's agent makes. A controller whose port it
announces is the agent's: ni-maschine lets go of it and relays the agent's
MIDI on the controller's port, until the port or the connection goes. It
also leaves the MK3 alone while another program holds the display
interface, until that program closes the USB device (inotify, the kernel
sends no event when a usbfs claim ends).

## Hardware

`model.c` has both controllers' report layouts, button and LED tables.

- hidraw, input report 0x01: buttons, encoder (low nibble), the MK3's 8
  knobs (u16, 0-999), touch strip (two fingers, u16 each), the MK3's pedal.
  0x02: pad events `{pad, kind << 4 | value >> 8, value}`. LEDs: byte =
  colour << 2 | level, the MK3 in output reports 0x80 and 0x81, the Mikro in
  0x80.
- MK3 display interface 5, bulk OUT 0x04: `84 00 <screen> 60 00 00 00 00 x y
  w h` (big endian u16), then `00 n` (n pixel pairs follow), `01 n` (the next
  pair n times), `03` blit, `40` end. RGB565, big endian.
- Mikro display, HID output report 0xE0: x, y, w, h (little endian u16, y
  and h in 8-row pages), then up to 256 bytes, a column per byte, bit k = row
  k, 1 = unlit.

## Building

    make          # needs alsa-lib and pkg-config
    make check    # engine tests

`font.h` comes from Spleen's BDF files (BSD-2-Clause):

    python3 fonts.py <spleen>/share/fonts/misc > font.h

## Debugging

    journalctl --user -u ni-maschine

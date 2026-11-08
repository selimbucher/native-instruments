# ni-maschine

MIDI mode of the Maschine MK3 without NI's software, natively: what
NIHostIntegrationAgent 1.18.0 does in MIDI mode. A user service
(`ni-maschine.service`, written and started by `ni`) that waits for an MK3
(udev events) and then:

- owns the ALSA port `Native Instruments:Maschine MK3 Ctrl MIDI`
- maps pads, buttons, knobs, encoder, touch strip, footswitch and pedal to
  MIDI with the Controller Editor template the agent keeps in the prefix,
  `drive_c/ProgramData/Native Instruments/Controller Editor/MaschineMK3.ncs`
  (re-read when it changes, NI's default template if there is none)
- drives the LEDs and both screens: KNOBS, MONITOR, PAGES, TEMPLATES and the
  Shift screen, as the agent has them

SHIFT + CHANNEL toggles MIDI mode, kept in
`~/.local/state/ni-wine/maschine-mode`.

## Hand-off to NI's agent

When nimidi.exe in a running prefix connects (`ni-maschine --attach PORT`)
and announces the MK3 port, the agent gets the MK3 and ni-maschine only
relays its MIDI on the same port. When the connection ends it takes the MK3
back. It also leaves the MK3 alone while another program holds the display
interface, until that program closes the USB device (inotify, the kernel
sends no event when a usbfs claim ends).

## Hardware

- hidraw, input report 0x01: 80 buttons, encoder (low nibble), 8 knobs
  (u16, 0-999), touch strip, pedal. 0x02: pad events
  `{pad, kind << 4 | value >> 8, value}`. Output 0x80 and 0x81: LEDs, byte =
  colour << 2 | level. Bit and LED tables in `layout.c`.
- Display interface 5, bulk OUT 0x04: `84 00 <screen> 60 00 00 00 00 x y w h`
  (big endian u16), then `00 n` (n pixel pairs follow), `01 n` (the next pair
  n times), `03` blit, `40` end. RGB565, big endian.

## Building

    make          # needs alsa-lib and pkg-config
    make check    # engine tests

`font.h` comes from Spleen's BDF files (BSD-2-Clause):

    python3 fonts.py <spleen>/share/fonts/misc > font.h

## Debugging

    journalctl --user -u ni-maschine

/*
 * Interface between a driver with a virtual MIDI port (nimk3.sys) and
 * nimidi.exe, which relays the port to ni-maschine (ALSA).
 *
 * The driver registers GUID_DEVINTERFACE_NIWINE_MIDI for each port. "Out" is
 * what NI's software sends (the port's input in a DAW), "in" what a DAW sends
 * to the port. Both are raw MIDI bytes.
 */

#ifndef NIMIDI_H
#define NIMIDI_H

/* {e7be2b5a-0074-492e-bb70-a6429d7e67ec} */
DEFINE_GUID(GUID_DEVINTERFACE_NIWINE_MIDI, 0xe7be2b5a, 0x0074, 0x492e, 0xbb, 0x70, 0xa6, 0x42, 0x9d, 0x7e, 0x67, 0xec);

/* out: port name, UTF-8 */
#define IOCTL_NIWINE_MIDI_NAME CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* out: MIDI NI sent, pends until there is some. The driver keeps what NI
 * sends from the first read on until the handle is closed. */
#define IOCTL_NIWINE_MIDI_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* in: MIDI for NI */
#define IOCTL_NIWINE_MIDI_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* frames between nimidi.exe and ni-maschine, both directions:
 * u8 type, u8 port, u16 length (little endian), payload */
#define NIMIDI_PORT_ADD    1 /* payload: port name, UTF-8 */
#define NIMIDI_PORT_REMOVE 2
#define NIMIDI_DATA        3 /* payload: MIDI bytes */
#define NIMIDI_HEADER      4

#endif

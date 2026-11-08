/* Maschine MK3 hardware: the HID interface through hidraw (pads, buttons,
 * knobs, LEDs) and the display interface through usbfs. */

#ifndef MK3_H
#define MK3_H

#include <stdbool.h>
#include "engine.h"
#include "screen.h"

struct mk3
{
    char hidraw[64];            /* /dev/hidrawN */
    char usb[64];               /* /dev/bus/usb/BBB/DDD */
    int hid, usbfs;             /* -1 when closed */

    bool seen;                  /* a report came since opening */
    unsigned char buttons[MK3_BUTTONS / 8];
    int encoder;
    int knobs[MK3_KNOBS];
    int pedal;
    int strip;
    unsigned char leds[MK3_LEDS];
    bool leds_sent;
    struct canvas sent[2];
    bool screens_sent;
};

/* finds a connected MK3, 0 if there is one */
int mk3_find(struct mk3 *dev);
/* opens and claims the display interface; -EBUSY if another program has it,
 * the USB node then stays open until mk3_close */
int mk3_open(struct mk3 *dev);
void mk3_close(struct mk3 *dev);
/* reads the pending reports into the engine; -1 if the device is gone */
int mk3_read(struct mk3 *dev, struct engine *e);
/* one input report into the engine */
void mk3_report(struct mk3 *dev, struct engine *e, const unsigned char *report, int len);
/* the display frame for a region (w even) of a screen into out, returns its size */
int mk3_encode(unsigned char *out, int screen, const struct canvas *c, int x, int y, int w, int h);
/* send what changed since the last call; -1 if the device is gone */
int mk3_leds(struct mk3 *dev, const unsigned char leds[MK3_LEDS]);
int mk3_screens(struct mk3 *dev, const struct canvas screens[2]);

#endif

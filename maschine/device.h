/* A connected controller: the HID interface through hidraw (buttons, pads, knobs, LEDs,
 * the Mikro MK3's display) and the MK3's display interface through usbfs. */

#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>
#include "engine.h"
#include "screen.h"

struct device
{
    const struct model *model;
    char hidraw[64];            /* /dev/hidrawN */
    char usb[64];               /* /dev/bus/usb/BBB/DDD, MK3 */
    int hid, usbfs;             /* -1 when closed */

    bool seen;                  /* a report came since opening */
    unsigned char buttons[MAX_BUTTONS / 8 + 1];
    int encoder;
    int knobs[KNOBS];
    int pedal;
    int strip;
    unsigned char leds[MAX_LEDS];
    bool leds_sent;
    struct canvas sent[2];      /* MK3 */
    struct mono_canvas mono_sent;
    bool screens_sent;
};

/* finds a connected controller of dev->model, 0 if there is one */
int device_find(struct device *dev);
/* opens it; for the MK3 also claims the display interface: -EBUSY if another
 * program has it, the USB node then stays open until device_close */
int device_open(struct device *dev);
void device_close(struct device *dev);
/* reads the pending reports into the engine; -1 if the device is gone */
int device_read(struct device *dev, struct engine *e);
/* one input report into the engine */
void device_report(struct device *dev, struct engine *e, const unsigned char *report, int len);
/* send what changed since the last call; -1 if the device is gone */
int device_leds(struct device *dev, const unsigned char leds[MAX_LEDS]);
int device_screens(struct device *dev, const struct canvas screens[2]);
int device_mono(struct device *dev, const struct mono_canvas *c);
/* the MK3 display frame for a region (w even) of a screen into out, returns its size */
int device_encode(unsigned char *out, int screen, const struct canvas *c, int x, int y, int w, int h);
/* the Mikro display report for a region (y, h in 8-row pages, w * h <= 256) into out */
int device_encode_mono(unsigned char *out, const struct mono_canvas *c, int x, int page, int w, int pages);

#endif

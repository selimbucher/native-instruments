/* The controllers ni-maschine drives: Maschine MK3 and Maschine Mikro MK3. Their
 * HID reports, buttons, LEDs and what MIDI mode does differently on each. */

#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>

#define MODELS 2
#define MAX_BUTTONS 0x51        /* the MK3's 80 report bits and TouchstripCap */
#define MAX_LEDS 103
#define PADS 16
#define KNOBS 8                 /* MK3 only */
#define STRIP_LEDS 25
#define KNOB_COUNTS 1000        /* per turn, the knobs report 0-999 */
#define PAD_MAX 4095
#define PEDAL_MAX 4095

/* an LED byte is colour << 2 | brightness, colour 0 is off */
#define LED_GREY 30
#define LED_WHITE 31
#define LED_DIM 0
#define LED_BRIGHT 2

static inline unsigned char led_byte(int color, int brightness)
{
    return color ? color << 2 | brightness : 0;
}

struct button_info
{
    const char *id;             /* template id, NULL if the bit has no control */
    unsigned char cc;           /* default CC */
    signed char led;            /* LED index, -1 for none */
};

enum display { DISPLAY_SCREENS, DISPLAY_MONO };

struct model
{
    const char *name;
    const char *hid_id;             /* in the hidraw device's uevent */
    const char *port_name;          /* the ALSA port */
    const char *template_file;      /* in Controller Editor's directory */
    const char *mode_file;          /* in ni-wine's state directory, MIDI mode on or off */

    /* input report 0x01, offsets including the report id */
    int report_size;
    int encoder_at;                 /* low nibble, wraps around */
    int knobs_at;                   /* u16 each, -1 without knobs */
    int strip_at;                   /* u16 first finger, then the second one */
    int pedal_at;                   /* u16, -1 without pedal */
    int strip_max;                  /* touch strip positions 1-strip_max, 0 = not touched */

    /* the report's buttons from byte 1, then TouchstripCap */
    const struct button_info *buttons;
    int button_bits;
    int touchstrip_cap;
    const char *wheel;              /* template id of the encoder */
    int shift;
    int mode;                       /* with SHIFT: MIDI mode on and off */
    bool shift_swallows;            /* SHIFT always silences buttons, the MK3's only with navigation */

    int leds;
    int led_report_2;               /* first LED of report 0x81, 0 if report 0x80 has all */
    int led_pads;                   /* 16 pads in device order */
    int led_strip;                  /* 25 LEDs, left to right */
    int led_shift;

    /* the MK3's navigation; -1 if the model has none */
    int left, right;                /* knob pages, templates with SHIFT */
    int led_left, led_right;
    int group_a;                    /* Group A-H: pad pages */
    signed char screen_buttons[8];  /* Button1-8 */
    /* the Mikro's: GROUP + pad selects the pad page */
    int group;

    enum display display;
    unsigned char sysex_id[2];      /* the product id in NI's SysEx to the host */
    bool shift_sysex;               /* SHIFT is reported to the host when there is no navigation */
};

extern const struct model maschine_mk3;
extern const struct model maschine_mikro_mk3;
extern const struct model *const models[MODELS];

/* device pad (top left first) -> template PadN (1-16, bottom left first) */
static inline int pad_number(int device)
{
    return (3 - device / 4) * 4 + device % 4 + 1;
}

#endif

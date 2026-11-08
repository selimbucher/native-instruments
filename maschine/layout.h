/* Maschine MK3 HID layout: the buttons of input report 0x01 and the LED
 * numbering of the output reports 0x80 (LEDs 0-61) and 0x81 (62-102). */

#ifndef LAYOUT_H
#define LAYOUT_H

#define MK3_BUTTONS 80
#define MK3_KNOBS 8
#define MK3_PADS 16
#define MK3_KNOB_COUNTS 1000    /* per turn, the knobs report 0-999 */
#define MK3_STRIP_MAX 1024      /* touch strip positions 1-1024, 0 = not touched */
#define MK3_PAD_MAX 4095
#define MK3_PEDAL_MAX 4095

#define MK3_LEDS 103
#define MK3_LED_STRIP 62        /* 25 touch strip LEDs, left to right */
#define MK3_LED_PADS 87         /* 16 pads in device order */
#define MK3_STRIP_LEDS 25

/* buttons with a meaning of their own */
#define BTN_ENCODER_UP 0x02     /* to 0x05: up, right, down, left */
#define BTN_ENCODER_LEFT 0x05
#define BTN_SHIFT 0x06
#define BTN_BUTTON8 0x07
#define BTN_GROUP_A 0x08        /* to 0x0f: Group A-H */
#define BTN_RIGHT 0x32
#define BTN_CHANNEL 0x38
#define BTN_LEFT 0x3b
#define BTN_BUTTON1 0x40        /* to 0x46: Button1-7 */
#define BTN_ENCODER_CAP 0x47
/* not a button of the report: pressed while the touch strip is touched */
#define BTN_TOUCHSTRIP_CAP 0x50
#define BTN_COUNT 0x51

#define LED_LEFT 6
#define LED_RIGHT 7
#define LED_BUTTON1 12
#define LED_SHIFT 44

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

extern const struct button_info mk3_buttons[BTN_COUNT];

/* ButtonN -> bit */
static inline int button_bit(int n)
{
    return n == 8 ? BTN_BUTTON8 : BTN_BUTTON1 + n - 1;
}

/* device pad (top left first) -> template PadN (1-16, bottom left first) */
static inline int pad_number(int device)
{
    return (3 - device / 4) * 4 + device % 4 + 1;
}

#endif

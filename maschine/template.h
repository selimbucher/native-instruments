/* Controller Editor templates for the Maschine MK3 (MaschineMK3.ncs, XML
 * <ni-controller-midi-map>) and the state MIDI mode keeps per control. Element
 * names, defaults and meanings follow NIHostIntegrationAgent's parser. */

#ifndef TEMPLATE_H
#define TEMPLATE_H

#include <stdbool.h>

enum control_type
{
    CONTROL_KNOB,
    CONTROL_PEDAL,
    CONTROL_BUTTON,
    CONTROL_FOOTSWITCH,
    CONTROL_WHEEL,
    CONTROL_PAD,
    CONTROL_PRESSURE,           /* <pad subtype="pressure"> */
    CONTROL_LED,
};

/* what a control sends: MIDI status bytes, Mackie Control ones below 0x80 */
enum
{
    MSG_MCU_BUTTON = 0x10,
    MSG_MCU_VPOT,
    MSG_MCU_CHANNEL,            /* channel fader */
    MSG_MCU_MASTER,             /* master fader */
    MSG_MCU_WHEEL,
    MSG_NOTE_OFF = 0x80,        /* only received */
    MSG_NOTE = 0x90,
    MSG_POLY_AT = 0xa0,
    MSG_CC = 0xb0,
    MSG_PROGRAM = 0xc0,
    MSG_CHANNEL_AT = 0xd0,
    MSG_PITCH_BEND = 0xe0,
    MSG_SONG_POSITION = 0xf2,
    MSG_START = 0xfa,
    MSG_CONTINUE = 0xfb,
    MSG_STOP = 0xfc,
};

/* <controller mode>: absolute, or relative steps as two's complement or around 64 */
enum { MODE_ABSOLUTE, MODE_COMP, MODE_OFFSET };

enum { BEHAVIOR_TOGGLE, BEHAVIOR_TRIGGER, BEHAVIOR_GATE, BEHAVIOR_INCREMENT };

/* <unit color-mode> of led elements */
enum { LED_MODE_DIM, LED_MODE_ON_OFF, LED_MODE_HSB, LED_MODE_RAW };

/* <ledPattern> of the touch strip */
enum { STRIP_BAR, STRIP_DOT, STRIP_CENTER, STRIP_BAR_DOT };

struct control
{
    char id[32];                /* "Knob1" */
    char name[48];              /* <name>, "" if none */
    enum control_type type;
    int message;                /* MSG_* */
    int mode;                   /* MODE_*, CCs only */
    int channel;
    int number;                 /* note, CC or MCU number */
    int min, max;
    int def;                    /* <default> */
    int inc;                    /* step of increment buttons, relative knobs and the wheel */
    int range;                  /* degrees of an absolute knob from min to max */
    int steps;                  /* steps per turn of a relative knob */
    int ticks;                  /* encoder ticks per step */
    int behavior;               /* BEHAVIOR_* of buttons and pads */
    int ledpattern;             /* STRIP_* */
    bool enabled;
    bool wrap;
    bool ondown;                /* buttons act when pressed, else when released */
    bool onifdown;              /* the LED shows the button, not the host */
    bool bipolar;
    float on_pressure;          /* note on and off thresholds of pressure pads */
    float off_pressure;
    int color_mode;             /* LED_MODE_*, led elements */
    int color_on, color_off;    /* palette indices, led elements */
    struct control *led;        /* "<id>IDX" or an <led> child */
    int led_index;              /* the <led> child while loading, -1 for none */

    /* state */
    float pos;                  /* knob position 0..1, or 1 for a button that is on */
    int last;                   /* last value sent or set from outside */
    int last_ext;               /* last value from the host */
    bool pressed;
    float turns;                /* relative knob turns not sent yet */
    int ticks_left;             /* encoder ticks not sent yet */
};

struct controls
{
    struct control *items;
    int count;
};

struct page
{
    char name[48];
    int column[8];              /* knob colours, palette indices */
    int column_b[8];            /* button colours */
    bool explicit_mcu;
    struct controls controls;
};

struct group
{
    char name[48];
    int color;
    struct controls controls;
};

struct map
{
    char name[48];
    int velocity_curve;
    int wrapmode;               /* for the pages, -1 = the file's */
    bool knob_pages;            /* Left/Right switch knob pages */
    bool pad_pages;             /* Group A-H switch pad pages */
    bool handle_transport;
    struct controls controls;
    struct page *pages;
    int page_count, page;
    struct group *groups;
    int group_count, group;
};

struct templates
{
    struct map *maps;
    int count, current;
    int wrapmode;               /* 0 wraps around at the ends */
    int display_mode;           /* the screen NI's agent saved */
    int knob_mode;              /* 0 shows knob values, 1 bars */
};

int templates_load(struct templates *t, const char *path);
/* NI's "Template 1", what the agent writes when there is no template yet */
void templates_default(struct templates *t);
void templates_free(struct templates *t);
struct control *controls_find(const struct controls *list, const char *id);
/* whether value v turns toggle c on */
bool toggle_value_on(const struct control *c, int v);

/* palette index -> MK3 LED colour, 0 = off */
int palette_led(int index);
/* palette index -> 0xRRGGBB for the screens */
unsigned int palette_rgb(int index);
#define PALETTE_SIZE 19
#define PALETTE_BLACK 16
#define PALETTE_GREY 17
#define PALETTE_WHITE 18

#endif

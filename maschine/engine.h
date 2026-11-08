/* MIDI mode of the Maschine MK3 as NIHostIntegrationAgent does it: controls
 * to MIDI by the template, MIDI from the host to control state, LEDs,
 * template/page/pad page navigation and the screens' state. No I/O. */

#ifndef ENGINE_H
#define ENGINE_H

#include "layout.h"
#include "template.h"

/* display_mode values of the template */
enum { SCREEN_SHIFT = 0, SCREEN_KNOBS = 4, SCREEN_TEMPLATES = 6, SCREEN_PAGES = 7, SCREEN_MONITOR = 9 };

/* kinds of pad report entries */
enum { PAD_SWITCH_ON, PAD_HIT_ON, PAD_SWITCH_OFF, PAD_HIT_OFF, PAD_PRESSURE };

struct engine
{
    struct templates t;
    void (*midi)(void *ctx, const unsigned char *data, int len);
    void *ctx;

    bool midi_mode;
    bool shift;
    int screen;                     /* SCREEN_*, SCREEN_SHIFT only while Shift is held */
    /* list cursors of the TEMPLATES and PAGES screens */
    int selected_template;
    int selected_page;
    int selected_group;
    int scroll_counts[MK3_KNOBS];   /* knob counts towards the next list step */
    bool buttons[BTN_COUNT];
    int strip;                      /* touch strip position, 0 = not touched */
    /* what the presses went to: releases go there even after navigation */
    struct control *held[BTN_COUNT];
    struct control *held_pad[MK3_PADS];
    struct control *held_pressure[MK3_PADS];    /* notes the pressure turned on */

    /* last control used, for the MONITOR screen */
    char monitor_name[48];
    char monitor_event[64];
    char monitor_hit[64];
    char monitor_press[64];
    bool monitor_pad;

    unsigned int changes;           /* counts state changes, for redraws */
};

void engine_init(struct engine *e, void (*midi)(void *, const unsigned char *, int), void *ctx);
/* takes over t; keeps the current template, pages and screen where possible */
void engine_set_templates(struct engine *e, struct templates *t);
void engine_free(struct engine *e);
void engine_set_midi_mode(struct engine *e, bool on);
/* releases everything held: note offs, gates off */
void engine_release_all(struct engine *e);
/* forgets what the hardware had pressed, after someone else had it */
void engine_reset_input(struct engine *e);

void engine_button(struct engine *e, int bit, bool down);
void engine_encoder(struct engine *e, int ticks);
void engine_knob(struct engine *e, int knob, int counts);          /* knob 1-8, MK3_KNOB_COUNTS a turn */
void engine_strip(struct engine *e, int pos);                       /* 0 = not touched, 1-MK3_STRIP_MAX */
void engine_pedal(struct engine *e, int value);                     /* 0-MK3_PEDAL_MAX */
void engine_pad(struct engine *e, int pad, int kind, int value);    /* device pad, 0-MK3_PAD_MAX */
void engine_midi_in(struct engine *e, const unsigned char *data, int len);

void engine_leds(const struct engine *e, unsigned char leds[MK3_LEDS]);

/* for the screens */
struct map *engine_map(const struct engine *e);
struct control *engine_find(const struct engine *e, const char *id);
/* "<prefix><n>" */
struct control *engine_find_n(const struct engine *e, const char *prefix, int n);
/* PadN, or PressureN if there is no PadN */
struct control *engine_pad_control(const struct engine *e, int n);
int engine_shown_screen(const struct engine *e);
/* <name>, else the id with a space before its number: "Knob1" -> "Knob 1" */
void control_label(const struct control *c, char *buf, int size);
bool control_is_on(const struct control *c);
/* "C3" for note 60 */
void note_name(int note, char *buf, int size);

#endif

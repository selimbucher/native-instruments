#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "util.h"

#define MIDI_MAX 127
#define MIDI14_MAX 0x3fff           /* pitch bend, song position, MCU faders */
#define MIDI_SYSEX 0xf0
#define MCU_WHEEL_CC 0x3c
#define MCU_MASTER_CHANNEL 8

/* knob counts per list step on the TEMPLATES and PAGES screens */
#define SCROLL_COUNTS 120

static void changed(struct engine *e)
{
    e->changes++;
}

struct map *engine_map(const struct engine *e)
{
    return e->t.count ? &e->t.maps[e->t.current] : NULL;
}

/* the base controls, then the current knob page, then the current pad page */
struct control *engine_find(const struct engine *e, const char *id)
{
    struct map *map = engine_map(e);
    struct control *c;

    if (!map || !id) return NULL;
    if ((c = controls_find(&map->controls, id))) return c;
    if (map->page_count && (c = controls_find(&map->pages[map->page].controls, id))) return c;
    if (map->group_count && (c = controls_find(&map->groups[map->group].controls, id))) return c;
    return NULL;
}

struct control *engine_find_n(const struct engine *e, const char *prefix, int n)
{
    char id[32];

    snprintf(id, sizeof(id), "%s%d", prefix, n);
    return engine_find(e, id);
}

struct control *engine_pad_control(const struct engine *e, int n)
{
    struct control *c = engine_find_n(e, "Pad", n);

    return c ? c : engine_find_n(e, "Pressure", n);
}

int engine_shown_screen(const struct engine *e)
{
    return e->shift ? SCREEN_SHIFT : e->screen;
}

void control_label(const struct control *c, char *buf, int size)
{
    const char *digits;

    if (c->name[0])
    {
        snprintf(buf, size, "%s", c->name);
        return;
    }
    for (digits = c->id + strlen(c->id); digits > c->id && isdigit((unsigned char)digits[-1]); digits--);
    if (*digits && digits > c->id) snprintf(buf, size, "%.*s %s", (int)(digits - c->id), c->id, digits);
    else snprintf(buf, size, "%s", c->id);
}

void note_name(int note, char *buf, int size)
{
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    snprintf(buf, size, "%s%d", names[note % 12], note / 12 - 2);
}

/* v as a position 0..1 between min and max */
static float norm(const struct control *c, int v)
{
    if (c->max == c->min) return 0;
    return clampf((float)(v - c->min) / (c->max - c->min), 0, 1);
}

static int scale(const struct control *c, float x)
{
    return c->min + (int)lroundf((c->max - c->min) * x);
}

static bool is_14bit(int message)
{
    return message == MSG_PITCH_BEND || message == MSG_SONG_POSITION || message == MSG_MCU_CHANNEL
        || message == MSG_MCU_MASTER;
}

static bool is_transport(const struct control *c)
{
    return c->message == MSG_START || c->message == MSG_CONTINUE || c->message == MSG_STOP;
}

static int value_14bit(const unsigned char *data, int len)
{
    return len > 2 ? data[1] | data[2] << 7 : 0;
}

static void set_14bit(unsigned char *p, int v)
{
    p[0] = v & 0x7f;
    p[1] = v >> 7;
}

/* the MONITOR screen's "CC - Ch 1 - 57 - 127" */
static void describe(const unsigned char *data, int len, char *buf, int size)
{
    static const char *kinds[] = {"Note Off", "Note On", "Poly AT", "CC", "Program", "Channel AT"};
    int kind = data[0] >> 4, channel = (data[0] & 15) + 1;
    char note[8];

    switch (data[0])
    {
    case MSG_START:
        snprintf(buf, size, "Start");
        return;
    case MSG_CONTINUE:
        snprintf(buf, size, "Continue");
        return;
    case MSG_STOP:
        snprintf(buf, size, "Stop");
        return;
    case MSG_SONG_POSITION:
        snprintf(buf, size, "Song Position");
        return;
    }
    switch (kind)
    {
    case 0x8:
    case 0x9:
    case 0xa:
        note_name(data[1], note, sizeof(note));
        snprintf(buf, size, "%s - Ch %d - %s - %d", kinds[kind - 8], channel, note, data[2]);
        break;
    case 0xb:
        snprintf(buf, size, "CC - Ch %d - %d - %d", channel, data[1], data[2]);
        break;
    case 0xc:
    case 0xd:
        snprintf(buf, size, "%s - Ch %d - %d", kinds[kind - 8], channel, data[1]);
        break;
    default:
        snprintf(buf, size, "Pitch Bend - Ch %d - %d", channel, value_14bit(data, len));
        break;
    }
}

/* the control lists of a map in the order the agent checks them: its own,
 * every knob page, every pad page; NULL past the end */
static struct controls *control_list(struct map *map, int i)
{
    if (i == 0) return &map->controls;
    i--;
    if (i < map->page_count) return &map->pages[i].controls;
    i -= map->page_count;
    if (i < map->group_count) return &map->groups[i].controls;
    return NULL;
}

static bool match(const struct control *c, const unsigned char *data, int len, int *value);
static void apply(struct control *c, int v, bool echo);

/* what the MK3 sends also updates the other controls with that message */
static void echo(struct engine *e, const struct control *src, const unsigned char *data, int len)
{
    struct controls *list;
    int i, j, v;

    for (i = 0; (list = control_list(engine_map(e), i)); i++)
        for (j = 0; j < list->count; j++)
        {
            struct control *c = &list->items[j];

            if (c != src && match(c, data, len, &v)) apply(c, v, true);
        }
}

static void send(struct engine *e, const struct control *c, const unsigned char *data, int len)
{
    char *monitor;

    e->midi(e->ctx, data, len);
    if (c)
    {
        control_label(c, e->monitor_name, sizeof(e->monitor_name));
        e->monitor_pad = c->type == CONTROL_PAD || c->type == CONTROL_PRESSURE;
        if (c->type == CONTROL_PRESSURE) monitor = e->monitor_press;
        else if (c->type == CONTROL_PAD) monitor = e->monitor_hit;
        else monitor = e->monitor_event;
        describe(data, len, monitor, sizeof(e->monitor_event));
        echo(e, c, data, len);
    }
    changed(e);
}

/* sends value v of c; an unchanged value only when forced */
static void emit(struct engine *e, struct control *c, int v, bool force)
{
    unsigned char data[3];
    int len = 3;

    if (!c->enabled) return;
    v = clamp(v, 0, is_14bit(c->message) ? MIDI14_MAX : MIDI_MAX);
    if (!force && v == c->last) return;
    c->last = v;
    switch (c->message)
    {
    case MSG_NOTE:
    case MSG_POLY_AT:
    case MSG_CC:
        data[0] = c->message | c->channel;
        data[1] = c->number;
        data[2] = v;
        break;
    case MSG_PROGRAM:
    case MSG_CHANNEL_AT:
        data[0] = c->message | c->channel;
        data[1] = v;
        len = 2;
        break;
    case MSG_PITCH_BEND:
        data[0] = MSG_PITCH_BEND | c->channel;
        set_14bit(data + 1, v);
        break;
    case MSG_SONG_POSITION:
        data[0] = MSG_SONG_POSITION;
        set_14bit(data + 1, v);
        break;
    case MSG_START:
    case MSG_CONTINUE:
    case MSG_STOP:
        data[0] = c->message;
        len = 1;
        break;
    case MSG_MCU_BUTTON:
        data[0] = MSG_NOTE;
        data[1] = c->number;
        data[2] = v ? MIDI_MAX : 0;
        break;
    case MSG_MCU_VPOT:
        data[0] = MSG_CC;
        data[1] = c->number;
        data[2] = v;
        break;
    case MSG_MCU_CHANNEL:
    case MSG_MCU_MASTER:
        data[0] = MSG_PITCH_BEND | (c->message == MSG_MCU_MASTER ? MCU_MASTER_CHANNEL : c->number & 15);
        set_14bit(data + 1, v);
        break;
    case MSG_MCU_WHEEL:
        data[0] = MSG_CC;
        data[1] = MCU_WHEEL_CC;
        data[2] = v;
        break;
    default:
        return;
    }
    send(e, c, data, len);
}

/* the value a message carries for c, false if it isn't for c */
static bool match(const struct control *c, const unsigned char *data, int len, int *value)
{
    int status = data[0] >= 0xf0 ? data[0] : data[0] & 0xf0;
    int channel = data[0] & 15;

    switch (c->message)
    {
    case MSG_SONG_POSITION:
    case MSG_START:
    case MSG_CONTINUE:
    case MSG_STOP:
        if (status != c->message) return false;
        if (c->message == MSG_SONG_POSITION) *value = value_14bit(data, len);
        else *value = c->message == MSG_STOP ? 0 : MIDI_MAX;
        return true;
    case MSG_MCU_BUTTON:
        if ((status != MSG_NOTE && status != MSG_NOTE_OFF) || len < 3 || data[1] != c->number) return false;
        *value = status == MSG_NOTE && data[2] ? MIDI_MAX : 0;
        return true;
    case MSG_MCU_VPOT:
    case MSG_MCU_WHEEL:
        if (status != MSG_CC || len < 3) return false;
        if (data[1] != (c->message == MSG_MCU_WHEEL ? MCU_WHEEL_CC : c->number)) return false;
        *value = data[2];
        return true;
    case MSG_MCU_CHANNEL:
    case MSG_MCU_MASTER:
        if (status != MSG_PITCH_BEND || len < 3) return false;
        if (channel != (c->message == MSG_MCU_MASTER ? MCU_MASTER_CHANNEL : c->number)) return false;
        *value = data[1] | data[2] << 7;
        return true;
    }
    if (channel != c->channel) return false;
    if (status != c->message && !(c->message == MSG_NOTE && status == MSG_NOTE_OFF)) return false;
    if (status < MSG_PROGRAM && (len < 3 || data[1] != c->number)) return false;
    if (status == MSG_NOTE_OFF) *value = 0;
    else if (status < MSG_PROGRAM) *value = data[2];
    else if (status < MSG_PITCH_BEND) *value = len > 1 ? data[1] : 0;
    else *value = value_14bit(data, len);
    return true;
}

/* a value from outside: from the host, or echoed from another control */
static void apply(struct control *c, int v, bool echo)
{
    switch (c->type)
    {
    case CONTROL_KNOB:
        if (c->mode != MODE_ABSOLUTE)
        {
            if (!echo) c->last_ext = v;
            break;
        }
        /* fall through */
    case CONTROL_PEDAL:
    case CONTROL_WHEEL:
    case CONTROL_LED:
        c->last = v;
        c->pos = norm(c, v);
        if (!echo) c->last_ext = v;
        break;
    case CONTROL_PRESSURE:
        c->last = v;
        break;
    case CONTROL_BUTTON:
    case CONTROL_FOOTSWITCH:
    case CONTROL_PAD:
        if (c->behavior == BEHAVIOR_TOGGLE)
        {
            c->last = v;
            c->pos = toggle_value_on(c, v);
        }
        else if (c->behavior == BEHAVIOR_INCREMENT) c->last = v;
        else if (!echo) c->last_ext = v;
        break;
    }
}

void engine_midi_in(struct engine *e, const unsigned char *data, int len)
{
    struct map *map = engine_map(e);
    struct controls *list;
    bool matched = false;
    int i, j, v;

    if (!map || len < 1 || data[0] < 0x80 || data[0] == MIDI_SYSEX) return;
    for (i = 0; (list = control_list(map, i)); i++)
        for (j = 0; j < list->count; j++)
            if (match(&list->items[j], data, len, &v))
            {
                apply(&list->items[j], v, false);
                matched = true;
            }
    if (matched) changed(e);
}

/* buttons, footswitches and pads that don't send notes */

/* notes always send a note off, other messages min unless it was sent already */
static void emit_off(struct engine *e, struct control *c)
{
    if (c->message == MSG_NOTE) emit(e, c, 0, true);
    else emit(e, c, c->min, false);
}

static void trigger(struct engine *e, struct control *c)
{
    emit(e, c, c->max, true);
    c->pos = 0;
    c->last = c->min;
}

static void toggle(struct engine *e, struct control *c)
{
    if (c->pos >= 0.5f)
    {
        emit_off(e, c);
        c->pos = 0;
    }
    else
    {
        emit(e, c, c->max, false);
        c->pos = 1;
    }
}

/* v into [min, max] (either order), wrapped or clamped */
static int fit(const struct control *c, long v, bool wrap)
{
    long lo = c->min < c->max ? c->min : c->max;
    long hi = c->min < c->max ? c->max : c->min;
    long span = hi - lo + 1;

    if (wrap) return lo + ((v - lo) % span + span) % span;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void increment(struct engine *e, struct control *c)
{
    int v = fit(c, (long)c->last + c->inc, c->wrap);

    emit(e, c, v, false);
    c->pos = norm(c, v);
}

/* what a press or release does: behaviors act on press or on release by <reaction> */
static void act(struct engine *e, struct control *c)
{
    switch (c->behavior)
    {
    case BEHAVIOR_TRIGGER:
        trigger(e, c);
        break;
    case BEHAVIOR_TOGGLE:
        toggle(e, c);
        break;
    case BEHAVIOR_INCREMENT:
        increment(e, c);
        break;
    }
}

static void press(struct engine *e, struct control *c)
{
    c->pressed = true;
    if (!c->enabled) return;
    if (c->message == MSG_MCU_BUTTON) emit(e, c, c->ondown ? MIDI_MAX : 0, true);
    else if (is_transport(c)) trigger(e, c);
    else if (c->behavior == BEHAVIOR_GATE)
    {
        c->pos = 1;
        emit(e, c, c->max, false);
    }
    else if (c->ondown) act(e, c);
}

static void release(struct engine *e, struct control *c)
{
    c->pressed = false;
    if (!c->enabled) return;
    if (c->message == MSG_MCU_BUTTON) emit(e, c, c->ondown ? 0 : MIDI_MAX, true);
    else if (is_transport(c)) return;
    else if (c->behavior == BEHAVIOR_GATE)
    {
        c->pos = 0;
        emit_off(e, c);
    }
    else if (!c->ondown) act(e, c);
}

/* pads that send notes: velocity on hit, a note off on release unless toggled on */
static void pad_release(struct engine *e, struct control *c)
{
    if (c->message != MSG_NOTE)
    {
        release(e, c);
        return;
    }
    c->pressed = false;
    if (c->enabled && c->behavior != BEHAVIOR_TOGGLE && c->pos >= 0.5f)
    {
        emit(e, c, 0, true);
        c->pos = 0;
    }
}

void engine_release_all(struct engine *e)
{
    struct control *c;
    int i;

    for (i = 0; i < BTN_COUNT; i++)
        if ((c = e->held[i]))
        {
            e->held[i] = NULL;
            release(e, c);
        }
    for (i = 0; i < MK3_PADS; i++)
    {
        if ((c = e->held_pad[i]))
        {
            e->held_pad[i] = NULL;
            pad_release(e, c);
        }
        if ((c = e->held_pressure[i]))
        {
            e->held_pressure[i] = NULL;
            emit(e, c, 0, true);
            c->pos = 0;
        }
    }
    changed(e);
}

void engine_reset_input(struct engine *e)
{
    engine_release_all(e);
    memset(e->buttons, 0, sizeof(e->buttons));
    e->shift = false;
    e->strip = 0;
    changed(e);
}

void engine_set_midi_mode(struct engine *e, bool on)
{
    if (!on) engine_release_all(e);
    e->midi_mode = on;
    changed(e);
}

/* navigation */

/* whether the template list, or the map's pages, wrap around at the ends:
 * <wrapmode> 0, the map's for its pages if it has one */
static bool wraps(const struct engine *e, bool pages)
{
    const struct map *map = engine_map(e);

    if (pages && map->wrapmode >= 0) return map->wrapmode == 0;
    return e->t.wrapmode == 0;
}

static int step_index(int index, int count, int dir, bool wrap)
{
    index += dir;
    if (index < 0) return wrap ? count - 1 : 0;
    if (index >= count) return wrap ? 0 : count - 1;
    return index;
}

static void select_lists(struct engine *e)
{
    const struct map *map = engine_map(e);

    e->selected_template = e->t.current;
    e->selected_page = map->page;
    e->selected_group = map->group;
}

static void select_template(struct engine *e, int index)
{
    if (index < 0 || index >= e->t.count || index == e->t.current) return;
    e->t.current = index;
    select_lists(e);
    changed(e);
}

/* n of ButtonN, 0 for other bits */
static int button_number(int bit)
{
    int n;

    for (n = 1; n <= 8; n++)
        if (bit == button_bit(n)) return n;
    return 0;
}

/* presses of Button1-8 the screens use; true if used */
static bool screen_button(struct engine *e, int bit)
{
    /* Button5-8 with Shift */
    static const int shift_screens[] = {SCREEN_MONITOR, SCREEN_KNOBS, SCREEN_PAGES, SCREEN_TEMPLATES};
    struct map *map = engine_map(e);
    int n = button_number(bit);

    if (!n) return false;
    if (e->shift)
    {
        if (n >= 5)
        {
            e->screen = shift_screens[n - 5];
            select_lists(e);
            changed(e);
        }
        return true;
    }
    if (e->screen == SCREEN_TEMPLATES)
    {
        /* LOAD */
        if (n == 6) select_template(e, e->selected_template);
        return true;
    }
    if (e->screen == SCREEN_PAGES)
    {
        /* LOAD for knob pages and for pad pages */
        if (n == 2 && e->selected_page < map->page_count) map->page = e->selected_page;
        if (n == 4 && e->selected_group < map->group_count) map->group = e->selected_group;
        changed(e);
        return true;
    }
    return false;
}

/* Left/Right: knob pages, templates with Shift */
static void step_page(struct engine *e, int dir)
{
    struct map *map = engine_map(e);

    if (e->shift) select_template(e, step_index(e->t.current, e->t.count, dir, wraps(e, false)));
    else if (map->page_count > 1) map->page = step_index(map->page, map->page_count, dir, wraps(e, true));
    changed(e);
}

static bool is_group_button(int bit)
{
    return bit >= BTN_GROUP_A && bit < BTN_GROUP_A + 8;
}

void engine_button(struct engine *e, int bit, bool down)
{
    struct map *map = engine_map(e);
    struct control *c;

    if (bit < 0 || bit >= BTN_COUNT || e->buttons[bit] == down) return;
    e->buttons[bit] = down;
    if (bit == BTN_SHIFT)
    {
        e->shift = down;
        changed(e);
        return;
    }
    /* a release goes where its press went, a gate never sticks */
    if (!down)
    {
        if ((c = e->held[bit]))
        {
            e->held[bit] = NULL;
            release(e, c);
            changed(e);
        }
        return;
    }
    if (!map) return;
    if (e->shift && bit == BTN_CHANNEL)
    {
        engine_set_midi_mode(e, !e->midi_mode);
        return;
    }
    if (!e->midi_mode || screen_button(e, bit)) return;
    if (map->knob_pages && (bit == BTN_LEFT || bit == BTN_RIGHT))
    {
        step_page(e, bit == BTN_RIGHT ? 1 : -1);
        return;
    }
    if (map->pad_pages && is_group_button(bit))
    {
        if (bit - BTN_GROUP_A < map->group_count) map->group = bit - BTN_GROUP_A;
        changed(e);
        return;
    }
    if (!(c = engine_find(e, mk3_buttons[bit].id))) return;
    /* with navigation, Shift makes every button a navigation button */
    if (e->shift && (map->knob_pages || map->pad_pages)) return;
    e->held[bit] = c;
    press(e, c);
    changed(e);
}

/* knobs, wheel, touch strip, pedal */

static int relative_value(int mode, int steps)
{
    if (mode == MODE_COMP) return steps < 0 ? steps + 128 : steps;
    return steps + 64;
}

/* bit 6 is the direction, the low bits the speed */
static int vpot_value(int steps)
{
    if (steps > 0) return clamp(steps, 0, 15);
    return 0x40 | clamp(-steps, 0, 15);
}

/* delta is in turns, or as a position 0..1 for the touch strip */
static void knob_turn(struct engine *e, struct control *c, float delta, bool position)
{
    float x, step;
    int n;

    if (c->mode == MODE_ABSOLUTE && c->message != MSG_MCU_VPOT)
    {
        x = c->pos + (position ? delta : delta * 360.0f / c->range);
        if (c->wrap) x -= floorf(x);
        else x = clampf(x, 0, 1);
        c->pos = x;
        if (c->message == MSG_MCU_CHANNEL || c->message == MSG_MCU_MASTER)
            emit(e, c, (int)lroundf(x * MIDI14_MAX), false);
        else emit(e, c, scale(c, x), false);
        return;
    }
    /* a step every 3600 / steps knob counts */
    step = 360.0f / c->steps;
    c->turns += delta;
    if (fabsf(c->turns * 100) < step) return;
    n = (int)lroundf(c->turns * 100 / step);
    if (c->inc) n *= c->inc;
    c->turns = 0;
    emit(e, c, c->message == MSG_MCU_VPOT ? vpot_value(n) : relative_value(c->mode, n), true);
}

static void list_scroll(struct engine *e, int *selected, int count, int knob, int counts)
{
    int *scroll = &e->scroll_counts[knob - 1], steps;

    *scroll += counts;
    steps = *scroll / SCROLL_COUNTS;
    *scroll %= SCROLL_COUNTS;
    if (!steps || count < 1) return;
    *selected = clamp(*selected + steps, 0, count - 1);
    changed(e);
}

void engine_knob(struct engine *e, int knob, int counts)
{
    struct map *map = engine_map(e);
    struct control *c;

    if (!map || !e->midi_mode || knob < 1 || knob > MK3_KNOBS) return;
    if (e->shift)
    {
        /* knob 5 of the Shift screen shows values or bars */
        if (knob == 5 && counts)
        {
            e->t.knob_mode = counts > 0;
            changed(e);
        }
        return;
    }
    if (e->screen == SCREEN_TEMPLATES)
    {
        if (knob >= 5) list_scroll(e, &e->selected_template, e->t.count, knob, counts);
        return;
    }
    if (e->screen == SCREEN_PAGES)
    {
        if (knob <= 2) list_scroll(e, &e->selected_page, map->page_count, knob, counts);
        else if (knob <= 4) list_scroll(e, &e->selected_group, map->group_count, knob, counts);
        return;
    }
    if ((c = engine_find_n(e, "Knob", knob)) && c->enabled)
        knob_turn(e, c, counts * (1.0f / MK3_KNOB_COUNTS), false);
}

void engine_encoder(struct engine *e, int ticks)
{
    struct control *c;
    unsigned char data[3];
    int n = ticks, v;

    if (!e->midi_mode || !(c = engine_find(e, "Encoder")) || !c->enabled) return;
    if (c->ticks > 1)
    {
        c->ticks_left += ticks;
        n = c->ticks_left / c->ticks;
        c->ticks_left %= c->ticks;
        if (!n) return;
    }
    if (c->message == MSG_MCU_WHEEL)
    {
        emit(e, c, n > 0 ? n : 0x40 | -n, true);
        return;
    }
    /* a note per step: min turning left, max turning right, velocity = steps */
    if (c->message == MSG_NOTE)
    {
        data[0] = MSG_NOTE | c->channel;
        data[1] = (n < 0 ? c->min : c->max) & 0x7f;
        data[2] = clamp(abs(n), 0, MIDI_MAX);
        send(e, c, data, 3);
        return;
    }
    if (c->mode != MODE_ABSOLUTE)
    {
        emit(e, c, relative_value(c->mode, c->inc ? n * c->inc : n), true);
        return;
    }
    v = fit(c, (long)c->last + (long)n * c->inc, c->wrap);
    c->pos = norm(c, v);
    emit(e, c, v, false);
}

void engine_strip(struct engine *e, int pos)
{
    struct control *c;
    float x;

    if (pos == e->strip) return;
    if (!e->midi_mode)
    {
        e->strip = pos;
        return;
    }
    /* touching and letting go press and release TouchstripCap */
    if ((e->strip == 0) != (pos == 0))
    {
        e->strip = pos;
        engine_button(e, BTN_TOUCHSTRIP_CAP, pos != 0);
        if (!pos) return;
    }
    e->strip = pos;
    if (!(c = engine_find(e, "Touchstrip")) || !c->enabled) return;
    x = (clamp(pos, 1, MK3_STRIP_MAX) - 1) / (float)(MK3_STRIP_MAX - 1);
    knob_turn(e, c, x - c->pos, true);
    if (c->mode != MODE_ABSOLUTE) c->pos = x;
    changed(e);
}

void engine_pedal(struct engine *e, int value)
{
    struct control *c;
    float x = clamp(value, 0, MK3_PEDAL_MAX) / (float)MK3_PEDAL_MAX;

    if (!e->midi_mode || !(c = engine_find(e, "Pedal")) || !c->enabled) return;
    c->pos = x;
    emit(e, c, scale(c, x), false);
}

/* pads */

/* <velocitycurve>: 0-2 soft to less soft, 3 linear, 4-6 less hard to hard, 7 fixed */
static float velocity_curve(int curve, float x)
{
    float u;

    x = clampf(x, 0, 1);
    u = 1 - x;
    switch (curve)
    {
    case 0: return 1 - (0.9f * u * u + 0.1f) * u;
    case 1: return 1 - (0.6f * u * u + 0.4f) * u;
    case 2: return 1 - (0.3f * u * u + 0.7f) * u;
    case 4: return (0.3f * x * x + 0.7f) * x;
    case 5: return (0.6f * x * x + 0.4f) * x;
    case 6: return (0.9f * x * x + 0.1f) * x;
    case 7: return 1;
    default: return x;
    }
}

/* PressureN: poly aftertouch and the like, or a note when pressed hard enough */
static void pad_pressure(struct engine *e, int pad, float x)
{
    struct control *c = e->held_pressure[pad];

    if (!c && !(c = engine_find_n(e, "Pressure", pad_number(pad)))) return;
    if (!c->enabled) return;
    x = clampf(x, 0, 1);
    if (c->message != MSG_NOTE) emit(e, c, scale(c, x), false);
    else if (c->pos < 0.5f && x >= c->on_pressure)
    {
        emit(e, c, c->max, true);
        c->pos = 1;
        e->held_pressure[pad] = c;
    }
    else if (c->pos >= 0.5f && x <= c->off_pressure)
    {
        emit(e, c, 0, true);
        c->pos = 0;
        e->held_pressure[pad] = NULL;
    }
}

static void pad_hit(struct engine *e, int pad, float x)
{
    struct map *map = engine_map(e);
    struct control *c = engine_find_n(e, "Pad", pad_number(pad));
    int velocity;

    if (!c) return;
    if (e->held_pad[pad]) pad_release(e, e->held_pad[pad]);
    e->held_pad[pad] = c;
    changed(e);
    if (c->message != MSG_NOTE)
    {
        press(e, c);
        return;
    }
    c->pressed = true;
    if (!c->enabled) return;
    if (c->behavior == BEHAVIOR_TOGGLE && c->pos >= 0.5f)
    {
        emit(e, c, 0, true);
        c->pos = 0;
        return;
    }
    velocity = clamp((int)lroundf(velocity_curve(map->velocity_curve, x) * MIDI_MAX), 0, MIDI_MAX);
    if (c->min > 0 || c->max < MIDI_MAX)
        velocity = c->min + (int)lroundf((c->max - c->min) * velocity / (float)MIDI_MAX);
    emit(e, c, velocity, true);
    c->pos = 1;
}

void engine_pad(struct engine *e, int pad, int kind, int value)
{
    struct control *c;
    float x = value / (float)MK3_PAD_MAX;

    if (!engine_map(e) || !e->midi_mode || pad < 0 || pad >= MK3_PADS) return;
    switch (kind)
    {
    case PAD_PRESSURE:
        pad_pressure(e, pad, x);
        break;
    case PAD_HIT_ON:
        pad_hit(e, pad, x);
        break;
    case PAD_HIT_OFF:
        if ((c = e->held_pad[pad]))
        {
            e->held_pad[pad] = NULL;
            pad_release(e, c);
            changed(e);
        }
        break;
    }
}

/* LEDs */

bool control_is_on(const struct control *c)
{
    switch (c->behavior)
    {
    case BEHAVIOR_TOGGLE:
        return c->pressed || c->pos >= 0.5f;
    case BEHAVIOR_INCREMENT:
        return norm(c, c->last) > 1.0f / 128;
    default:
        return c->onifdown ? c->pressed : c->last_ext != 0;
    }
}

static unsigned char control_led(const struct control *c)
{
    const struct control *led = c->led;
    bool on = control_is_on(c);
    int brightness = on ? LED_BRIGHT : LED_DIM;

    if (!c->enabled) return 0;
    /* white without a led element */
    if (!led) return led_byte(LED_WHITE, brightness);
    switch (led->color_mode)
    {
    case LED_MODE_DIM:
        return led_byte(palette_led(led->color_on), brightness);
    case LED_MODE_ON_OFF:
        return led_byte(palette_led(on ? led->color_on : led->color_off), LED_BRIGHT);
    case LED_MODE_RAW:
        return c->onifdown ? 0 : c->last_ext & 0x7f;
    default:
        return 0;
    }
}

static void strip_leds(const struct engine *e, unsigned char *leds)
{
    const struct control *c = engine_find(e, "Touchstrip");
    const int last = MK3_STRIP_LEDS - 1, center = last / 2;
    int i, color, end, dot = -1;
    bool lit;
    float v;

    if (!c || !c->enabled || c->mode != MODE_ABSOLUTE) return;
    color = palette_led(c->led ? c->led->color_on : PALETTE_WHITE);
    v = c->onifdown ? c->pos : norm(c, c->last_ext);
    /* a bar of the host's value with a dot where the finger was */
    if (c->ledpattern == STRIP_BAR_DOT)
    {
        v = norm(c, c->last_ext);
        dot = lroundf(last * c->pos);
    }
    end = lroundf(last * v);
    for (i = 0; i < MK3_STRIP_LEDS; i++)
    {
        switch (c->ledpattern)
        {
        case STRIP_DOT:
            lit = i == end;
            break;
        case STRIP_CENTER:
            lit = i == center || (i > center ? i <= end : i >= end);
            break;
        default:
            lit = i < lroundf(MK3_STRIP_LEDS * v);
            break;
        }
        leds[MK3_LED_STRIP + i] = led_byte(color, lit ? LED_BRIGHT : LED_DIM);
    }
    if (dot >= 0 && dot < MK3_STRIP_LEDS) leds[MK3_LED_STRIP + dot] = led_byte(LED_WHITE, LED_BRIGHT);
}

/* Left/Right light while there is somewhere to go */
static void arrow_leds(const struct engine *e, unsigned char *leds)
{
    const struct map *map = engine_map(e);
    int count = e->shift ? e->t.count : map->page_count;
    int current = e->shift ? e->t.current : map->page;
    bool wrap = wraps(e, !e->shift);
    bool left = count >= 2 && (wrap || current > 0);
    bool right = count >= 2 && (wrap || current < count - 1);

    leds[LED_LEFT] = led_byte(LED_WHITE, left ? LED_BRIGHT : LED_DIM);
    leds[LED_RIGHT] = led_byte(LED_WHITE, right ? LED_BRIGHT : LED_DIM);
}

void engine_leds(const struct engine *e, unsigned char leds[MK3_LEDS])
{
    const struct map *map = engine_map(e);
    const struct control *c;
    int bit, group, pad, n, screen = engine_shown_screen(e);

    memset(leds, 0, MK3_LEDS);
    if (!map || !e->midi_mode) return;
    for (bit = 0; bit < BTN_COUNT; bit++)
    {
        const struct button_info *b = &mk3_buttons[bit];

        if (!b->id || b->led < 0 || bit == BTN_SHIFT) continue;
        if (map->pad_pages && is_group_button(bit))
        {
            group = bit - BTN_GROUP_A;
            if (group < map->group_count)
                leds[b->led] = led_byte(palette_led(map->groups[group].color),
                                        group == map->group ? LED_BRIGHT : LED_DIM);
            continue;
        }
        if ((c = engine_find(e, b->id))) leds[b->led] = control_led(c);
    }
    leds[LED_SHIFT] = led_byte(LED_WHITE, e->shift ? LED_BRIGHT : LED_DIM);
    if (map->knob_pages) arrow_leds(e, leds);
    /* the Shift screen lights the screen buttons 5-8, TEMPLATES and PAGES none */
    for (n = 1; n <= 8; n++)
    {
        unsigned char *led = &leds[mk3_buttons[button_bit(n)].led];

        if (screen == SCREEN_SHIFT) *led = n >= 5 ? led_byte(LED_WHITE, LED_BRIGHT) : 0;
        else if (screen == SCREEN_TEMPLATES || screen == SCREEN_PAGES) *led = 0;
    }
    for (pad = 0; pad < MK3_PADS; pad++)
    {
        c = engine_pad_control(e, pad_number(pad));
        leds[MK3_LED_PADS + pad] = c ? control_led(c) : 0;
    }
    strip_leds(e, leds);
}

/* setup */

void engine_init(struct engine *e, void (*midi)(void *, const unsigned char *, int), void *ctx)
{
    memset(e, 0, sizeof(*e));
    e->midi = midi;
    e->ctx = ctx;
    e->screen = SCREEN_KNOBS;
    snprintf(e->monitor_name, sizeof(e->monitor_name), "none");
}

void engine_set_templates(struct engine *e, struct templates *t)
{
    int current = e->t.current, page = -1, group = -1;

    if (e->t.count)
    {
        engine_release_all(e);
        page = engine_map(e)->page;
        group = engine_map(e)->group;
    }
    templates_free(&e->t);
    e->t = *t;
    memset(t, 0, sizeof(*t));
    if (page >= 0 && current < e->t.count)
    {
        struct map *map = &e->t.maps[current];

        e->t.current = current;
        if (page < map->page_count) map->page = page;
        if (group < map->group_count) map->group = group;
    }
    e->screen = e->t.display_mode;
    if (e->screen != SCREEN_KNOBS && e->screen != SCREEN_MONITOR) e->screen = SCREEN_KNOBS;
    memset(e->buttons, 0, sizeof(e->buttons));
    e->shift = false;
    if (e->t.count) select_lists(e);
    changed(e);
}

void engine_free(struct engine *e)
{
    templates_free(&e->t);
}

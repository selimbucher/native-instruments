#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "template.h"
#include "util.h"
#include "xml.h"

static const unsigned int palette[PALETTE_SIZE] =
{
    0xff130f, 0xff3c14, 0xff7800, 0xffb900, 0xffff00, 0x92ff00, 0x26ff26, 0x00d181, 0x00b4e9, 0x0078ff,
    0x0048ff, 0x8200ff, 0xa200c8, 0xf500c8, 0xff0078, 0xf7073e, 0x000000, 0x5a5a5a, 0xffffff,
};

int palette_led(int index)
{
    if (index < 0 || index >= PALETTE_SIZE || index == PALETTE_BLACK) return 0;
    if (index == PALETTE_GREY) return LED_GREY;
    if (index == PALETTE_WHITE) return LED_WHITE;
    return index + 1;
}

unsigned int palette_rgb(int index)
{
    return index >= 0 && index < PALETTE_SIZE ? palette[index] : palette[PALETTE_WHITE];
}

bool toggle_value_on(const struct control *c, int v)
{
    return 2 * v >= c->min + c->max && v != c->min;
}

/* the array with one more zeroed item at the end, NULL if out of memory */
static void *grow(void *array, int count, size_t size)
{
    char *grown = realloc(array, (count + 1) * size);

    if (grown) memset(grown + count * size, 0, size);
    return grown;
}

/* a control with the agent's defaults */
static struct control *add(struct controls *list, enum control_type type, const char *id)
{
    struct control *items, *c;

    if (!(items = grow(list->items, list->count, sizeof(*items)))) return NULL;
    list->items = items;
    c = &items[list->count++];
    snprintf(c->id, sizeof(c->id), "%s", id ? id : "");
    c->type = type;
    c->strip = type == CONTROL_KNOB && !strcmp(c->id, "Touchstrip");
    c->message = MSG_CC;
    c->number = 1;
    c->max = 127;
    c->inc = 1;
    c->enabled = true;
    c->ondown = true;
    c->range = 360;
    c->steps = 20;
    c->on_pressure = 0.2f;
    c->off_pressure = 0.1f;
    c->behavior = type == CONTROL_PAD ? BEHAVIOR_GATE : BEHAVIOR_TOGGLE;
    c->color_on = 2;
    c->color_off = 1;
    c->led_index = -1;
    return c;
}

static int number(const char *s, int def)
{
    char *end;
    long v;

    if (!s) return def;
    v = strtol(s, &end, 10);
    return end == s ? def : (int)v;
}

static bool is_on(const char *s)
{
    return !strcmp(s, "on");
}

static void copy_attr(char *buf, int size, const struct xml *node, const char *name)
{
    const char *value = xml_attr(node, name);

    snprintf(buf, size, "%s", value ? value : "");
}

/* <note>, <controller mode="...">, <mcu-button> and so on; false if node is none of them */
static bool parse_message(struct control *c, const struct xml *node)
{
    static const struct
    {
        const char *name;
        int message;
        bool numbered;
    }
    messages[] =
    {
        {"note", MSG_NOTE, true},
        {"polyat", MSG_POLY_AT, true},
        {"controller", MSG_CC, true},
        {"program", MSG_PROGRAM, false},
        {"channelat", MSG_CHANNEL_AT, false},
        {"pitchbend", MSG_PITCH_BEND, false},
        {"songpos", MSG_SONG_POSITION, false},
        {"start", MSG_START, false},
        {"continue", MSG_CONTINUE, false},
        {"stop", MSG_STOP, false},
        {"mcu-button", MSG_MCU_BUTTON, true},
        {"mcu-vpot", MSG_MCU_VPOT, true},
        {"mcu-channel", MSG_MCU_CHANNEL, true},
        {"mcu-master", MSG_MCU_MASTER, false},
        {"mcu-wheel", MSG_MCU_WHEEL, false},
    };
    const char *mode;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(messages); i++)
        if (!strcmp(node->name, messages[i].name)) break;
    if (i == ARRAY_SIZE(messages)) return false;

    c->message = messages[i].message;
    if (messages[i].numbered) c->number = number(node->text, 0);
    if (c->message == MSG_PITCH_BEND)
    {
        c->min = 0;
        c->def = 0x2000;
        c->max = 0x3fff;
    }
    if (c->message == MSG_CC && (mode = xml_attr(node, "mode")))
    {
        if (!strcmp(mode, "comp")) c->mode = MODE_COMP;
        else if (!strcmp(mode, "offset")) c->mode = MODE_OFFSET;
        else c->mode = MODE_ABSOLUTE;
    }
    return true;
}

static void parse_behavior(struct control *c, const struct xml *node)
{
    const char *on_if_down = xml_attr(node, "onIfDown");
    const char *s = node->text;

    /* the agent accepts the misspelling "toogle" too */
    if (!strcmp(s, "toggle") || !strcmp(s, "toogle")) c->behavior = BEHAVIOR_TOGGLE;
    else if (!strcmp(s, "trigger")) c->behavior = BEHAVIOR_TRIGGER;
    else if (!strcmp(s, "gate")) c->behavior = BEHAVIOR_GATE;
    else if (!strcmp(s, "increment")) c->behavior = BEHAVIOR_INCREMENT;
    if (on_if_down) c->onifdown = is_on(on_if_down);
}

/* <display><unit color-mode="..." color-on-index="..." color-off-index="..."/></display> */
static void parse_display(struct control *c, const struct xml *node)
{
    const struct xml *unit = node->child;

    if (!unit || strcmp(unit->name, "unit")) return;
    c->color_mode = number(xml_attr(unit, "color-mode"), c->color_mode);
    c->color_on = number(xml_attr(unit, "color-on-index"), c->color_on);
    c->color_off = number(xml_attr(unit, "color-off-index"), c->color_off);
}

/* the state NI's agent saved with the template */
static void init_state(struct control *c, bool have_last)
{
    if (!have_last) c->last = c->def;
    if (c->max != c->min) c->pos = clampf((float)(c->last - c->min) / (c->max - c->min), 0, 1);
    /* so that the first press sends min */
    if (c->behavior == BEHAVIOR_INCREMENT && !have_last)
        c->last = c->wrap ? c->max - c->inc + 1 : c->min - c->inc;
    if (c->type == CONTROL_BUTTON || c->type == CONTROL_FOOTSWITCH || c->type == CONTROL_PAD)
        c->pos = c->behavior == BEHAVIOR_TOGGLE && toggle_value_on(c, c->last);
}

static void parse_control(struct control *c, const struct xml *node)
{
    bool have_last = false;
    const struct xml *e;
    int steps;

    c->ticks = number(xml_attr(node, "ticks"), c->ticks);
    for (e = node->child; e; e = e->next)
    {
        const char *name = e->name, *text = e->text;

        if (parse_message(c, e)) continue;
        if (!strcmp(name, "channel")) c->channel = number(text, 0) & 15;
        else if (!strcmp(name, "min") || !strcmp(name, "off")) c->min = number(text, c->min);
        else if (!strcmp(name, "max") || !strcmp(name, "on")) c->max = number(text, c->max);
        else if (!strcmp(name, "default")) c->def = number(text, c->def);
        else if (!strcmp(name, "inc")) c->inc = number(text, c->inc);
        else if (!strcmp(name, "wrap")) c->wrap = is_on(text);
        else if (!strcmp(name, "range")) c->range = clamp(number(text, 360), 30, 3600);
        else if (!strcmp(name, "steps"))
        {
            steps = number(text, 20);
            c->steps = steps > 0 ? steps : 20;
        }
        else if (!strcmp(name, "bipolar")) c->bipolar = is_on(text);
        else if (!strcmp(name, "ledPattern")) c->ledpattern = number(text, 0);
        else if (!strcmp(name, "last"))
        {
            c->last = number(text, 0);
            have_last = true;
        }
        else if (!strcmp(name, "lastExt")) c->last_ext = number(text, 0);
        else if (!strcmp(name, "behavior")) parse_behavior(c, e);
        else if (!strcmp(name, "reaction"))
        {
            if (!strcmp(text, "ondown")) c->ondown = true;
            else if (!strcmp(text, "onup")) c->ondown = false;
        }
        else if (!strcmp(name, "note-on-pressure")) c->on_pressure = number(text, 200) / 1000.0f;
        else if (!strcmp(name, "note-off-pressure")) c->off_pressure = number(text, 100) / 1000.0f;
        else if (!strcmp(name, "disabled")) c->enabled = false;
        else if (!strcmp(name, "enabled")) c->enabled = true;
        else if (!strcmp(name, "name")) snprintf(c->name, sizeof(c->name), "%s", text);
        else if (!strcmp(name, "display")) parse_display(c, e);
    }

    /* MIDI values, whatever the file says */
    c->number = clamp(c->number, 0, 127);
    c->min = clamp(c->min, 0, 0x3fff);
    c->max = clamp(c->max, 0, 0x3fff);
    c->def = clamp(c->def, 0, 0x3fff);
    c->last = clamp(c->last, -0x4000, 0x3fff);
    c->last_ext = clamp(c->last_ext, 0, 0x3fff);
    c->inc = clamp(c->inc, -0x3fff, 0x3fff);
    init_state(c, have_last);
}

/* The led of control X is an <led> inside it or "XIDX" next to it. Only
 * once the list is complete: adding moves the items. */
static void link_leds(struct controls *list)
{
    char id[40];
    int i, j;

    for (i = 0; i < list->count; i++)
    {
        struct control *c = &list->items[i];

        c->led = NULL;
        if (c->type == CONTROL_LED) continue;
        if (c->led_index >= 0)
        {
            c->led = &list->items[c->led_index];
            continue;
        }
        snprintf(id, sizeof(id), "%sIDX", c->id);
        for (j = 0; j < list->count; j++)
        {
            struct control *led = &list->items[j];

            if (led->type == CONTROL_LED && !strcmp(led->id, id)) c->led = led;
        }
    }
}

static int parse_controls(struct controls *list, const struct xml *parent)
{
    static const struct
    {
        const char *name;
        enum control_type type;
    }
    types[] =
    {
        {"knob", CONTROL_KNOB},
        {"pedal", CONTROL_PEDAL},
        {"button", CONTROL_BUTTON},
        {"footswitch", CONTROL_FOOTSWITCH},
        {"wheel", CONTROL_WHEEL},
        {"pad", CONTROL_PAD},
        {"led", CONTROL_LED},
    };
    struct control *c, *led;
    const struct xml *e, *led_node;
    const char *subtype;
    unsigned int i;
    int index;

    for (e = parent->child; e; e = e->next)
    {
        for (i = 0; i < ARRAY_SIZE(types); i++)
            if (!strcmp(e->name, types[i].name)) break;
        if (i == ARRAY_SIZE(types)) continue;
        if (!(c = add(list, types[i].type, xml_attr(e, "id")))) return -1;
        subtype = xml_attr(e, "subtype");
        if (c->type == CONTROL_PAD && subtype && !strcmp(subtype, "pressure")) c->type = CONTROL_PRESSURE;
        parse_control(c, e);
        if ((led_node = xml_child(e, "led")))
        {
            index = c - list->items;
            if (!(led = add(list, CONTROL_LED, ""))) return -1;
            parse_control(led, led_node);
            list->items[index].led_index = list->count - 1;
        }
    }
    link_leds(list);
    return 0;
}

struct control *controls_find(const struct controls *list, const char *id)
{
    int i;

    for (i = 0; i < list->count; i++)
        if (list->items[i].type != CONTROL_LED && !strcmp(list->items[i].id, id)) return &list->items[i];
    return NULL;
}

static int parse_page(struct map *map, const struct xml *node)
{
    struct page *pages, *page;
    char attr[32];
    int i;

    if (!(pages = grow(map->pages, map->page_count, sizeof(*pages)))) return -1;
    map->pages = pages;
    page = &pages[map->page_count++];
    copy_attr(page->name, sizeof(page->name), node, "name");
    for (i = 0; i < 8; i++)
    {
        snprintf(attr, sizeof(attr), "colorColumn%d-index", i + 1);
        page->column[i] = number(xml_attr(node, attr), PALETTE_WHITE);
        snprintf(attr, sizeof(attr), "colorColumn%dB-index", i + 1);
        page->column_b[i] = number(xml_attr(node, attr), PALETTE_WHITE);
    }
    page->explicit_mcu = number(xml_attr(node, "explicitMCU"), 0) != 0;
    return parse_controls(&page->controls, node);
}

static int parse_group(struct map *map, const struct xml *node)
{
    struct group *groups, *group;

    if (!(groups = grow(map->groups, map->group_count, sizeof(*groups)))) return -1;
    map->groups = groups;
    group = &groups[map->group_count++];
    copy_attr(group->name, sizeof(group->name), node, "name");
    group->color = number(xml_attr(node, "color-index"), 9);
    return parse_controls(&group->controls, node);
}

static int parse_map(struct map *map, const struct xml *node)
{
    const struct xml *section, *e;

    copy_attr(map->name, sizeof(map->name), node, "name");
    map->velocity_curve = number(xml_text(node, "velocitycurve"), 3);
    map->wrapmode = number(xml_text(node, "wrapmode"), -1);
    map->knob_pages = !xml_child(node, "handleKnobControls");
    map->pad_pages = !xml_child(node, "handleGroupControls");
    map->handle_transport = number(xml_text(node, "handleTransport"), 0) != 0;
    if ((section = xml_child(node, "controls")) && parse_controls(&map->controls, section)) return -1;
    if ((section = xml_child(node, "pages")))
    {
        for (e = section->child; e; e = e->next)
            if (!strcmp(e->name, "page") && parse_page(map, e)) return -1;
        map->page = number(xml_text(section, "current_index"), 0);
    }
    if ((section = xml_child(node, "groups")))
    {
        for (e = section->child; e; e = e->next)
            if (!strcmp(e->name, "group") && parse_group(map, e)) return -1;
        map->group = number(xml_text(section, "current_index"), 0);
    }
    if (map->page < 0 || map->page >= map->page_count) map->page = 0;
    if (map->group < 0 || map->group >= map->group_count) map->group = 0;
    return 0;
}

/* the whole file as a string, NULL if it can't be read */
static char *read_all(const char *path)
{
    char *data = NULL;
    FILE *file;
    long size;

    if (!(file = fopen(path, "rb"))) return NULL;
    if (!fseek(file, 0, SEEK_END) && (size = ftell(file)) >= 0 && !fseek(file, 0, SEEK_SET)
            && (data = malloc(size + 1)))
    {
        if (fread(data, 1, size, file) == (size_t)size) data[size] = 0;
        else
        {
            free(data);
            data = NULL;
        }
    }
    fclose(file);
    return data;
}

int templates_load(struct templates *t, const char *path)
{
    const struct xml *e, *state;
    struct xml *root;
    struct map *maps;
    char *data;
    int ret = -1;

    memset(t, 0, sizeof(*t));
    if (!(data = read_all(path))) return -1;
    root = xml_parse(data);
    free(data);
    if (!root || strcmp(root->name, "ni-controller-midi-map")) goto done;

    t->wrapmode = number(xml_text(root, "wrapmode"), 0);
    t->current = number(xml_text(root, "current_index"), 0);
    t->display_mode = 4;
    if ((state = xml_child(root, "display_state")))
    {
        t->display_mode = number(xml_text(state, "display_mode"), 4);
        t->knob_mode = number(xml_text(state, "knob_mode"), 0);
    }
    for (e = root->child; e; e = e->next)
    {
        if (strcmp(e->name, "midi-map")) continue;
        if (!(maps = grow(t->maps, t->count, sizeof(*maps)))) goto done;
        t->maps = maps;
        if (parse_map(&maps[t->count++], e)) goto done;
    }
    if (t->current < 0 || t->current >= t->count) t->current = 0;
    ret = t->count ? 0 : -1;
done:
    xml_free(root);
    if (ret) templates_free(t);
    return ret;
}

/* the default template */

/* a control with the LED following the button, not the host */
static struct control *add_cc(struct controls *list, enum control_type type, const char *id, int cc)
{
    struct control *c = add(list, type, id);

    if (!c) return NULL;
    c->number = cc;
    c->onifdown = true;
    return c;
}

static void add_led(struct controls *list, const char *id, int message, int number)
{
    char name[40];
    struct control *led;

    snprintf(name, sizeof(name), "%sIDX", id);
    if (!(led = add(list, CONTROL_LED, name))) return;
    led->message = message;
    led->number = number;
}

/* the MK3's buttons that NI's default template treats specially */
#define MK3_ENCODER_UP 0x02     /* to 0x05: up, right, down, left */
#define MK3_ENCODER_LEFT 0x05
#define MK3_ENCODER_CAP 0x47

static void mk3_controls(struct map *map)
{
    const struct model *m = &maschine_mk3;
    struct control *c;
    bool footswitch, encoder_direction;
    int bit;

    for (bit = 0; bit <= m->touchstrip_cap; bit++)
    {
        const struct button_info *b = &m->buttons[bit];

        /* Button1-8 and KnobCap1-8 belong to the knob pages */
        if (!b->id || bit == m->shift) continue;
        if (starts_with(b->id, "Button") || starts_with(b->id, "KnobCap")) continue;
        footswitch = starts_with(b->id, "FSw");
        encoder_direction = bit >= MK3_ENCODER_UP && bit <= MK3_ENCODER_LEFT;
        c = add_cc(&map->controls, footswitch ? CONTROL_FOOTSWITCH : CONTROL_BUTTON, b->id, b->cc);
        if (!c) return;
        if (footswitch || encoder_direction) c->behavior = BEHAVIOR_GATE;
        if (bit == MK3_ENCODER_CAP || bit == m->touchstrip_cap) c->enabled = false;
        if (bit == m->touchstrip_cap)
        {
            c->message = MSG_POLY_AT;
            c->number = 2;
        }
        if (encoder_direction || starts_with(b->id, "Group")) add_led(&map->controls, b->id, MSG_CC, b->cc);
    }
    add_cc(&map->controls, CONTROL_WHEEL, "Encoder", 8);
    add_cc(&map->controls, CONTROL_KNOB, "Touchstrip", 1);
    add_led(&map->controls, "Touchstrip", MSG_CC, 1);
    add_cc(&map->controls, CONTROL_PEDAL, "Pedal", 11);
    link_leds(&map->controls);
}

/* two knob pages: Button1-8, Knob1-8 and the disabled KnobCap1-8 */
static void mk3_pages(struct map *map)
{
    static const int button_ccs[2][8] = {{22, 23, 24, 25, 26, 27, 28, 29}, {60, 61, 62, 63, 66, 67, 68, 69}};
    static const int first_knob_cc[2] = {70, 108};
    struct control *c;
    char id[32];
    int i, j;

    if (!(map->pages = calloc(2, sizeof(*map->pages)))) return;
    map->page_count = 2;
    for (i = 0; i < 2; i++)
    {
        struct page *page = &map->pages[i];

        snprintf(page->name, sizeof(page->name), "Knob Page %d", i + 1);
        for (j = 0; j < 8; j++) page->column[j] = page->column_b[j] = PALETTE_WHITE;
        for (j = 0; j < 8; j++)
        {
            snprintf(id, sizeof(id), "Button%d", j + 1);
            add_cc(&page->controls, CONTROL_BUTTON, id, button_ccs[i][j]);
        }
        for (j = 0; j < 8; j++)
        {
            snprintf(id, sizeof(id), "Knob%d", j + 1);
            add_cc(&page->controls, CONTROL_KNOB, id, first_knob_cc[i] + j);
        }
        for (j = 0; j < 8; j++)
        {
            snprintf(id, sizeof(id), "KnobCap%d", j + 1);
            if ((c = add_cc(&page->controls, CONTROL_BUTTON, id, 112 + j))) c->enabled = false;
        }
    }
}

static void mikro_controls(struct map *map)
{
    const struct model *m = &maschine_mikro_mk3;
    struct control *c;
    int bit;

    for (bit = 0; bit < m->touchstrip_cap; bit++)
    {
        const struct button_info *b = &m->buttons[bit];

        if (!b->id || bit == m->shift) continue;
        if (!(c = add_cc(&map->controls, CONTROL_BUTTON, b->id, b->cc))) return;
        if (!strcmp(b->id, "Navigation Touch")) c->enabled = false;
    }
    add_cc(&map->controls, CONTROL_WHEEL, "Navigation", 7);
    add_cc(&map->controls, CONTROL_KNOB, "Touchstrip", 1);
    add_led(&map->controls, "Touchstrip", MSG_CC, 1);
    if ((c = add_cc(&map->controls, CONTROL_BUTTON, "TouchstripCap", 2)))
    {
        c->message = MSG_POLY_AT;
        c->enabled = false;
    }
    link_leds(&map->controls);
}

/* the Mikro has no knobs: one empty knob page */
static void mikro_pages(struct map *map)
{
    struct page *page;
    int i;

    if (!(page = map->pages = calloc(1, sizeof(*map->pages)))) return;
    map->page_count = 1;
    snprintf(page->name, sizeof(page->name), "Knob Page");
    for (i = 0; i < 8; i++) page->column[i] = page->column_b[i] = PALETTE_WHITE;
}

/* pad pages A-H: an octave higher each, from note 12 */
static void default_groups(struct map *map, int color)
{
    struct control *c;
    char id[32];
    int i, pad, note;

    if (!(map->groups = calloc(8, sizeof(*map->groups)))) return;
    map->group_count = 8;
    for (i = 0; i < 8; i++)
    {
        struct group *group = &map->groups[i];

        snprintf(group->name, sizeof(group->name), "Pad Page %c", 'A' + i);
        group->color = color;
        for (pad = 1; pad <= PADS; pad++)
        {
            note = 12 * (i + 1) + pad - 1;
            snprintf(id, sizeof(id), "Pad%d", pad);
            if ((c = add_cc(&group->controls, CONTROL_PAD, id, note))) c->message = MSG_NOTE;
            add_led(&group->controls, id, MSG_NOTE, note);
            snprintf(id, sizeof(id), "Pressure%d", pad);
            if ((c = add_cc(&group->controls, CONTROL_PRESSURE, id, note))) c->message = MSG_POLY_AT;
        }
        link_leds(&group->controls);
    }
}

void templates_default(struct templates *t, const struct model *model)
{
    struct map *map;

    memset(t, 0, sizeof(*t));
    t->display_mode = 4;
    if (!(t->maps = calloc(1, sizeof(*map)))) return;
    t->count = 1;
    map = t->maps;
    snprintf(map->name, sizeof(map->name), "Template 1");
    map->velocity_curve = 3;
    map->wrapmode = -1;
    map->knob_pages = true;
    map->pad_pages = true;
    if (model == &maschine_mikro_mk3)
    {
        mikro_controls(map);
        mikro_pages(map);
        default_groups(map, PALETTE_RED);
    }
    else
    {
        mk3_controls(map);
        mk3_pages(map);
        default_groups(map, PALETTE_BLUE);
    }
}

static void controls_free(struct controls *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void templates_free(struct templates *t)
{
    int i, j;

    for (i = 0; i < t->count; i++)
    {
        struct map *map = &t->maps[i];

        controls_free(&map->controls);
        for (j = 0; j < map->page_count; j++) controls_free(&map->pages[j].controls);
        for (j = 0; j < map->group_count; j++) controls_free(&map->groups[j].controls);
        free(map->pages);
        free(map->groups);
    }
    free(t->maps);
    memset(t, 0, sizeof(*t));
}

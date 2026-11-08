#include <stdio.h>
#include <string.h>
#include "screen.h"
#include "font.h"

#define BG 0x131313
#define LINE 0x2c2c2c
#define GREY 0x707070
#define WHITE 0xffffff

/* a quarter of a screen: the column of one knob and one of Button1-8 */
#define CELL_W 120
/* the row of Button1-8 labels at the top, the knob row at the bottom */
#define TOP_H 40
#define KNOBS_Y 200
#define MARGIN 16
#define HEADER_Y (TOP_H + 12)
#define TITLE_Y (TOP_H + 8)
#define LIST_Y (TOP_H + 30)
#define HINT_Y (KNOBS_Y + 24)
#define KNOB_LABEL_Y (KNOBS_Y + 10)
#define KNOB_VALUE_Y (KNOBS_Y + 34)
#define KNOB_BAR_Y (KNOBS_Y + 40)
#define KNOB_BAR_H 14
#define KNOB_BAR_W (CELL_W - 24)
#define MAX_ITEMS 64

struct font
{
    const unsigned short *glyphs;
    int w, h;
};

static const struct font small = {&font8x16[0][0], 8, 16};
static const struct font medium = {&font12x24[0][0], 12, 24};
static const struct font large = {&font16x32[0][0], 16, 32};

static unsigned short rgb565(unsigned int c)
{
    return (c >> 8 & 0xf800) | (c >> 5 & 0x07e0) | (c >> 3 & 0x1f);
}

static void fill(struct canvas *c, int x, int y, int w, int h, unsigned int rgb)
{
    unsigned short v = rgb565(rgb);
    int i, j;

    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) c->px[j][i] = v;
}

/* decodes the UTF-8 character at *s and moves past it, '?' if it is broken */
static unsigned int next_char(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned int ch;
    int more;

    if (*p < 0x80)
    {
        ch = *p;
        more = 0;
    }
    else if ((*p & 0xe0) == 0xc0)
    {
        ch = *p & 0x1f;
        more = 1;
    }
    else if ((*p & 0xf0) == 0xe0)
    {
        ch = *p & 0x0f;
        more = 2;
    }
    else
    {
        ch = *p & 0x07;
        more = 3;
    }
    for (p++; more && (*p & 0xc0) == 0x80; more--, p++) ch = ch << 6 | (*p & 0x3f);
    *s = (const char *)p;
    return more ? '?' : ch;
}

/* the glyph of ch in the fonts: ASCII, Latin-1, else '?' */
static int glyph_index(unsigned int ch)
{
    if (ch >= 32 && ch < 127) return ch - 32;
    if (ch >= 160 && ch < 256) return 95 + ch - 160;
    return '?' - 32;
}

static int text_width(const struct font *f, const char *s)
{
    int n = 0;

    while (*s)
    {
        next_char(&s);
        n++;
    }
    return n * f->w;
}

/* draws at most max_w pixels of s */
static void text(struct canvas *c, const struct font *f, int x, int y, const char *s, unsigned int rgb,
                 int max_w)
{
    unsigned short v = rgb565(rgb);
    const unsigned short *g;
    int i, j;

    while (*s && max_w >= f->w)
    {
        g = f->glyphs + glyph_index(next_char(&s)) * f->h;
        for (j = 0; j < f->h; j++)
            for (i = 0; i < f->w; i++)
            {
                if (!(g[j] >> (15 - i) & 1)) continue;
                if (x + i >= 0 && x + i < SCREEN_W && y + j >= 0 && y + j < SCREEN_H) c->px[y + j][x + i] = v;
            }
        x += f->w;
        max_w -= f->w;
    }
}

static void text_center(struct canvas *c, const struct font *f, int x, int y, int w, const char *s,
                        unsigned int rgb)
{
    int tw = text_width(f, s);

    if (tw > w) tw = w - w % f->w;
    text(c, f, x + (w - tw) / 2, y, s, rgb, w);
}

static const struct page *current_page(const struct engine *e)
{
    const struct map *map = engine_map(e);

    return map->page_count ? &map->pages[map->page] : NULL;
}

static void separators(struct canvas *c)
{
    int i;

    for (i = 1; i < 4; i++)
    {
        fill(c, i * CELL_W, 6, 1, TOP_H - 12, LINE);
        fill(c, i * CELL_W, KNOBS_Y + 8, 1, SCREEN_H - KNOBS_Y - 16, LINE);
    }
    fill(c, 0, TOP_H, SCREEN_W, 1, LINE);
    fill(c, 0, KNOBS_Y, SCREEN_W, 1, LINE);
}

/* a label over one of Button1-8, filled when on */
static void top_cell(struct canvas *c, int i, const char *label, unsigned int rgb, bool on, bool enabled)
{
    int x = i * CELL_W;

    if (!enabled) text_center(c, &small, x + 4, 12, CELL_W - 8, label, GREY);
    else if (on)
    {
        fill(c, x + 6, 5, CELL_W - 12, TOP_H - 10, rgb);
        text_center(c, &small, x + 8, 12, CELL_W - 16, label, BG);
    }
    else
    {
        fill(c, x + 6, TOP_H - 6, CELL_W - 12, 2, rgb);
        text_center(c, &small, x + 8, 12, CELL_W - 16, label, rgb);
    }
}

/* Button1-4 on the left screen, 5-8 on the right one */
static void page_buttons(struct canvas *c, const struct engine *e, int screen)
{
    const struct page *page = current_page(e);
    struct control *button;
    char label[48];
    int i, n;

    for (i = 0; i < 4; i++)
    {
        n = screen * 4 + i + 1;
        if (!(button = engine_find_n(e, "Button", n))) continue;
        control_label(button, label, sizeof(label));
        top_cell(c, i, label, page ? palette_rgb(page->column_b[n - 1]) : WHITE, control_is_on(button),
                 button->enabled);
    }
}

/* a bar from the left, or from the middle for bipolar knobs */
static void knob_bar(struct canvas *c, const struct control *knob, int x, unsigned int rgb)
{
    int w = (int)(KNOB_BAR_W * knob->pos + 0.5f), mid = KNOB_BAR_W / 2;

    fill(c, x, KNOB_BAR_Y, KNOB_BAR_W, KNOB_BAR_H, LINE);
    if (!knob->bipolar) fill(c, x, KNOB_BAR_Y, w, KNOB_BAR_H, rgb);
    else if (w >= mid) fill(c, x + mid, KNOB_BAR_Y, w - mid, KNOB_BAR_H, rgb);
    else fill(c, x + w, KNOB_BAR_Y, mid - w, KNOB_BAR_H, rgb);
}

/* Knob1-4 on the left screen, 5-8 on the right one: label and value or bar */
static void knob_cells(struct canvas *c, const struct engine *e, int screen)
{
    const struct page *page = current_page(e);
    struct control *knob;
    char label[48], value[16];
    unsigned int rgb;
    int i, n, x;

    for (i = 0; i < 4; i++)
    {
        n = screen * 4 + i + 1;
        x = i * CELL_W;
        rgb = page ? palette_rgb(page->column[n - 1]) : WHITE;
        if (!(knob = engine_find_n(e, "Knob", n)))
        {
            text_center(c, &small, x + 4, KNOB_LABEL_Y, CELL_W - 8, "EMPTY", GREY);
            continue;
        }
        control_label(knob, label, sizeof(label));
        text_center(c, &small, x + 4, KNOB_LABEL_Y, CELL_W - 8, label, knob->enabled ? rgb : GREY);
        if (!knob->enabled) continue;
        if (e->t.knob_mode)
        {
            knob_bar(c, knob, x + 12, rgb);
            continue;
        }
        if (knob->bipolar) snprintf(value, sizeof(value), "%1.2f", 2 * knob->pos - 1);
        else snprintf(value, sizeof(value), "%d", knob->mode == MODE_ABSOLUTE ? knob->last : knob->last_ext);
        text_center(c, &medium, x + 4, KNOB_VALUE_Y, CELL_W - 8, value, WHITE);
    }
}

/* "Template 01" over the template's name, the same for the pages below */
static void header(struct canvas *c, const struct engine *e, bool highlight)
{
    const struct map *map = engine_map(e);
    int y = HEADER_Y, w = SCREEN_W - 2 * MARGIN;
    char label[32];

    if (highlight) fill(c, 8, y - 4, SCREEN_W - 16, 48, LINE);
    snprintf(label, sizeof(label), "Template %02d", e->t.current + 1);
    text(c, &small, MARGIN, y, label, GREY, w);
    text(c, &medium, MARGIN, y + 16, map->name, WHITE, w);
    y += 50;
    if (map->knob_pages && map->page_count)
    {
        snprintf(label, sizeof(label), "Knob Page %02d", map->page + 1);
        text(c, &small, MARGIN, y, label, GREY, w);
        text(c, &medium, MARGIN, y + 16, map->pages[map->page].name, WHITE, w);
        y += 50;
    }
    if (map->pad_pages && map->group_count)
    {
        snprintf(label, sizeof(label), "Pad Page %02d", map->group + 1);
        text(c, &small, MARGIN, y, label, GREY, w);
        text(c, &medium, MARGIN, y + 16, map->groups[map->group].name, WHITE, w);
    }
}

static void pad_label(const struct control *pad, char *buf, int size)
{
    if (!pad || !pad->enabled) snprintf(buf, size, "Off");
    else if (pad->name[0]) snprintf(buf, size, "%s", pad->name);
    else if (pad->message == MSG_NOTE) note_name(pad->number, buf, size);
    else control_label(pad, buf, size);
}

/* a cell filled when on, else a label in its colour */
static void cell(struct canvas *c, int x, int y, int h, int text_y, const char *label, unsigned int rgb,
                 bool on)
{
    if (on)
    {
        fill(c, x, y, CELL_W - 12, h, rgb);
        text_center(c, &small, x + 2, y + text_y, CELL_W - 16, label, BG);
    }
    else text_center(c, &small, x + 2, y + text_y, CELL_W - 16, label, rgb);
}

/* the pad pages (or Group A-H buttons) in two rows, then the pads of the current page */
static void pads(struct canvas *c, const struct engine *e)
{
    const struct map *map = engine_map(e);
    struct control *pad, *button;
    char label[48];
    unsigned int rgb;
    int i, x, y;
    bool on;

    for (i = 0; i < 8; i++)
    {
        x = (i % 4) * CELL_W + 6;
        y = TOP_H + 6 + (i / 4) * 30;
        if (map->pad_pages)
        {
            if (i >= map->group_count) continue;
            rgb = palette_rgb(map->groups[i].color);
            snprintf(label, sizeof(label), "%s", map->groups[i].name);
        }
        else
        {
            if (!(button = engine_find(e, mk3_buttons[BTN_GROUP_A + i].id))) continue;
            rgb = button->led ? palette_rgb(button->led->color_on) : WHITE;
            control_label(button, label, sizeof(label));
        }
        cell(c, x, y, 26, 5, label, rgb, map->pad_pages && i == map->group);
    }
    fill(c, 0, TOP_H + 70, SCREEN_W, 1, LINE);
    for (i = 0; i < MK3_PADS; i++)
    {
        pad = engine_pad_control(e, pad_number(i));
        x = (i % 4) * CELL_W + 6;
        y = TOP_H + 76 + (i / 4) * 21;
        pad_label(pad, label, sizeof(label));
        on = pad && pad->enabled && control_is_on(pad);
        if (!pad || !pad->enabled) rgb = GREY;
        else rgb = pad->led ? palette_rgb(pad->led->color_on) : WHITE;
        cell(c, x, y, 20, 2, label, rgb, on);
    }
}

/* a scrolling list of rows, the selected one filled, the current one marked */
static void list(struct canvas *c, const struct font *f, int x, int y, int w, int rows,
                 const char *const *items, int count, int selected, int current)
{
    char line[64];
    int i, first = selected - rows / 2;

    if (first > count - rows) first = count - rows;
    if (first < 0) first = 0;
    for (i = first; i < count && i < first + rows; i++, y += f->h + 4)
    {
        snprintf(line, sizeof(line), "%c%02d - %s", i == current ? '>' : ' ', i + 1, items[i]);
        if (i == selected)
        {
            fill(c, x, y - 2, w, f->h + 4, WHITE);
            text(c, f, x + 4, y, line, BG, w - 8);
        }
        else text(c, f, x + 4, y, line, i == current ? WHITE : GREY, w - 8);
    }
}

static void screen_templates(struct canvas s[2], const struct engine *e)
{
    const char *names[MAX_ITEMS];
    int i, n = e->t.count < MAX_ITEMS ? e->t.count : MAX_ITEMS;

    header(&s[0], e, false);
    for (i = 0; i < n; i++) names[i] = e->t.maps[i].name;
    text(&s[1], &small, MARGIN, TITLE_Y, "TEMPLATES", GREY, SCREEN_W);
    list(&s[1], &medium, 8, LIST_Y, SCREEN_W - 16, 5, names, n, e->selected_template, e->t.current);
    top_cell(&s[1], 1, "LOAD", WHITE, false, true);
    text_center(&s[1], &small, 0, HINT_Y, SCREEN_W, "turn knobs 5-8 to select", GREY);
}

/* knob pages on the left half, pad pages on the right half */
static void screen_pages(struct canvas s[2], const struct engine *e)
{
    const struct map *map = engine_map(e);
    const int half = SCREEN_W / 2;
    const char *names[MAX_ITEMS];
    int i, n;

    text(&s[0], &small, MARGIN, TITLE_Y, "KNOB PAGES", GREY, half);
    text(&s[0], &small, half + MARGIN, TITLE_Y, "PAD PAGES", GREY, half);
    n = map->page_count < MAX_ITEMS ? map->page_count : MAX_ITEMS;
    for (i = 0; i < n; i++) names[i] = map->pages[i].name;
    list(&s[0], &small, 8, LIST_Y, half - 16, 6, names, n, e->selected_page, map->page);
    n = map->group_count < MAX_ITEMS ? map->group_count : MAX_ITEMS;
    for (i = 0; i < n; i++) names[i] = map->groups[i].name;
    list(&s[0], &small, half + 8, LIST_Y, half - 16, 6, names, n, e->selected_group, map->group);
    if (map->page_count) top_cell(&s[0], 1, "LOAD", WHITE, false, true);
    if (map->group_count) top_cell(&s[0], 3, "LOAD", WHITE, false, true);
    text_center(&s[0], &small, 0, HINT_Y, half, "knobs 1-2", GREY);
    text_center(&s[0], &small, half, HINT_Y, half, "knobs 3-4", GREY);
    header(&s[1], e, false);
}

static void monitor_line(struct canvas *c, int y, const char *what, const char *value)
{
    char line[96];

    snprintf(line, sizeof(line), "%s: %s", what, value[0] ? value : "none");
    text(c, &small, MARGIN, y, line, WHITE, SCREEN_W - 2 * MARGIN);
}

static void screen_monitor(struct canvas s[2], const struct engine *e)
{
    header(&s[0], e, false);
    page_buttons(&s[0], e, 0);
    page_buttons(&s[1], e, 1);
    text(&s[1], &small, MARGIN, TITLE_Y, "MONITOR", GREY, SCREEN_W);
    text(&s[1], &medium, MARGIN, LIST_Y, e->monitor_name, WHITE, SCREEN_W - 2 * MARGIN);
    if (e->monitor_pad)
    {
        monitor_line(&s[1], TOP_H + 70, "Hit", e->monitor_hit);
        monitor_line(&s[1], TOP_H + 94, "Press", e->monitor_press);
    }
    else monitor_line(&s[1], TOP_H + 70, "Event", e->monitor_event);
    knob_cells(&s[0], e, 0);
    knob_cells(&s[1], e, 1);
}

static void screen_shift(struct canvas s[2], const struct engine *e)
{
    /* Button5-8 */
    static const char *labels[] = {"MONITOR", "KNOBS", "PAGES", "TEMPLATES"};
    static const int screens[] = {SCREEN_MONITOR, SCREEN_KNOBS, SCREEN_PAGES, SCREEN_TEMPLATES};
    int i;

    header(&s[0], e, true);
    for (i = 0; i < 4; i++) top_cell(&s[1], i, labels[i], WHITE, e->screen == screens[i], true);
    text_center(&s[1], &small, 4, KNOB_LABEL_Y, CELL_W - 8, "Show", WHITE);
    text_center(&s[1], &medium, 4, KNOB_VALUE_Y, CELL_W - 8, e->t.knob_mode ? "BARS" : "VALUES", WHITE);
    text_center(&s[1], &small, CELL_W, TOP_H + 60, 3 * CELL_W, "SHIFT + LEFT/RIGHT: template", GREY);
    text_center(&s[1], &small, CELL_W, TOP_H + 84, 3 * CELL_W, "SHIFT + CHANNEL: leave MIDI mode", GREY);
}

static void screen_knobs(struct canvas s[2], const struct engine *e)
{
    header(&s[0], e, false);
    page_buttons(&s[0], e, 0);
    page_buttons(&s[1], e, 1);
    pads(&s[1], e);
    knob_cells(&s[0], e, 0);
    knob_cells(&s[1], e, 1);
}

static void screen_idle(struct canvas s[2])
{
    text_center(&s[0], &large, 0, 104, SCREEN_W, "MASCHINE MK3", WHITE);
    text_center(&s[0], &small, 0, 150, SCREEN_W, "ni-wine", GREY);
    text_center(&s[1], &medium, 0, 88, SCREEN_W, "Start MASCHINE or press", GREY);
    text_center(&s[1], &medium, 0, 122, SCREEN_W, "SHIFT + CHANNEL", WHITE);
    text_center(&s[1], &medium, 0, 156, SCREEN_W, "for MIDI mode", GREY);
}

void screen_draw(const struct engine *e, struct canvas s[2])
{
    fill(&s[0], 0, 0, SCREEN_W, SCREEN_H, BG);
    fill(&s[1], 0, 0, SCREEN_W, SCREEN_H, BG);
    if (!e->midi_mode || !engine_map(e))
    {
        screen_idle(s);
        return;
    }
    separators(&s[0]);
    separators(&s[1]);
    switch (engine_shown_screen(e))
    {
    case SCREEN_SHIFT:
        screen_shift(s, e);
        break;
    case SCREEN_TEMPLATES:
        screen_templates(s, e);
        break;
    case SCREEN_PAGES:
        screen_pages(s, e);
        break;
    case SCREEN_MONITOR:
        screen_monitor(s, e);
        break;
    default:
        screen_knobs(s, e);
        break;
    }
}

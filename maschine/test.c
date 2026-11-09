/* Engine tests on NI's default templates. The expected MIDI and LEDs are what
 * NIHostIntegrationAgent sends in MIDI mode with those templates. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "device.h"
#include "screen.h"
#include "xml.h"

/* MK3 buttons and LEDs */
#define BTN_SHIFT 0x06
#define BTN_GROUP_A 0x08
#define BTN_RIGHT 0x32
#define BTN_CHANNEL 0x38
#define BTN_LEFT 0x3b
#define BTN_BUTTON1 0x40
#define LED_BUTTON1 12
#define LED_SHIFT 44
#define LED_PADS 87
#define LED_STRIP 62

/* Mikro buttons and LEDs */
#define MIKRO_MASCHINE 0x00
#define MIKRO_GROUP 0x0e
#define MIKRO_PLAY 0x16
#define MIKRO_REC 0x17
#define MIKRO_SHIFT 0x19
#define MIKRO_PUSH 0x27
#define MIKRO_TOUCH 0x28
#define MIKRO_LED_PADS 0x27
#define MIKRO_LED_STRIP 0x37

static unsigned char out[64][16];
static int out_count, out_len[64];
static int failures;

static void midi(void *ctx, const unsigned char *data, int len)
{
    if (out_count < 64 && len <= (int)sizeof(out[0]))
    {
        memcpy(out[out_count], data, len);
        out_len[out_count++] = len;
    }
}

#define CHECK(cond) check(cond, __LINE__, #cond)

static void check(bool ok, int line, const char *what)
{
    if (ok) return;
    printf("%s:%d: %s\n", __FILE__, line, what);
    failures++;
}

/* the messages sent since the last call, as "b0 39 7f, ..." */
static const char *sent(void)
{
    static char buf[512];
    int i, j, n = 0;

    buf[0] = 0;
    for (i = 0; i < out_count; i++)
        for (j = 0; j < out_len[i]; j++)
            n += snprintf(buf + n, sizeof(buf) - n, "%s%02x", j ? " " : i ? ", " : "", out[i][j]);
    out_count = 0;
    return buf;
}

#define SENT(expected) check_sent(__LINE__, expected)

static void check_sent(int line, const char *expected)
{
    const char *got = sent();

    if (!strcmp(got, expected)) return;
    printf("%s:%d: sent \"%s\", expected \"%s\"\n", __FILE__, line, got, expected);
    failures++;
}

/* printf into a buffer of its own */
static const char *with(const char *fmt, ...)
{
    static char buf[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

/* what the agent sends leaving a pad page of NI's default: Pad1-16's notes off, in id order */
static const char *page_releases(int first_note)
{
    static const int order[] = {1, 10, 11, 12, 13, 14, 15, 16, 2, 3, 4, 5, 6, 7, 8, 9};
    static char buf[256];
    int i, n = 0;

    for (i = 0; i < PADS; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s90 %02x 00", i ? ", " : "", first_note + order[i] - 1);
    return buf;
}

#define HELLO "f0 00 21 09 16 00 4d 50 00 01 46 01 f7"

static void put16(unsigned char *p, int v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static void pad_entry(unsigned char *p, int pad, int kind, int value)
{
    p[0] = pad;
    p[1] = kind << 4 | value >> 8;
    p[2] = value & 0xff;
}

static void tap(struct engine *e, int bit)
{
    engine_button(e, bit, true);
    engine_button(e, bit, false);
}

static void setup_model(struct engine *e, const struct model *model)
{
    struct templates t;

    engine_init(e, model, midi, NULL);
    templates_default(&t, model);
    engine_set_templates(e, &t);
    engine_set_midi_mode(e, true);
    out_count = 0;
}

static void setup(struct engine *e)
{
    setup_model(e, &maschine_mk3);
}

/* the screen button ButtonN of the MK3 */
static int screen_button(int n)
{
    return maschine_mk3.screen_buttons[n - 1];
}

static void test_leds(void)
{
    /* report 0x80 as the agent wrote it, Play toggled off */
    static const char *agent = "7c7c7c7c7c7c7e7e7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c2a28"
                               "2828282828287c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c0c0c0c0c";
    unsigned char leds[MAX_LEDS];
    char hex[2 * LED_STRIP + 1];
    struct engine e;
    int i;

    setup(&e);
    engine_leds(&e, leds);
    for (i = 0; i < LED_STRIP; i++) sprintf(hex + 2 * i, "%02x", leds[i]);
    if (strcmp(hex, agent))
    {
        printf("leds  %s\nagent %s\n", hex, agent);
        failures++;
    }
    for (i = 0; i < PADS; i++) CHECK(leds[LED_PADS + i] == 0x0c);
    for (i = 0; i < STRIP_LEDS; i++) CHECK(leds[LED_STRIP + i] == 0);

    /* host turns Play and Button1 on */
    engine_midi_in(&e, (const unsigned char *)"\xb0\x39\x7f", 3);
    engine_midi_in(&e, (const unsigned char *)"\xb0\x16\x7f", 3);
    engine_leds(&e, leds);
    CHECK(leds[41] == 0x7e);
    CHECK(leds[12] == 0x7e);
    /* notes from the host don't light pads with onIfDown */
    engine_midi_in(&e, (const unsigned char *)"\x90\x0c\x7f", 3);
    engine_leds(&e, leds);
    CHECK(leds[LED_PADS + 12] == 0x0c);
    engine_pad(&e, 12, PAD_HIT_ON, 4095);
    engine_leds(&e, leds);
    CHECK(leds[LED_PADS + 12] == 0x0e);
    SENT("90 0c 7f");

    /* Shift */
    engine_button(&e, BTN_SHIFT, true);
    engine_leds(&e, leds);
    CHECK(leds[LED_SHIFT] == 0x7e);
    CHECK(leds[LED_BUTTON1] == 0);
    CHECK(leds[16] == 0x7e);
    engine_button(&e, BTN_SHIFT, false);

    engine_set_midi_mode(&e, false);
    engine_leds(&e, leds);
    for (i = 0; i < MAX_LEDS; i++) CHECK(!leds[i]);
    engine_free(&e);
}

static void test_buttons(void)
{
    struct engine e;

    setup(&e);
    tap(&e, 0x2d);                          /* Play, toggle */
    SENT("b0 39 7f");
    tap(&e, 0x2d);
    SENT("b0 39 00");
    engine_button(&e, 0x02, true);          /* EncoderUp, gate */
    SENT("b0 1e 7f");
    engine_button(&e, 0x02, false);
    SENT("b0 1e 00");
    tap(&e, BTN_BUTTON1);
    SENT("b0 16 7f");
    tap(&e, 0x47);                          /* EncoderCap, disabled */
    SENT("");
    engine_button(&e, 0x16, true);          /* FSwTip */
    engine_button(&e, 0x16, false);
    SENT("b0 40 7f, b0 40 00");

    /* gate held across Shift still releases */
    engine_button(&e, 0x02, true);
    engine_button(&e, BTN_SHIFT, true);
    engine_button(&e, 0x02, false);
    engine_button(&e, BTN_SHIFT, false);
    SENT("b0 1e 7f, b0 1e 00");

    /* Shift swallows buttons */
    engine_button(&e, BTN_SHIFT, true);
    tap(&e, 0x2d);
    engine_button(&e, BTN_SHIFT, false);
    SENT("");
    engine_free(&e);
}

static void test_navigation(void)
{
    struct engine e;

    setup(&e);
    engine_knob(&e, 1, 500);
    SENT("b0 46 40");
    tap(&e, BTN_RIGHT);
    SENT("");
    CHECK(engine_map(&e)->page == 1);
    engine_knob(&e, 1, 1000);
    SENT("b0 6c 7f");
    tap(&e, BTN_BUTTON1);
    SENT("b0 3c 7f");
    tap(&e, BTN_RIGHT);                     /* wraps around */
    CHECK(engine_map(&e)->page == 0);
    tap(&e, BTN_LEFT);
    CHECK(engine_map(&e)->page == 1);

    tap(&e, BTN_GROUP_A + 1);
    SENT(page_releases(12));
    engine_pad(&e, 12, PAD_HIT_ON, 4095);
    engine_pad(&e, 12, PAD_HIT_OFF, 0);
    SENT("90 18 7f, 90 18 00");
    engine_pad(&e, 3, PAD_HIT_ON, 2048);    /* Pad16 */
    SENT("90 27 40");
    engine_pad(&e, 3, PAD_HIT_OFF, 0);
    SENT("90 27 00");

    /* MIDI mode */
    engine_button(&e, BTN_SHIFT, true);
    tap(&e, BTN_CHANNEL);
    engine_button(&e, BTN_SHIFT, false);
    CHECK(!e.midi_mode);
    tap(&e, 0x2d);
    engine_knob(&e, 1, 100);
    SENT("");
    engine_button(&e, BTN_SHIFT, true);
    tap(&e, BTN_CHANNEL);
    engine_button(&e, BTN_SHIFT, false);
    CHECK(e.midi_mode);
    SENT(HELLO);

    /* screens */
    engine_button(&e, BTN_SHIFT, true);
    CHECK(engine_shown_screen(&e) == SCREEN_SHIFT);
    tap(&e, screen_button(7));
    engine_button(&e, BTN_SHIFT, false);
    CHECK(engine_shown_screen(&e) == SCREEN_PAGES);
    engine_knob(&e, 3, 500);
    tap(&e, screen_button(4));
    CHECK(engine_map(&e)->group == 5);
    SENT(page_releases(24));
    engine_button(&e, BTN_SHIFT, true);
    tap(&e, screen_button(6));
    engine_button(&e, BTN_SHIFT, false);
    CHECK(engine_shown_screen(&e) == SCREEN_KNOBS);
    engine_free(&e);
}

static void test_pads(void)
{
    struct engine e;
    struct map *m;

    setup(&e);
    engine_pad(&e, 12, PAD_HIT_ON, 1);
    SENT("90 0c 00");
    engine_pad(&e, 12, PAD_PRESSURE, 2048);
    engine_pad(&e, 12, PAD_PRESSURE, 2048);
    engine_pad(&e, 12, PAD_PRESSURE, 4095);
    SENT("a0 0c 40, a0 0c 7f");
    engine_pad(&e, 12, PAD_HIT_OFF, 0);
    SENT("90 0c 00");
    m = engine_map(&e);
    m->velocity_curve = 7;
    engine_pad(&e, 0, PAD_HIT_ON, 100);     /* Pad13 */
    SENT("90 18 7f");
    m->velocity_curve = 0;
    engine_pad(&e, 0, PAD_HIT_OFF, 0);
    engine_pad(&e, 0, PAD_HIT_ON, 2048);    /* 1 - (0.9/8 + 0.05) = 0.8375 */
    SENT("90 18 00, 90 18 6a");
    engine_free(&e);
}

static void test_wheel_strip(void)
{
    struct engine e;

    setup(&e);
    engine_encoder(&e, 3);
    SENT("b0 08 03");
    engine_encoder(&e, -5);
    SENT("b0 08 00");
    engine_strip(&e, 512);
    SENT("b0 01 3f");
    engine_strip(&e, 1024);
    SENT("b0 01 7f");
    engine_strip(&e, 0);
    SENT("");
    engine_pedal(&e, 4095);
    SENT("b0 0b 7f");
    engine_free(&e);
}

static void test_xml(void)
{
    struct xml *x = xml_parse("<?xml version=\"1.0\"?><!-- c --><a x='1 &amp; 2'><b>t&lt;&#65;</b><c/>"
                              "<d y=\"&quot;\"> <![CDATA[<raw>]]> </d></a>");
    CHECK(x && !strcmp(x->name, "a"));
    CHECK(x && !strcmp(xml_attr(x, "x"), "1 & 2"));
    CHECK(x && !strcmp(xml_text(x, "b"), "t<A"));
    CHECK(x && xml_child(x, "c") && !strcmp(xml_text(x, "c"), ""));
    CHECK(x && !strcmp(xml_text(x, "d"), "<raw>"));
    CHECK(x && !strcmp(xml_attr(xml_child(x, "d"), "y"), "\""));
    xml_free(x);
    CHECK(!xml_parse("<a><b></a"));
}

static void test_reports(void)
{
    unsigned char r[42] = {0x01}, pads[64] = {0x02};
    struct device dev = {.model = &maschine_mk3, .hid = -1, .usbfs = -1};
    struct engine e;

    setup(&e);
    put16(r + 12, 990);
    device_report(&dev, &e, r, sizeof(r));                 /* positions only */
    SENT("");
    r[1 + (0x2d >> 3)] |= 1 << (0x2d & 7);              /* Play */
    device_report(&dev, &e, r, sizeof(r));
    SENT("b0 39 7f");
    r[1 + (0x2d >> 3)] = 0;
    r[11] = 3;                                          /* encoder +3 */
    put16(r + 12, 10);                                  /* knob 1 990 -> 10: +20 */
    device_report(&dev, &e, r, sizeof(r));
    SENT("b0 08 03, b0 46 03");
    r[11] = 14;                                         /* 3 -> 14: -5 */
    put16(r + 30, 512);                                 /* touch strip */
    device_report(&dev, &e, r, sizeof(r));
    SENT("b0 08 00, b0 01 3f");
    pad_entry(pads + 1, 12, PAD_HIT_ON, 4095);          /* Pad1 */
    pad_entry(pads + 4, 12, PAD_PRESSURE, 2048);
    device_report(&dev, &e, pads, sizeof(pads));
    SENT("90 0c 7f, a0 0c 40");
    memset(pads + 1, 0, sizeof(pads) - 1);
    pad_entry(pads + 1, 12, PAD_HIT_OFF, 0);
    device_report(&dev, &e, pads, sizeof(pads));
    SENT("90 0c 00");
    engine_free(&e);
}

/* a region being filled from a display frame */
struct region
{
    struct canvas *c;
    int x, y, w, h, pos;
};

static int be16(const unsigned char *p)
{
    return p[0] << 8 | p[1];
}

static void put_pixel(struct region *r, int v)
{
    if (r->pos < r->w * r->h) r->c->px[r->y + r->pos / r->w][r->x + r->pos % r->w] = v;
    r->pos++;
}

/* applies a display frame like the MK3 does */
static int decode(struct canvas *c, const unsigned char *f, int len)
{
    struct region r = {c, be16(f + 8), be16(f + 10), be16(f + 12), be16(f + 14), 0};
    int off = 16, cmd, n, i;

    if (f[0] != 0x84) return -1;
    while (off + 4 <= len)
    {
        cmd = f[off];
        n = f[off + 1] << 16 | be16(f + off + 2);
        off += 4;
        if (cmd == 0x00)
        {
            for (i = 0; i < 2 * n; i++) put_pixel(&r, be16(f + off + 2 * i));
            off += 4 * n;
        }
        else if (cmd == 0x01)
        {
            for (i = 0; i < n; i++)
            {
                put_pixel(&r, be16(f + off));
                put_pixel(&r, be16(f + off + 2));
            }
            off += 4;
        }
        else if (cmd == 0x40) return off == len && r.pos == r.w * r.h ? 0 : -1;
        else if (cmd != 0x03) return -1;
    }
    return -1;
}

static void test_frames(void)
{
    static struct canvas s[2], out;
    static unsigned char frame[16 + 4 * SCREEN_W * SCREEN_H + 64];
    struct engine e;
    int len;

    setup(&e);
    engine_knob(&e, 1, 400);
    engine_pad(&e, 12, PAD_HIT_ON, 3000);
    screen_draw(&e, s);
    memset(&out, 0, sizeof(out));
    len = device_encode(frame, 1, &s[1], 0, 0, SCREEN_W, SCREEN_H);
    CHECK(frame[2] == 1);
    CHECK(len < SCREEN_W * SCREEN_H);                   /* the background repeats */
    CHECK(!decode(&out, frame, len));
    CHECK(!memcmp(&out, &s[1], sizeof(out)));
    /* a region */
    memset(&out, 0, sizeof(out));
    len = device_encode(frame, 0, &s[0], 118, 30, 64, 100);
    CHECK(!decode(&out, frame, len));
    CHECK(!memcmp(out.px[30] + 118, s[0].px[30] + 118, 64 * 2));
    CHECK(!memcmp(out.px[129] + 118, s[0].px[129] + 118, 64 * 2));
    CHECK(out.px[30][117] == 0 && out.px[130][118] == 0);
    engine_free(&e);
}

/* releases go where the press went, whatever changed in between */
static void test_held(void)
{
    struct templates t;
    struct control *c;
    struct engine e;

    setup(&e);
    /* leaving a page releases what it holds, the later releases send nothing */
    engine_pad(&e, 12, PAD_HIT_ON, 4095);   /* Pad1, page A */
    tap(&e, BTN_GROUP_A + 1);
    engine_pad(&e, 12, PAD_HIT_OFF, 0);
    SENT(with("90 0c 7f, %s", page_releases(12)));
    tap(&e, BTN_GROUP_A);
    SENT(page_releases(24));

    c = engine_find(&e, "Button1");
    c->behavior = BEHAVIOR_GATE;
    engine_button(&e, BTN_BUTTON1, true);
    tap(&e, BTN_RIGHT);
    engine_button(&e, BTN_BUTTON1, false);
    SENT("b0 16 7f, b0 16 00");
    tap(&e, BTN_LEFT);

    /* leaving MIDI mode, reloading and losing the MK3 release everything */
    engine_pad(&e, 12, PAD_HIT_ON, 4095);
    engine_button(&e, 0x02, true);
    engine_button(&e, BTN_SHIFT, true);
    tap(&e, BTN_CHANNEL);
    engine_button(&e, BTN_SHIFT, false);
    SENT("90 0c 7f, b0 1e 7f, b0 1e 00, 90 0c 00");
    engine_pad(&e, 12, PAD_HIT_OFF, 0);
    engine_button(&e, 0x02, false);
    SENT("");
    engine_set_midi_mode(&e, true);
    engine_pad(&e, 12, PAD_HIT_ON, 4095);
    templates_default(&t, &maschine_mk3);
    engine_set_templates(&e, &t);
    SENT("90 0c 7f, 90 0c 00");
    engine_pad(&e, 12, PAD_HIT_ON, 4095);
    engine_button(&e, BTN_SHIFT, true);
    engine_reset_input(&e);
    CHECK(!e.shift);
    SENT("90 0c 7f, 90 0c 00");
    engine_free(&e);
}

static void test_increment(void)
{
    struct engine e;
    struct control *c;

    setup(&e);
    c = engine_find(&e, "Play");
    c->behavior = BEHAVIOR_INCREMENT;
    c->min = 1;
    c->max = 0;
    c->wrap = true;
    c->last = 0;
    tap(&e, 0x2d);
    tap(&e, 0x2d);
    SENT("b0 39 01, b0 39 00");
    c->min = 0;
    c->max = 2;
    c->wrap = false;
    c->inc = 5;
    tap(&e, 0x2d);
    SENT("b0 39 02");
    engine_free(&e);
}

/* the agent's messages to the host and its strip patterns */
static void test_agent_details(void)
{
    unsigned char leds[MAX_LEDS];
    struct control *strip;
    struct engine e;
    struct map *map;
    int i;

    setup(&e);
    map = engine_map(&e);
    engine_connected(&e);
    SENT(HELLO);
    engine_set_midi_mode(&e, false);
    engine_connected(&e);
    SENT("");
    engine_set_midi_mode(&e, true);

    /* SHIFT goes to the host only without navigation */
    engine_button(&e, BTN_SHIFT, true);
    engine_button(&e, BTN_SHIFT, false);
    SENT("");
    map->knob_pages = false;
    map->pad_pages = false;
    engine_button(&e, BTN_SHIFT, true);
    engine_button(&e, BTN_SHIFT, false);
    SENT("f0 00 21 09 16 00 4d 50 00 01 4d 01 f7, f0 00 21 09 16 00 4d 50 00 01 4d 00 f7");
    map->knob_pages = true;
    map->pad_pages = true;

    /* pressure that isn't at 0 is released with the page */
    engine_pad(&e, 12, PAD_HIT_ON, 4095);
    engine_pad(&e, 12, PAD_PRESSURE, 2048);
    engine_pad(&e, 12, PAD_HIT_OFF, 0);
    SENT("90 0c 7f, a0 0c 40, 90 0c 00");
    tap(&e, BTN_GROUP_A + 1);
    SENT(with("%s, a0 0c 00", page_releases(12)));

    /* centre pattern: the centre LED and those towards the value */
    strip = engine_find(&e, "Touchstrip");
    strip->ledpattern = STRIP_CENTER;
    strip->pos = 0.39f;
    engine_leds(&e, leds);
    for (i = 0; i < STRIP_LEDS; i++) CHECK(!leds[LED_STRIP + i] == !(i >= 10 && i <= 12));
    strip->pos = 0.53f;
    engine_leds(&e, leds);
    for (i = 0; i < STRIP_LEDS; i++) CHECK(!leds[LED_STRIP + i] == (i != 12));
    engine_free(&e);
}

/* the Mikro MK3 */

static void mikro_controls(struct device *dev, struct engine *e, unsigned long long buttons, int encoder,
                           int strip)
{
    unsigned char r[14] = {0x01};
    int i;

    for (i = 0; i < 6; i++) r[1 + i] = buttons >> (8 * i);
    r[7] = encoder;
    put16(r + 10, strip);
    device_report(dev, e, r, sizeof(r));
}

static void test_mikro_mode(void)
{
    struct device dev = {.model = &maschine_mikro_mk3, .hid = -1, .usbfs = -1};
    unsigned char leds[MAX_LEDS];
    struct engine e;
    int i;

    setup_model(&e, &maschine_mikro_mk3);
    engine_set_midi_mode(&e, false);
    mikro_controls(&dev, &e, 0, 0, 0);
    mikro_controls(&dev, &e, 1ull << MIKRO_SHIFT, 0, 0);
    mikro_controls(&dev, &e, 1ull << MIKRO_SHIFT | 1ull << MIKRO_MASCHINE, 0, 0);
    CHECK(e.midi_mode);
    SENT("f0 00 21 09 17 00 4d 50 00 01 46 01 f7");
    mikro_controls(&dev, &e, 0, 0, 0);
    SENT("");
    engine_leds(&e, leds);
    for (i = 0; i < 0x27; i++) CHECK(leds[i] == (i == 0x08 || i == 0x09 ? 0 : 0x7c));
    for (i = 0; i < PADS; i++) CHECK(leds[MIKRO_LED_PADS + i] == 0x0c);
    engine_free(&e);
}

static void test_mikro_buttons(void)
{
    unsigned char leds[MAX_LEDS];
    struct engine e;

    setup_model(&e, &maschine_mikro_mk3);
    tap(&e, MIKRO_MASCHINE);
    SENT("b0 26 7f");
    tap(&e, MIKRO_PLAY);
    SENT("b0 39 7f");
    engine_leds(&e, leds);
    CHECK(leds[MIKRO_MASCHINE] == 0x7e && leds[MIKRO_PLAY] == 0x7e);
    tap(&e, 0x08);                          /* unnamed */
    tap(&e, MIKRO_PUSH);
    tap(&e, MIKRO_TOUCH);                   /* disabled */
    SENT("b0 08 7f");
    /* SHIFT keeps the buttons quiet, not the pads and the encoder */
    engine_button(&e, MIKRO_SHIFT, true);
    tap(&e, MIKRO_PLAY);
    engine_pad(&e, 0, PAD_HIT_ON, 4095);
    engine_pad(&e, 0, PAD_HIT_OFF, 0);
    engine_encoder(&e, 2);
    engine_button(&e, MIKRO_SHIFT, false);
    SENT("90 18 7f, 90 18 00, b0 07 02");
    /* the host turns Rec off */
    tap(&e, MIKRO_REC);
    engine_midi_in(&e, (const unsigned char *)"\xb0\x3a\x00", 3);
    engine_leds(&e, leds);
    CHECK(leds[MIKRO_REC] == 0x7c);
    engine_free(&e);
}

static void test_mikro_pad_pages(void)
{
    unsigned char leds[MAX_LEDS];
    struct engine e;
    int i;

    setup_model(&e, &maschine_mikro_mk3);
    engine_button(&e, MIKRO_GROUP, true);
    SENT("");
    engine_leds(&e, leds);
    CHECK(leds[MIKRO_LED_PADS] == 0x06 && leds[MIKRO_LED_PADS + 1] == 0x04 && leds[MIKRO_LED_PADS + 8] == 0);
    CHECK(leds[MIKRO_GROUP] == 0x7c);
    engine_pad(&e, 1, PAD_HIT_ON, 4095);
    engine_pad(&e, 1, PAD_HIT_OFF, 0);
    SENT(page_releases(12));
    engine_pad(&e, 12, PAD_HIT_ON, 4095);  /* no page 13 */
    SENT("");
    engine_button(&e, MIKRO_GROUP, false);
    engine_leds(&e, leds);
    for (i = 0; i < PADS; i++) CHECK(leds[MIKRO_LED_PADS + i] == 0x0c);
    engine_pad(&e, 12, PAD_HIT_ON, 4095);
    SENT("90 18 7f");
    engine_free(&e);
}

static void test_mikro_strip(void)
{
    struct device dev = {.model = &maschine_mikro_mk3, .hid = -1, .usbfs = -1};
    unsigned char leds[MAX_LEDS];
    struct engine e;
    int i;

    setup_model(&e, &maschine_mikro_mk3);
    mikro_controls(&dev, &e, 0, 0, 0);
    mikro_controls(&dev, &e, 0, 1, 1);
    SENT("b0 07 01");
    mikro_controls(&dev, &e, 0, 1, 100);
    SENT("b0 01 3f");
    engine_leds(&e, leds);
    for (i = 0; i < STRIP_LEDS; i++) CHECK((leds[MIKRO_LED_STRIP + i] == 0x0e) == (i < 12));
    mikro_controls(&dev, &e, 0, 1, 200);
    mikro_controls(&dev, &e, 0, 1, 250);
    mikro_controls(&dev, &e, 0, 1, 50);
    mikro_controls(&dev, &e, 0, 1, 0);
    SENT("b0 01 7f, b0 01 1f");
    engine_free(&e);
}

static void test_mikro_display(void)
{
    static struct mono_canvas c;
    unsigned char report[265];
    struct engine e;

    setup_model(&e, &maschine_mikro_mk3);
    screen_draw_mono(&e, &c);
    CHECK(c.px[12][MONO_W / 2] || c.px[16][MONO_W / 2 + 4]);
    CHECK(!c.px[0][0] && !c.px[31][127]);
    CHECK(device_encode_mono(report, &c, 0, 2, MONO_W, 2) == 265);
    CHECK(!memcmp(report, "\xe0\x00\x00\x02\x00\x80\x00\x02\x00", 9));
    CHECK(report[9] == 0xff);               /* unlit pixels are 1 */
    engine_set_midi_mode(&e, false);
    screen_draw_mono(&e, &c);
    device_encode_mono(report, &c, 0, 0, MONO_W, 2);
    CHECK(report[9 + 64] == 0xff && report[9 + 255] == 0xff);
    engine_free(&e);
}

/* a template file, if given, must load and match NI's default (the agent
 * writes the default when there is none) */
static void test_file(const char *path)
{
    struct templates t, d;
    struct control *a, *b;
    int i, j;

    if (templates_load(&t, path))
    {
        printf("%s: can't load\n", path);
        failures++;
        return;
    }
    templates_default(&d, &maschine_mk3);
    CHECK(t.count == 1 && t.maps[0].page_count == 2 && t.maps[0].group_count == 8);
    CHECK(t.maps[0].controls.count == d.maps[0].controls.count);
    for (i = 0; i < d.maps[0].controls.count; i++)
    {
        b = &d.maps[0].controls.items[i];
        if (!(a = controls_find(&t.maps[0].controls, b->id))) continue;
        if (a->message != b->message || a->number != b->number || a->behavior != b->behavior
                || a->enabled != b->enabled)
        {
            printf("%s differs: message %x/%x number %d/%d behavior %d/%d enabled %d/%d\n", b->id, a->message,
                   b->message, a->number, b->number, a->behavior, b->behavior, a->enabled, b->enabled);
            failures++;
        }
    }
    for (j = 0; j < 8; j++)
        for (i = 0; i < d.maps[0].groups[j].controls.count; i++)
        {
            b = &d.maps[0].groups[j].controls.items[i];
            a = controls_find(&t.maps[0].groups[j].controls, b->id);
            if (b->type == CONTROL_LED || (a && a->number == b->number && a->message == b->message)) continue;
            printf("group %d %s differs\n", j, b->id);
            failures++;
        }
    templates_free(&t);
    templates_free(&d);
}

int main(int argc, char **argv)
{
    test_xml();
    test_leds();
    test_buttons();
    test_navigation();
    test_pads();
    test_wheel_strip();
    test_reports();
    test_frames();
    test_held();
    test_increment();
    test_agent_details();
    test_mikro_mode();
    test_mikro_buttons();
    test_mikro_pad_pages();
    test_mikro_strip();
    test_mikro_display();
    if (argc > 1) test_file(argv[1]);
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/usbdevice_fs.h>
#include "mk3.h"
#include "util.h"

#define HID_ID "HID_ID=0003:000017CC:00001600"
#define DISPLAY_INTERFACE 5
#define DISPLAY_ENDPOINT 0x04
/* a multiple of the endpoint's 512-byte packets, so the device sees one transfer */
#define BULK_CHUNK 16384
#define BULK_TIMEOUT_MS 1000

/* input report 0x01 */
#define REPORT_CONTROLS 0x01
#define CONTROLS_BUTTONS 1          /* MK3_BUTTONS bits */
#define CONTROLS_ENCODER 11         /* low nibble, wraps around */
#define CONTROLS_KNOBS 12           /* u16 each, 0-999, wraps around */
#define CONTROLS_STRIP 30           /* u16 first finger, u16 second finger */
#define CONTROLS_PEDAL 34           /* u16 */
#define CONTROLS_SIZE 36
#define ENCODER_POSITIONS 16

/* input report 0x02: up to 21 entries {pad, kind << 4 | value >> 8, value & 0xff}, then zeros */
#define REPORT_PADS 0x02
#define PAD_ENTRY 3
#define PADS_SIZE 64

/* output reports: LEDs below MK3_LED_STRIP, the others */
#define REPORT_LEDS 0x80
#define REPORT_LEDS_2 0x81

/* display frames: header, commands, RGB565 big endian */
#define FRAME_HEADER 16
#define CMD_PIXELS 0x00             /* n pixel pairs follow */
#define CMD_REPEAT 0x01             /* the next pixel pair n times */
#define CMD_BLIT 0x03
#define CMD_END 0x40
#define MIN_REPEAT 3

static int read_file(const char *path, char *buf, int size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC), n;

    if (fd < 0) return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

static int read_number(const char *dir, const char *name)
{
    char path[PATH_MAX], buf[32];

    if (!FORMAT(path, "%s/%s", dir, name) || read_file(path, buf, sizeof(buf)) < 0) return -1;
    return atoi(buf);
}

/* /dev/bus/usb/BBB/DDD of the USB device a hidraw node belongs to */
static int usb_node(const char *hidraw, char *buf, int size)
{
    char path[PATH_MAX], real[PATH_MAX], *slash;
    int i, bus, num;

    if (!FORMAT(path, "/sys/class/hidraw/%s/device", hidraw) || !realpath(path, real)) return -1;
    /* .../1-3/1-3:1.4/0003:17CC:1600.000B -> .../1-3 */
    for (i = 0; i < 2; i++)
    {
        if (!(slash = strrchr(real, '/'))) return -1;
        *slash = 0;
    }
    if (strncmp(real, "/sys/", 5)) return -1;
    if ((bus = read_number(real, "busnum")) <= 0 || (num = read_number(real, "devnum")) <= 0) return -1;
    return snprintf(buf, size, "/dev/bus/usb/%03d/%03d", bus, num) < size ? 0 : -1;
}

int mk3_find(struct mk3 *dev)
{
    char path[PATH_MAX], uevent[1024];
    struct dirent *ent;
    int found = -1;
    DIR *dir;

    if (!(dir = opendir("/sys/class/hidraw"))) return -1;
    while (found && (ent = readdir(dir)))
    {
        if (ent->d_name[0] == '.') continue;
        if (!FORMAT(path, "/sys/class/hidraw/%s/device/uevent", ent->d_name)) continue;
        if (read_file(path, uevent, sizeof(uevent)) < 0 || !strstr(uevent, HID_ID)) continue;
        if (usb_node(ent->d_name, dev->usb, sizeof(dev->usb))) continue;
        if (FORMAT(dev->hidraw, "/dev/%s", ent->d_name)) found = 0;
    }
    closedir(dir);
    return found;
}

int mk3_open(struct mk3 *dev)
{
    unsigned int iface = DISPLAY_INTERFACE;
    int err;

    if (dev->usbfs < 0 && (dev->usbfs = open(dev->usb, O_RDWR | O_CLOEXEC)) < 0) goto fail;
    if (ioctl(dev->usbfs, USBDEVFS_CLAIMINTERFACE, &iface) < 0)
    {
        /* the node stays open, closing it would look like the other program's close */
        if (errno == EBUSY) return -EBUSY;
        goto fail;
    }
    if ((dev->hid = open(dev->hidraw, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) goto fail;
    dev->seen = false;
    memset(dev->buttons, 0, sizeof(dev->buttons));
    dev->strip = 0;
    dev->leds_sent = false;
    dev->screens_sent = false;
    return 0;

fail:
    err = errno;
    mk3_close(dev);
    return -err;
}

void mk3_close(struct mk3 *dev)
{
    if (dev->hid >= 0) close(dev->hid);
    /* closing releases the claimed interface */
    if (dev->usbfs >= 0) close(dev->usbfs);
    dev->hid = -1;
    dev->usbfs = -1;
}

static int u16(const unsigned char *p)
{
    return p[0] | p[1] << 8;
}

static bool bit_set(const unsigned char *bits, int i)
{
    return bits[i / 8] >> (i % 8) & 1;
}

/* the shorter way between two readings of a position that wraps around */
static int shortest(int delta, int period)
{
    if (delta > period / 2) return delta - period;
    if (delta < -period / 2) return delta + period;
    return delta;
}

/* buttons, encoder, knobs, touch strip, pedal; the first report after opening
 * only sets where the encoder, knobs and pedal are */
static void controls(struct mk3 *dev, struct engine *e, const unsigned char *r)
{
    const unsigned char *buttons = r + CONTROLS_BUTTONS;
    int i, v, delta, first, second;

    for (i = 0; i < MK3_BUTTONS; i++)
        if (bit_set(buttons, i) != bit_set(dev->buttons, i)) engine_button(e, i, bit_set(buttons, i));
    memcpy(dev->buttons, buttons, sizeof(dev->buttons));

    v = r[CONTROLS_ENCODER] & (ENCODER_POSITIONS - 1);
    if (dev->seen && (delta = shortest(v - dev->encoder, ENCODER_POSITIONS))) engine_encoder(e, delta);
    dev->encoder = v;

    for (i = 0; i < MK3_KNOBS; i++)
    {
        v = u16(r + CONTROLS_KNOBS + 2 * i);
        if (dev->seen && (delta = shortest(v - dev->knobs[i], MK3_KNOB_COUNTS))) engine_knob(e, i + 1, delta);
        dev->knobs[i] = v;
    }

    /* with two fingers the first one counts */
    first = u16(r + CONTROLS_STRIP);
    second = u16(r + CONTROLS_STRIP + 2);
    v = first ? first : second;
    if (v != dev->strip) engine_strip(e, v);
    dev->strip = v;

    v = clamp(u16(r + CONTROLS_PEDAL), 0, MK3_PEDAL_MAX);
    if (dev->seen && v != dev->pedal) engine_pedal(e, v);
    dev->pedal = v;
    dev->seen = true;
}

static void pads(struct engine *e, const unsigned char *r, int len)
{
    const unsigned char *entry;
    int off;

    for (off = 1; off + PAD_ENTRY <= len && off < PADS_SIZE; off += PAD_ENTRY)
    {
        entry = r + off;
        if (!entry[0] && !entry[1] && !entry[2]) break;
        engine_pad(e, entry[0], entry[1] >> 4, (entry[1] & 15) << 8 | entry[2]);
    }
}

void mk3_report(struct mk3 *dev, struct engine *e, const unsigned char *r, int len)
{
    if (r[0] == REPORT_CONTROLS && len >= CONTROLS_SIZE) controls(dev, e, r);
    else if (r[0] == REPORT_PADS) pads(e, r, len);
}

int mk3_read(struct mk3 *dev, struct engine *e)
{
    unsigned char r[128];
    ssize_t n;

    while ((n = read(dev->hid, r, sizeof(r))) > 0) mk3_report(dev, e, r, n);
    return n < 0 && errno != EAGAIN && errno != EINTR ? -1 : 0;
}

static int write_leds(struct mk3 *dev, int report, const unsigned char *leds, int count)
{
    unsigned char r[1 + MK3_LEDS];

    r[0] = report;
    memcpy(r + 1, leds, count);
    return write(dev->hid, r, 1 + count) < 0 ? -1 : 0;
}

int mk3_leds(struct mk3 *dev, const unsigned char leds[MK3_LEDS])
{
    const int second = MK3_LEDS - MK3_LED_STRIP;

    if (!dev->leds_sent || memcmp(leds, dev->leds, MK3_LED_STRIP))
        if (write_leds(dev, REPORT_LEDS, leds, MK3_LED_STRIP)) return -1;
    if (!dev->leds_sent || memcmp(leds + MK3_LED_STRIP, dev->leds + MK3_LED_STRIP, second))
        if (write_leds(dev, REPORT_LEDS_2, leds + MK3_LED_STRIP, second)) return -1;
    memcpy(dev->leds, leds, MK3_LEDS);
    dev->leds_sent = true;
    return 0;
}

static unsigned char *put16(unsigned char *p, int v)
{
    p[0] = v >> 8;
    p[1] = v;
    return p + 2;
}

static unsigned char *put_command(unsigned char *p, int cmd, int n)
{
    p[0] = cmd;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
    return p + 4;
}

static unsigned char *put_pair(unsigned char *p, const unsigned short *pair)
{
    return put16(put16(p, pair[0]), pair[1]);
}

static bool same_pair(const unsigned short *a, const unsigned short *b)
{
    return a[0] == b[0] && a[1] == b[1];
}

int mk3_encode(unsigned char *out, int screen, const struct canvas *c, int x, int y, int w, int h)
{
    static unsigned short px[SCREEN_W * SCREEN_H];
    unsigned char *p = out, *literal = NULL;
    int i, j, n = 0, pairs, pair, run, literal_pairs = 0;

    *p++ = 0x84;
    *p++ = 0;
    *p++ = screen;
    *p++ = 0x60;
    memset(p, 0, 4);
    p = put16(put16(put16(put16(p + 4, x), y), w), h);

    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) px[n++] = c->px[j][i];
    pairs = n / 2;
    for (pair = 0; pair < pairs; pair += run)
    {
        for (run = 1; pair + run < pairs && same_pair(px + 2 * (pair + run), px + 2 * pair); run++);
        if (run >= MIN_REPEAT)
        {
            if (literal) put_command(literal, CMD_PIXELS, literal_pairs);
            literal = NULL;
            p = put_pair(put_command(p, CMD_REPEAT, run), px + 2 * pair);
            continue;
        }
        run = 1;
        /* a CMD_PIXELS header, filled in when the run of literals ends */
        if (!literal)
        {
            literal = p;
            p += 4;
            literal_pairs = 0;
        }
        p = put_pair(p, px + 2 * pair);
        literal_pairs++;
    }
    if (literal) put_command(literal, CMD_PIXELS, literal_pairs);
    p = put_command(p, CMD_BLIT, 0);
    p = put_command(p, CMD_END, 0);
    return p - out;
}

static int bulk(int fd, unsigned char *data, int len)
{
    struct usbdevfs_bulktransfer t;
    int off, n;

    for (off = 0; off < len; off += n)
    {
        n = len - off < BULK_CHUNK ? len - off : BULK_CHUNK;
        t.ep = DISPLAY_ENDPOINT;
        t.len = n;
        t.timeout = BULK_TIMEOUT_MS;
        t.data = data + off;
        if (ioctl(fd, USBDEVFS_BULK, &t) < 0) return -1;
    }
    return 0;
}

/* the rows and columns that differ from what was sent, false if nothing does */
static bool dirty_region(const struct mk3 *dev, const struct canvas *c, int screen,
                         int *left, int *top, int *right, int *bottom)
{
    const struct canvas *sent = &dev->sent[screen];
    int x, y;

    *left = SCREEN_W;
    *right = -1;
    *top = SCREEN_H;
    *bottom = -1;
    for (y = 0; y < SCREEN_H; y++)
    {
        if (dev->screens_sent && !memcmp(c->px[y], sent->px[y], sizeof(c->px[y]))) continue;
        for (x = 0; x < SCREEN_W; x++)
            if (!dev->screens_sent || c->px[y][x] != sent->px[y][x])
            {
                if (x < *left) *left = x;
                if (x > *right) *right = x;
            }
        if (y < *top) *top = y;
        *bottom = y;
    }
    return *bottom >= 0;
}

int mk3_screens(struct mk3 *dev, const struct canvas screens[2])
{
    static unsigned char frame[FRAME_HEADER + 4 * SCREEN_W * SCREEN_H + 64];
    int i, len, left, top, right, bottom;

    for (i = 0; i < 2; i++)
    {
        if (!dirty_region(dev, &screens[i], i, &left, &top, &right, &bottom)) continue;
        /* pixels go in pairs */
        left &= ~1;
        right |= 1;
        len = mk3_encode(frame, i, &screens[i], left, top, right - left + 1, bottom - top + 1);
        if (bulk(dev->usbfs, frame, len)) return -1;
    }
    memcpy(dev->sent, screens, sizeof(dev->sent));
    dev->screens_sent = true;
    return 0;
}

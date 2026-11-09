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
#include "device.h"
#include "util.h"

/* the MK3's display interface */
#define DISPLAY_INTERFACE 5
#define DISPLAY_ENDPOINT 0x04
/* a multiple of the endpoint's 512-byte packets, so the device sees one transfer */
#define BULK_CHUNK 16384
#define BULK_TIMEOUT_MS 1000

/* input report 0x01, the layout in the model */
#define REPORT_CONTROLS 0x01
#define ENCODER_POSITIONS 16

/* input report 0x02: up to 21 entries {pad, kind << 4 | value >> 8, value & 0xff}, then zeros */
#define REPORT_PADS 0x02
#define PAD_ENTRY 3
#define PADS_SIZE 64

/* output reports: the LEDs (the MK3's below its strip, then 0x81 the others) */
#define REPORT_LEDS 0x80
#define REPORT_LEDS_2 0x81

/* the Mikro's display: E0, x, y, w, h (u16 little endian, y and h in 8-row pages),
 * then up to 256 bytes, page by page, a column per byte, bit k = row k, 1 = unlit */
#define REPORT_MONO 0xe0
#define MONO_HEADER 9
#define MONO_DATA 256
#define MONO_REPORT (MONO_HEADER + MONO_DATA)
#define MONO_PAGES (MONO_H / 8)

/* the MK3's display frames: header, commands, RGB565 big endian */
#define FRAME_HEADER 16
#define CMD_PIXELS 0x00             /* n pixel pairs follow */
#define CMD_REPEAT 0x01             /* the next pixel pair n times */
#define CMD_BLIT 0x03
#define CMD_END 0x40
#define MIN_REPEAT 3

static int read_file(const char *path, char *buf, int size)
{
    int fd, n;

    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) return -1;
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

int device_find(struct device *dev)
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
        if (read_file(path, uevent, sizeof(uevent)) < 0 || !strstr(uevent, dev->model->hid_id)) continue;
        /* the MK3's display interface is on the USB device */
        dev->usb[0] = 0;
        if (dev->model->display == DISPLAY_SCREENS && usb_node(ent->d_name, dev->usb, sizeof(dev->usb)))
            continue;
        if (FORMAT(dev->hidraw, "/dev/%s", ent->d_name)) found = 0;
    }
    closedir(dir);
    return found;
}

/* the MK3's display interface */
static int claim_display(struct device *dev)
{
    unsigned int iface = DISPLAY_INTERFACE;

    if (dev->usbfs < 0 && (dev->usbfs = open(dev->usb, O_RDWR | O_CLOEXEC)) < 0) return -errno;
    /* on EBUSY the node stays open, closing it would look like the other program's close */
    return ioctl(dev->usbfs, USBDEVFS_CLAIMINTERFACE, &iface) < 0 ? -errno : 0;
}

int device_open(struct device *dev)
{
    int err;

    if (dev->model->display == DISPLAY_SCREENS && (err = claim_display(dev)))
    {
        if (err != -EBUSY) device_close(dev);
        return err;
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
    device_close(dev);
    return -err;
}

void device_close(struct device *dev)
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
static void controls(struct device *dev, struct engine *e, const unsigned char *r)
{
    const struct model *m = dev->model;
    const unsigned char *buttons = r + 1;
    int i, v, delta, first, second;

    for (i = 0; i < m->button_bits; i++)
        if (bit_set(buttons, i) != bit_set(dev->buttons, i)) engine_button(e, i, bit_set(buttons, i));
    memcpy(dev->buttons, buttons, (m->button_bits + 7) / 8);

    v = r[m->encoder_at] & (ENCODER_POSITIONS - 1);
    if (dev->seen && (delta = shortest(v - dev->encoder, ENCODER_POSITIONS))) engine_encoder(e, delta);
    dev->encoder = v;

    for (i = 0; m->knobs_at >= 0 && i < KNOBS; i++)
    {
        v = u16(r + m->knobs_at + 2 * i);
        if (dev->seen && (delta = shortest(v - dev->knobs[i], KNOB_COUNTS))) engine_knob(e, i + 1, delta);
        dev->knobs[i] = v;
    }

    /* with two fingers the first one counts */
    first = u16(r + m->strip_at);
    second = u16(r + m->strip_at + 2);
    v = first ? first : second;
    if (v != dev->strip) engine_strip(e, v);
    dev->strip = v;

    if (m->pedal_at >= 0)
    {
        v = clamp(u16(r + m->pedal_at), 0, PEDAL_MAX);
        if (dev->seen && v != dev->pedal) engine_pedal(e, v);
        dev->pedal = v;
    }
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

void device_report(struct device *dev, struct engine *e, const unsigned char *r, int len)
{
    if (r[0] == REPORT_CONTROLS && len >= dev->model->report_size) controls(dev, e, r);
    else if (r[0] == REPORT_PADS) pads(e, r, len);
}

int device_read(struct device *dev, struct engine *e)
{
    unsigned char r[128];
    ssize_t n;

    while ((n = read(dev->hid, r, sizeof(r))) > 0) device_report(dev, e, r, n);
    return n < 0 && errno != EAGAIN && errno != EINTR ? -1 : 0;
}

/* LEDs first to first + count, if they changed */
static int write_leds(struct device *dev, int report, const unsigned char *leds, int first, int count)
{
    unsigned char r[1 + MAX_LEDS];

    if (dev->leds_sent && !memcmp(leds + first, dev->leds + first, count)) return 0;
    r[0] = report;
    memcpy(r + 1, leds + first, count);
    return write(dev->hid, r, 1 + count) < 0 ? -1 : 0;
}

int device_leds(struct device *dev, const unsigned char leds[MAX_LEDS])
{
    const struct model *m = dev->model;
    int first = m->led_report_2 ? m->led_report_2 : m->leds;

    if (write_leds(dev, REPORT_LEDS, leds, 0, first)) return -1;
    if (first < m->leds && write_leds(dev, REPORT_LEDS_2, leds, first, m->leds - first)) return -1;
    memcpy(dev->leds, leds, m->leds);
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

int device_encode(unsigned char *out, int screen, const struct canvas *c, int x, int y, int w, int h)
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
static bool dirty_region(const struct device *dev, const struct canvas *c, int screen,
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

int device_screens(struct device *dev, const struct canvas screens[2])
{
    static unsigned char frame[FRAME_HEADER + 4 * SCREEN_W * SCREEN_H + 64];
    int i, len, left, top, right, bottom;

    for (i = 0; i < 2; i++)
    {
        if (!dirty_region(dev, &screens[i], i, &left, &top, &right, &bottom)) continue;
        /* pixels go in pairs */
        left &= ~1;
        right |= 1;
        len = device_encode(frame, i, &screens[i], left, top, right - left + 1, bottom - top + 1);
        if (bulk(dev->usbfs, frame, len)) return -1;
    }
    memcpy(dev->sent, screens, sizeof(dev->sent));
    dev->screens_sent = true;
    return 0;
}

static unsigned char *put16le(unsigned char *p, int v)
{
    p[0] = v;
    p[1] = v >> 8;
    return p + 2;
}

/* 8 rows of a column as the Mikro takes them */
static unsigned char mono_byte(const struct mono_canvas *c, int x, int page)
{
    unsigned char byte = 0;
    int k;

    for (k = 0; k < 8; k++)
        if (!c->px[8 * page + k][x]) byte |= 1 << k;
    return byte;
}

int device_encode_mono(unsigned char *out, const struct mono_canvas *c, int x, int page, int w, int pages)
{
    unsigned char *p = out;
    int i, j;

    *p++ = REPORT_MONO;
    p = put16le(put16le(put16le(put16le(p, x), page), w), pages);
    for (j = page; j < page + pages; j++)
        for (i = x; i < x + w; i++) *p++ = mono_byte(c, i, j);
    memset(p, 0, out + MONO_REPORT - p);
    return MONO_REPORT;
}

/* the columns and pages that differ from what was sent, false if nothing does */
static bool mono_dirty(const struct device *dev, const struct mono_canvas *c,
                       int *left, int *right, int *first, int *last)
{
    int x, page;

    *left = MONO_W;
    *right = -1;
    *first = MONO_PAGES;
    *last = -1;
    for (page = 0; page < MONO_PAGES; page++)
        for (x = 0; x < MONO_W; x++)
        {
            if (dev->screens_sent && mono_byte(c, x, page) == mono_byte(&dev->mono_sent, x, page)) continue;
            if (x < *left) *left = x;
            if (x > *right) *right = x;
            if (page < *first) *first = page;
            *last = page;
        }
    return *last >= 0;
}

/* the changed region, in as many 8-row pages per report as fit */
int device_mono(struct device *dev, const struct mono_canvas *c)
{
    unsigned char report[MONO_REPORT];
    int left, right, first, last, w, page, pages;

    if (mono_dirty(dev, c, &left, &right, &first, &last))
    {
        w = right - left + 1;
        for (page = first; page <= last; page += pages)
        {
            pages = MONO_DATA / w;
            if (pages > last - page + 1) pages = last - page + 1;
            device_encode_mono(report, c, left, page, w, pages);
            if (write(dev->hid, report, MONO_REPORT) < 0) return -1;
        }
    }
    memcpy(&dev->mono_sent, c, sizeof(*c));
    dev->screens_sent = true;
    return 0;
}

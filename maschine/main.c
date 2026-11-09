/*
 * ni-maschine: MIDI mode of the Maschine MK3 and Mikro MK3 without NI's software running.
 *
 * While a controller is plugged in, owns its ALSA port ("Native Instruments:Maschine
 * MK3 Ctrl MIDI", "Native Instruments:Maschine Mikro MK3") and does what
 * NIHostIntegrationAgent does in MIDI mode, with the Controller Editor template in the
 * ni-wine prefix. While a prefix with ni-wine's nimidi service runs, nimidi connects
 * and announces the ports NI's agent makes: the agent has that controller then, and
 * this program relays the agent's MIDI on the same port. It also leaves the MK3 to
 * any other program that holds its display interface.
 *
 * `ni-maschine --attach PORT` is what nimidi starts: it connects to nimidi at
 * 127.0.0.1:PORT, hands the connection to the running ni-maschine and exits.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include "device.h"
#include "engine.h"
#include "screen.h"
#include "seq.h"
#include "util.h"

#define CLIENT_NAME "Native Instruments"
#define TEMPLATE_DIR "drive_c/ProgramData/Native Instruments/Controller Editor"
#define SOCKET_NAME "ni-maschine.sock"
#define NIMIDI_LINKS 4              /* nimidi connections, one per running prefix */
#define CONTROLLERS MODELS          /* one of each model */
#define REDRAW_MS 15
#define MAX_RETRY_S 30
#define SEQ_FDS 8

/* frames to and from nimidi.exe: u8 type, u8 port, u16 length (little endian), payload */
#define FRAME_PORT_ADD 1
#define FRAME_PORT_REMOVE 2
#define FRAME_DATA 3
#define FRAME_HEADER 4
#define FRAME_MAX 0xffff

struct controller
{
    const struct model *model;
    struct device dev;
    struct engine engine;
    int port;                       /* its ALSA port, -1 if none */
    bool present, owned;
    unsigned int shown_changes;     /* engine.changes the LEDs and display show */
    int redraw_timer, retry_timer;
    bool redraw_pending;
    int retry_s;                    /* the next retry after a failure, backing off */
    int usb_watch;                  /* closes of the MK3's USB node while another program has it */
    bool busy_retry;                /* one more try after the display interface was busy */
    char mode_path[PATH_MAX];
    struct canvas screens[2];       /* MK3 */
    struct mono_canvas mono;        /* Mikro */
};

/* a connection from nimidi */
struct link
{
    int fd;                         /* -1 if unused */
    int ports[CONTROLLERS];         /* nimidi's ids of the controllers' ports, -1 if none */
    int have;
    unsigned char buf[FRAME_HEADER + FRAME_MAX];
};

static struct controller controllers[CONTROLLERS];
static struct seq *seq;
static struct link links[NIMIDI_LINKS];
static char template_dir[PATH_MAX];
static int signal_fd = -1, udev_fd = -1, listen_fd = -1, inotify_fd = -1;
static int template_watch = -1;
static bool watching_template_dir;  /* else the nearest parent that exists */

static void say(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "ni-maschine: ");
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static int socket_path(struct sockaddr_un *addr)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char fallback[64];

    if (!dir || !*dir)
    {
        snprintf(fallback, sizeof(fallback), "/run/user/%u", getuid());
        dir = fallback;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    return FORMAT(addr->sun_path, "%s/%s", dir, SOCKET_NAME) ? 0 : -1;
}

static int send_fd(int sock, int fd)
{
    char control[CMSG_SPACE(sizeof(int))] = {0}, byte = 0;
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control};
    struct cmsghdr *cmsg;

    msg.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}

/* --attach */
static int attach(int tcp_port)
{
    struct sockaddr_in nimidi = {.sin_family = AF_INET, .sin_port = htons(tcp_port)};
    struct sockaddr_un daemon;
    int tcp, local, tries;

    nimidi.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((tcp = socket(AF_INET, SOCK_STREAM, 0)) < 0
            || connect(tcp, (struct sockaddr *)&nimidi, sizeof(nimidi)) < 0)
    {
        say("connecting to nimidi failed: %s", strerror(errno));
        return 1;
    }
    if (socket_path(&daemon)) return 1;
    /* the service may still be starting with the session */
    for (tries = 0; ; tries++)
    {
        if ((local = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return 1;
        if (!connect(local, (struct sockaddr *)&daemon, sizeof(daemon))) break;
        close(local);
        if (tries == 50)
        {
            say("%s: %s, is ni-maschine.service running?", daemon.sun_path, strerror(errno));
            return 1;
        }
        usleep(100000);
    }
    return send_fd(local, tcp) ? 1 : 0;
}

/* timers */

static void arm(int timer, long ms)
{
    struct itimerspec t = {.it_value = {.tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000}};

    timerfd_settime(timer, 0, &t, NULL);
}

static bool expired(int timer)
{
    unsigned long long expirations;

    return read(timer, &expirations, sizeof(expirations)) > 0;
}

/* after a failure other than another program having the MK3, like no permission yet */
static void retry_later(struct controller *c)
{
    c->retry_s = c->retry_s ? c->retry_s * 2 : 1;
    if (c->retry_s > MAX_RETRY_S) c->retry_s = MAX_RETRY_S;
    arm(c->retry_timer, c->retry_s * 1000L);
}

/* the controllers */

/* NI's agent has it once nimidi announced its port */
static bool agents(const struct controller *c)
{
    int i, k = c - controllers;

    for (i = 0; i < NIMIDI_LINKS; i++)
        if (links[i].fd >= 0 && links[i].ports[k] >= 0) return true;
    return false;
}

static void lost(struct controller *c);

static void update_leds(struct controller *c)
{
    unsigned char leds[MAX_LEDS];

    if (!c->owned) return;
    engine_leds(&c->engine, leds);
    if (device_leds(&c->dev, leds)) lost(c);
}

static void redraw(struct controller *c)
{
    int ret;

    c->redraw_pending = false;
    if (!c->owned) return;
    if (c->model->display == DISPLAY_SCREENS)
    {
        screen_draw(&c->engine, c->screens);
        ret = device_screens(&c->dev, c->screens);
    }
    else
    {
        screen_draw_mono(&c->engine, &c->mono);
        ret = device_mono(&c->dev, &c->mono);
    }
    if (ret) lost(c);
}

/* LEDs now, the display a little later so that a burst of changes draws once */
static void refresh(struct controller *c)
{
    if (c->engine.changes == c->shown_changes) return;
    c->shown_changes = c->engine.changes;
    update_leds(c);
    if (c->owned && !c->redraw_pending)
    {
        c->redraw_pending = true;
        arm(c->redraw_timer, REDRAW_MS);
    }
}

static void unwatch_usb(struct controller *c)
{
    if (c->usb_watch >= 0) inotify_rm_watch(inotify_fd, c->usb_watch);
    c->usb_watch = -1;
}

static void open_port(struct controller *c)
{
    if (c->port >= 0 || !c->present) return;
    if (!seq) seq = seq_open(CLIENT_NAME);
    if (seq && (c->port = seq_add_port(seq, c->model->port_name)) >= 0) return;
    say("creating the ALSA port %s failed, trying again", c->model->port_name);
    retry_later(c);
}

static void take(struct controller *c)
{
    int ret;

    if (!c->present || c->owned || agents(c)) return;
    if ((ret = device_open(&c->dev)) == -EBUSY)
    {
        /* no event says when a usbfs claim ends, but the holder's close of the node does */
        if (c->usb_watch < 0)
        {
            c->usb_watch = inotify_add_watch(inotify_fd, c->dev.usb, IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
            say("the %s's display interface is in use, waiting for it", c->model->name);
        }
        if (c->busy_retry)
        {
            c->busy_retry = false;
            arm(c->retry_timer, 1000);
        }
        return;
    }
    unwatch_usb(c);
    c->busy_retry = false;
    if (ret)
    {
        say("opening the %s failed: %s, trying again", c->model->name, strerror(-ret));
        retry_later(c);
        return;
    }
    c->owned = true;
    c->retry_s = 0;
    /* whatever was held belongs to the one who had the controller */
    engine_reset_input(&c->engine);
    engine_connected(&c->engine);
    say("driving the %s (%s)", c->model->name, c->engine.midi_mode ? "MIDI mode" : "idle");
    c->shown_changes = c->engine.changes;
    update_leds(c);
    redraw(c);
}

static void release(struct controller *c)
{
    unwatch_usb(c);
    device_close(&c->dev);
    c->owned = false;
    c->redraw_pending = false;
}

/* NI's agent takes over */
static void hand_over(struct controller *c)
{
    if (!c->owned) return;
    say("NI's agent takes the %s", c->model->name);
    engine_release_all(&c->engine);
    release(c);
}

static void blank(struct controller *c)
{
    unsigned char leds[MAX_LEDS] = {0};

    if (!c->owned) return;
    memset(c->screens, 0, sizeof(c->screens));
    memset(&c->mono, 0, sizeof(c->mono));
    device_leds(&c->dev, leds);
    if (c->model->display == DISPLAY_SCREENS) device_screens(&c->dev, c->screens);
    else device_mono(&c->dev, &c->mono);
}

static void device_added(struct controller *c)
{
    if (c->present || device_find(&c->dev)) return;
    c->present = true;
    say("%s at %s", c->model->name, c->dev.hidraw);
    open_port(c);
    take(c);
}

static void device_removed(struct controller *c)
{
    if (!c->present) return;
    engine_release_all(&c->engine);
    release(c);
    seq_remove_port(seq, c->port);
    c->port = -1;
    c->present = false;
    say("%s removed", c->model->name);
}

/* an I/O error on an open device: unplugged, or taken away */
static void lost(struct controller *c)
{
    engine_release_all(&c->engine);
    release(c);
    if (device_find(&c->dev) || access(c->dev.hidraw, F_OK)) device_removed(c);
}

static void engine_midi(void *ctx, const unsigned char *data, int len)
{
    struct controller *c = ctx;

    if (c->port >= 0) seq_send(seq, c->port, data, len);
}

/* MIDI mode on or off, kept across restarts */

static void save_mode(struct controller *c)
{
    char dir[PATH_MAX], *p;
    FILE *f;

    snprintf(dir, sizeof(dir), "%s", c->mode_path);
    for (p = strchr(dir + 1, '/'); p; p = strchr(p + 1, '/'))
    {
        *p = 0;
        mkdir(dir, 0755);
        *p = '/';
    }
    if ((f = fopen(c->mode_path, "w")))
    {
        fprintf(f, "%s\n", c->engine.midi_mode ? "midi" : "idle");
        fclose(f);
    }
}

static void load_mode(struct controller *c)
{
    char buf[16] = "";
    FILE *f;

    if ((f = fopen(c->mode_path, "r")))
    {
        if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
        fclose(f);
    }
    engine_set_midi_mode(&c->engine, starts_with(buf, "midi"));
}

static void hid_input(struct controller *c)
{
    bool midi_mode = c->engine.midi_mode;

    if (device_read(&c->dev, &c->engine))
    {
        lost(c);
        return;
    }
    if (c->engine.midi_mode != midi_mode) save_mode(c);
    refresh(c);
}

/* the templates */

static void load_template(struct controller *c)
{
    char path[PATH_MAX];
    struct templates t;

    if (!FORMAT(path, "%s/%s", template_dir, c->model->template_file) || templates_load(&t, path))
    {
        if (!access(path, F_OK)) say("%s: can't read it, using NI's default template", path);
        templates_default(&t, c->model);
    }
    engine_set_templates(&c->engine, &t);
    refresh(c);
}

/* the templates' directory, or the nearest parent that exists until it does */
static void watch_templates(void)
{
    const unsigned int events = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF;
    char dir[PATH_MAX], *slash;

    if (template_watch >= 0) inotify_rm_watch(inotify_fd, template_watch);
    snprintf(dir, sizeof(dir), "%s", template_dir);
    for (;;)
    {
        template_watch = inotify_add_watch(inotify_fd, dir, events);
        if (template_watch >= 0 || !(slash = strrchr(dir, '/')) || slash == dir) break;
        *slash = 0;
    }
    watching_template_dir = template_watch >= 0 && !strcmp(dir, template_dir);
}

/* inotify: the templates, and the MK3's USB node while another program has it open */

/* an event of the template watch: the controller whose template was written, or NULL */
static struct controller *template_event(const struct inotify_event *ev, bool *rewatch)
{
    int i;

    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
    {
        *rewatch = true;
        return NULL;
    }
    /* a directory on the way to it appeared */
    if (!watching_template_dir)
    {
        if (ev->mask & IN_ISDIR) *rewatch = true;
        return NULL;
    }
    /* written, not just created */
    if (!ev->len || !(ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) return NULL;
    for (i = 0; i < CONTROLLERS; i++)
        if (!strcmp(ev->name, controllers[i].model->template_file)) return &controllers[i];
    return NULL;
}

static void inotify_input(void)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *ev;
    bool reload[CONTROLLERS] = {false};
    bool usb_closed[CONTROLLERS] = {false};
    bool rewatch = false, had_dir;
    struct controller *c;
    ssize_t n, i;
    int k;

    while ((n = read(inotify_fd, buf, sizeof(buf))) > 0)
        for (i = 0; i < n; i += sizeof(*ev) + ev->len)
        {
            ev = (const struct inotify_event *)(buf + i);
            for (k = 0; k < CONTROLLERS; k++)
            {
                c = &controllers[k];
                if (c->usb_watch < 0 || ev->wd != c->usb_watch) continue;
                if (ev->mask & IN_IGNORED) c->usb_watch = -1;
                else usb_closed[k] = true;
            }
            /* events of replaced watches don't count */
            if (ev->wd == template_watch && (c = template_event(ev, &rewatch)))
                reload[c - controllers] = true;
        }
    if (rewatch)
    {
        had_dir = watching_template_dir;
        watch_templates();
        if (watching_template_dir && !had_dir)
            for (k = 0; k < CONTROLLERS; k++) reload[k] = true;
    }
    for (k = 0; k < CONTROLLERS; k++)
    {
        if (reload[k]) load_template(&controllers[k]);
        /* the close comes just before the claim ends: try now and once more a bit later */
        if (usb_closed[k])
        {
            controllers[k].busy_retry = true;
            arm(controllers[k].retry_timer, 100);
        }
    }
}

/* udev */

/* what libudev puts before the properties */
struct udev_header
{
    char prefix[8];             /* "libudev" */
    unsigned int magic;         /* 0xfeedcafe, big endian */
    unsigned int header_size;
    unsigned int properties_off;
    unsigned int properties_len;
    unsigned int filter_subsystem_hash;
    unsigned int filter_devtype_hash;
    unsigned int filter_tag_bloom_hi;
    unsigned int filter_tag_bloom_lo;
};

static int udev_open(void)
{
    struct sockaddr_nl addr = {.nl_family = AF_NETLINK, .nl_groups = 2};    /* udevd's group */
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    int on = 1, size = 16 << 20;

    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));
    /* bursts on resume or when a hub comes back */
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size)))
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

/* udev events were lost: go by what is there now */
static void rescan(void)
{
    struct controller *c;
    struct device now;
    int k;

    for (k = 0; k < CONTROLLERS; k++)
    {
        c = &controllers[k];
        now = (struct device){.model = c->model, .hid = -1, .usbfs = -1};
        if (c->present && (device_find(&now) || strcmp(now.hidraw, c->dev.hidraw))) device_removed(c);
        device_added(c);
        take(c);
    }
}

/* only udevd's messages, which come after its rules ran (permissions) */
static bool from_udevd(struct msghdr *msg, const char *buf, ssize_t n)
{
    const struct udev_header *h = (const struct udev_header *)buf;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);

    if (!cmsg || cmsg->cmsg_type != SCM_CREDENTIALS) return false;
    if (((struct ucred *)CMSG_DATA(cmsg))->uid != 0) return false;
    return (size_t)n >= sizeof(*h) && !memcmp(h->prefix, "libudev", 8) && ntohl(h->magic) == 0xfeedcafe
        && h->properties_off + h->properties_len <= (size_t)n;
}

static void udev_message(const char *buf)
{
    const struct udev_header *h = (const struct udev_header *)buf;
    const char *action = "", *subsystem = "", *devname = "", *p, *end;
    struct controller *c;
    int k;

    end = buf + h->properties_off + h->properties_len;
    for (p = buf + h->properties_off; p < end; p += strlen(p) + 1)
    {
        if (starts_with(p, "ACTION=")) action = p + 7;
        else if (starts_with(p, "SUBSYSTEM=")) subsystem = p + 10;
        else if (starts_with(p, "DEVNAME=")) devname = p + 8;
    }
    if (strcmp(subsystem, "hidraw")) return;
    for (k = 0; k < CONTROLLERS; k++)
    {
        c = &controllers[k];
        if (!strcmp(action, "add")) device_added(c);
        else if (!strcmp(action, "remove") && c->present && !strcmp(devname, c->dev.hidraw))
            device_removed(c);
    }
}

static void udev_input(void)
{
    char buf[8192], control[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec iov = {buf, sizeof(buf) - 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control};
    ssize_t n;

    for (;;)
    {
        msg.msg_controllen = sizeof(control);
        if ((n = recvmsg(udev_fd, &msg, 0)) <= 0)
        {
            if (n < 0 && errno == ENOBUFS)
            {
                rescan();
                continue;
            }
            break;
        }
        if (!from_udevd(&msg, buf, n)) continue;
        buf[n] = 0;
        udev_message(buf);
    }
}

/* links to nimidi */

static void take_all(void)
{
    int k;

    for (k = 0; k < CONTROLLERS; k++) take(&controllers[k]);
}

static void link_close(struct link *l)
{
    int k;

    close(l->fd);
    l->fd = -1;
    for (k = 0; k < CONTROLLERS; k++)
    {
        if (l->ports[k] >= 0) say("NI's agent let go of the %s", controllers[k].model->name);
        l->ports[k] = -1;
    }
    take_all();
}

/* never blocks: a Wine side that doesn't read loses its link */
static void link_send(struct link *l, int type, int port, const unsigned char *data, int len)
{
    unsigned char frame[FRAME_HEADER + FRAME_MAX];

    if (len > FRAME_MAX) return;
    frame[0] = type;
    frame[1] = port;
    frame[2] = len & 0xff;
    frame[3] = len >> 8;
    memcpy(frame + FRAME_HEADER, data, len);
    if (send(l->fd, frame, FRAME_HEADER + len, MSG_NOSIGNAL | MSG_DONTWAIT) != FRAME_HEADER + len)
        shutdown(l->fd, SHUT_RDWR);
}

/* the controller whose port has this name */
static struct controller *named_controller(const unsigned char *name, int len)
{
    const char *own;
    int k;

    for (k = 0; k < CONTROLLERS; k++)
    {
        own = controllers[k].model->port_name;
        if (len == (int)strlen(own) && !memcmp(name, own, len)) return &controllers[k];
    }
    return NULL;
}

/* the controller whose port nimidi calls port */
static struct controller *linked_controller(const struct link *l, int port)
{
    int k;

    for (k = 0; k < CONTROLLERS; k++)
        if (l->ports[k] == port) return &controllers[k];
    return NULL;
}

static void link_frame(struct link *l, int type, int port, const unsigned char *data, int len)
{
    struct controller *c;
    int k;

    if (type == FRAME_PORT_ADD) c = named_controller(data, len);
    else c = linked_controller(l, port);
    if (!c) return;
    k = c - controllers;
    switch (type)
    {
    case FRAME_PORT_ADD:
        l->ports[k] = port;
        hand_over(c);
        break;
    case FRAME_PORT_REMOVE:
        l->ports[k] = -1;
        take(c);
        break;
    case FRAME_DATA:
        if (c->port >= 0) seq_send(seq, c->port, data, len);
        /* the agent sends only in MIDI mode, keep it when the controller comes back */
        if (!c->engine.midi_mode)
        {
            engine_set_midi_mode(&c->engine, true);
            save_mode(c);
        }
        break;
    }
}

static void link_input(struct link *l)
{
    int n, done, size;

    n = recv(l->fd, l->buf + l->have, sizeof(l->buf) - l->have, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
    {
        link_close(l);
        return;
    }
    if (n < 0) return;
    l->have += n;
    for (done = 0; l->have - done >= FRAME_HEADER; done += FRAME_HEADER + size)
    {
        size = l->buf[done + 2] | l->buf[done + 3] << 8;
        if (FRAME_HEADER + size > l->have - done) break;
        link_frame(l, l->buf[done], l->buf[done + 1], l->buf + done + FRAME_HEADER, size);
    }
    memmove(l->buf, l->buf + done, l->have - done);
    l->have -= done;
}

/* the fd an --attach of the same user sends, -1 if none */
static int receive_fd(int sock)
{
    char control[CMSG_SPACE(sizeof(int))], byte;
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control};
    struct timeval timeout = {.tv_sec = 1};
    struct ucred cred;
    socklen_t len = sizeof(cred);
    struct cmsghdr *cmsg;
    int fd = -1;

    msg.msg_controllen = sizeof(control);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != getuid()) return -1;
    if (recvmsg(sock, &msg, 0) != 1) return -1;
    if ((cmsg = CMSG_FIRSTHDR(&msg)) && cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

static void accept_attach(void)
{
    int sock, tcp, i, k;

    if ((sock = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC)) < 0) return;
    tcp = receive_fd(sock);
    close(sock);
    if (tcp < 0) return;
    for (i = 0; i < NIMIDI_LINKS && links[i].fd >= 0; i++);
    if (i == NIMIDI_LINKS)
    {
        close(tcp);
        return;
    }
    fcntl(tcp, F_SETFD, FD_CLOEXEC);
    fcntl(tcp, F_SETFL, fcntl(tcp, F_GETFL) | O_NONBLOCK);
    links[i].fd = tcp;
    for (k = 0; k < CONTROLLERS; k++) links[i].ports[k] = -1;
    links[i].have = 0;
}

static struct controller *port_owner(int port)
{
    int k;

    for (k = 0; k < CONTROLLERS; k++)
        if (controllers[k].port == port) return &controllers[k];
    return NULL;
}

/* the DAW -> the engine, and NI's agent when it has the controller */
static void port_input(void)
{
    unsigned char data[4096];
    struct controller *c;
    int n, i, port, k;

    while (seq && (n = seq_receive(seq, &port, data, sizeof(data))) > 0)
    {
        if (!(c = port_owner(port))) continue;
        engine_midi_in(&c->engine, data, n);
        k = c - controllers;
        for (i = 0; i < NIMIDI_LINKS; i++)
        {
            if (links[i].fd >= 0 && links[i].ports[k] >= 0)
                link_send(&links[i], FRAME_DATA, links[i].ports[k], data, n);
        }
    }
    for (k = 0; k < CONTROLLERS; k++) refresh(&controllers[k]);
}

/* setup */

static int listen_socket(void)
{
    struct sockaddr_un addr;
    int fd;

    if (socket_path(&addr) || (fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return -1;
    if (!connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
    {
        say("already running");
        exit(0);
    }
    close(fd);
    unlink(addr.sun_path);
    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) return -1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int setup_paths(void)
{
    const char *prefix = getenv("NI_WINE_PREFIX"), *home = getenv("HOME"), *state = getenv("XDG_STATE_HOME");
    char buf[PATH_MAX], state_dir[PATH_MAX];
    int k;

    if (!home) home = "/";
    if ((!prefix || !*prefix) && FORMAT(buf, "%s/.wine-ni", home)) prefix = buf;
    if (!FORMAT(template_dir, "%s/%s", prefix, TEMPLATE_DIR)) return -1;
    if (state && *state)
    {
        if (!FORMAT(state_dir, "%s/ni-wine", state)) return -1;
    }
    else if (!FORMAT(state_dir, "%s/.local/state/ni-wine", home)) return -1;
    for (k = 0; k < CONTROLLERS; k++)
        if (!FORMAT(controllers[k].mode_path, "%s/%s", state_dir, controllers[k].model->mode_file)) return -1;
    return 0;
}

static int open_fds(const sigset_t *signals)
{
    const int timer_flags = TFD_CLOEXEC | TFD_NONBLOCK;
    struct controller *c;
    int k;

    if ((signal_fd = signalfd(-1, signals, SFD_CLOEXEC)) < 0) return -1;
    if ((udev_fd = udev_open()) < 0) return -1;
    if ((listen_fd = listen_socket()) < 0) return -1;
    if ((inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) < 0) return -1;
    for (k = 0; k < CONTROLLERS; k++)
    {
        c = &controllers[k];
        if ((c->redraw_timer = timerfd_create(CLOCK_MONOTONIC, timer_flags)) < 0) return -1;
        if ((c->retry_timer = timerfd_create(CLOCK_MONOTONIC, timer_flags)) < 0) return -1;
    }
    return 0;
}

enum { POLL_SIGNAL, POLL_UDEV, POLL_LISTEN, POLL_INOTIFY, POLL_LINKS };
#define POLL_SEQ (POLL_LINKS + NIMIDI_LINKS)
enum { CONTROLLER_REDRAW, CONTROLLER_RETRY, CONTROLLER_HID, CONTROLLER_FDS };

static void add_controller_fds(struct pollfd *fds, int at, const struct controller *c)
{
    fds[at + CONTROLLER_REDRAW] = (struct pollfd){.fd = c->redraw_timer, .events = POLLIN};
    fds[at + CONTROLLER_RETRY] = (struct pollfd){.fd = c->retry_timer, .events = POLLIN};
    fds[at + CONTROLLER_HID] = (struct pollfd){.fd = c->owned ? c->dev.hid : -1, .events = POLLIN};
}

static void controller_events(struct controller *c, const struct pollfd *fds)
{
    if (fds[CONTROLLER_REDRAW].revents && expired(c->redraw_timer)) redraw(c);
    if (fds[CONTROLLER_HID].revents && c->owned) hid_input(c);
    if (fds[CONTROLLER_RETRY].revents && expired(c->retry_timer))
    {
        open_port(c);
        take(c);
    }
}

/* until a signal */
static void run(void)
{
    struct pollfd fds[POLL_SEQ + SEQ_FDS + CONTROLLERS * CONTROLLER_FDS];
    int n, i, k, seq_count, controllers_at;

    for (;;)
    {
        fds[POLL_SIGNAL] = (struct pollfd){.fd = signal_fd, .events = POLLIN};
        fds[POLL_UDEV] = (struct pollfd){.fd = udev_fd, .events = POLLIN};
        fds[POLL_LISTEN] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
        fds[POLL_INOTIFY] = (struct pollfd){.fd = inotify_fd, .events = POLLIN};
        for (i = 0; i < NIMIDI_LINKS; i++)
            fds[POLL_LINKS + i] = (struct pollfd){.fd = links[i].fd, .events = POLLIN};
        seq_count = seq ? seq_poll_fds(seq, fds + POLL_SEQ, SEQ_FDS) : 0;
        controllers_at = POLL_SEQ + seq_count;
        for (k = 0; k < CONTROLLERS; k++)
            add_controller_fds(fds, controllers_at + k * CONTROLLER_FDS, &controllers[k]);
        n = controllers_at + CONTROLLERS * CONTROLLER_FDS;

        if (poll(fds, n, -1) < 0)
        {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[POLL_SIGNAL].revents) return;
        if (fds[POLL_UDEV].revents) udev_input();
        if (fds[POLL_LISTEN].revents) accept_attach();
        if (fds[POLL_INOTIFY].revents) inotify_input();
        for (k = 0; k < CONTROLLERS; k++)
            controller_events(&controllers[k], fds + controllers_at + k * CONTROLLER_FDS);
        for (i = 0; i < seq_count; i++)
            if (fds[POLL_SEQ + i].revents)
            {
                port_input();
                break;
            }
        for (i = 0; i < NIMIDI_LINKS; i++)
            if (links[i].fd >= 0 && fds[POLL_LINKS + i].revents) link_input(&links[i]);
    }
}

static void init_controllers(void)
{
    struct controller *c;
    int k;

    for (k = 0; k < CONTROLLERS; k++)
    {
        c = &controllers[k];
        c->model = models[k];
        c->dev = (struct device){.model = c->model, .hid = -1, .usbfs = -1};
        c->port = -1;
        c->redraw_timer = -1;
        c->retry_timer = -1;
        c->usb_watch = -1;
        engine_init(&c->engine, c->model, engine_midi, c);
    }
}

int main(int argc, char **argv)
{
    sigset_t signals;
    int i, k;

    if (argc == 3 && !strcmp(argv[1], "--attach")) return attach(atoi(argv[2]));
    if (argc != 1)
    {
        fprintf(stderr, "usage: ni-maschine [--attach PORT]\n");
        return 2;
    }

    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigprocmask(SIG_BLOCK, &signals, NULL);
    signal(SIGPIPE, SIG_IGN);
    for (i = 0; i < NIMIDI_LINKS; i++) links[i].fd = -1;
    init_controllers();

    if (setup_paths())
    {
        say("paths too long");
        return 1;
    }
    for (k = 0; k < CONTROLLERS; k++) load_mode(&controllers[k]);
    if (open_fds(&signals))
    {
        say("setup failed: %s", strerror(errno));
        return 1;
    }
    watch_templates();
    for (k = 0; k < CONTROLLERS; k++)
    {
        load_template(&controllers[k]);
        device_added(&controllers[k]);
    }

    run();

    for (k = 0; k < CONTROLLERS; k++)
    {
        engine_release_all(&controllers[k].engine);
        blank(&controllers[k]);
        release(&controllers[k]);
        engine_free(&controllers[k].engine);
    }
    seq_close(seq);
    return 0;
}

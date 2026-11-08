/*
 * ni-maschine: MIDI mode of the Maschine MK3 without NI's software running.
 *
 * While an MK3 is plugged in, owns the ALSA port "Native Instruments:Maschine
 * MK3 Ctrl MIDI" and does what NIHostIntegrationAgent does in MIDI mode, with
 * the Controller Editor template in the ni-wine prefix. While a prefix with
 * ni-wine's nimidi service runs, nimidi connects and NI's agent gets the MK3:
 * this program then only relays the agent's MIDI on the same port. It also
 * leaves the MK3 to any other program that holds the display interface.
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
#include "engine.h"
#include "mk3.h"
#include "screen.h"
#include "seq.h"
#include "util.h"

#define CLIENT_NAME "Native Instruments"
#define PORT_NAME "Maschine MK3 Ctrl MIDI"
#define TEMPLATE_DIR "drive_c/ProgramData/Native Instruments/Controller Editor"
#define TEMPLATE_FILE "MaschineMK3.ncs"
#define SOCKET_NAME "ni-maschine.sock"
#define NIMIDI_LINKS 4             /* nimidi connections, one per running prefix */
#define REDRAW_MS 15
#define MAX_RETRY_S 30
#define SEQ_FDS 8

/* frames to and from nimidi.exe: u8 type, u8 port, u16 length (little endian), payload */
#define FRAME_PORT_ADD 1
#define FRAME_PORT_REMOVE 2
#define FRAME_DATA 3
#define FRAME_HEADER 4
#define FRAME_MAX 0xffff

/* a connection from nimidi */
struct link
{
    int fd;                     /* -1 if unused */
    int port;                   /* nimidi's id of the MK3 port, -1 if none */
    int have;
    unsigned char buf[FRAME_HEADER + FRAME_MAX];
};

static struct engine engine;
static struct mk3 dev = {.hid = -1, .usbfs = -1};
static bool present, owned;
static struct seq *seq;
static struct link links[NIMIDI_LINKS];
static struct canvas screens[2];
static unsigned int shown_changes;  /* engine.changes the LEDs and screens show */
static char template_dir[PATH_MAX], state_path[PATH_MAX];

static int signal_fd = -1, udev_fd = -1, listen_fd = -1, inotify_fd = -1;
static int redraw_timer = -1, retry_timer = -1;
static bool redraw_pending;
static int retry_s;                 /* the next retry after a failure, backing off */
static int template_watch = -1;
static bool watching_template_dir;  /* else the nearest parent that exists */
static int usb_watch = -1;          /* closes of the USB node while another program has the MK3 */
static bool busy_retry;             /* one more try after the display interface was busy */

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

static void retry_in(long ms)
{
    arm(retry_timer, ms);
}

/* after a failure other than another program having the MK3, like no permission yet */
static void retry_later(void)
{
    retry_s = retry_s ? retry_s * 2 : 1;
    if (retry_s > MAX_RETRY_S) retry_s = MAX_RETRY_S;
    retry_in(retry_s * 1000L);
}

/* the MK3 */

static bool relaying(void)
{
    int i;

    for (i = 0; i < NIMIDI_LINKS; i++)
        if (links[i].fd >= 0 && links[i].port >= 0) return true;
    return false;
}

static void lost(void);

static void update_leds(void)
{
    unsigned char leds[MK3_LEDS];

    if (!owned) return;
    engine_leds(&engine, leds);
    if (mk3_leds(&dev, leds)) lost();
}

static void redraw(void)
{
    redraw_pending = false;
    if (!owned) return;
    screen_draw(&engine, screens);
    if (mk3_screens(&dev, screens)) lost();
}

/* LEDs now, the screens a little later so that a burst of changes draws once */
static void refresh(void)
{
    if (engine.changes == shown_changes) return;
    shown_changes = engine.changes;
    update_leds();
    if (owned && !redraw_pending)
    {
        redraw_pending = true;
        arm(redraw_timer, REDRAW_MS);
    }
}

static void unwatch_usb(void)
{
    if (usb_watch >= 0) inotify_rm_watch(inotify_fd, usb_watch);
    usb_watch = -1;
}

static void open_port(void)
{
    if (seq || !present) return;
    if (!(seq = seq_open(CLIENT_NAME, PORT_NAME)))
    {
        say("creating the ALSA port failed, trying again");
        retry_later();
    }
}

static void take(void)
{
    int ret;

    if (!present || owned || relaying()) return;
    if ((ret = mk3_open(&dev)) == -EBUSY)
    {
        /* no event says when a usbfs claim ends, but the holder's close of the node does */
        if (usb_watch < 0)
        {
            usb_watch = inotify_add_watch(inotify_fd, dev.usb, IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
            say("the display interface is in use, waiting for it");
        }
        if (busy_retry)
        {
            busy_retry = false;
            retry_in(1000);
        }
        return;
    }
    unwatch_usb();
    busy_retry = false;
    if (ret)
    {
        say("opening the MK3 failed: %s, trying again", strerror(-ret));
        retry_later();
        return;
    }
    owned = true;
    retry_s = 0;
    /* whatever was held belongs to the one who had the MK3 */
    engine_reset_input(&engine);
    say("driving the MK3 (%s)", engine.midi_mode ? "MIDI mode" : "idle");
    shown_changes = engine.changes;
    update_leds();
    redraw();
}

static void release(void)
{
    unwatch_usb();
    mk3_close(&dev);
    owned = false;
    redraw_pending = false;
}

static void blank(void)
{
    unsigned char leds[MK3_LEDS] = {0};

    if (!owned) return;
    memset(screens, 0, sizeof(screens));
    mk3_leds(&dev, leds);
    mk3_screens(&dev, screens);
}

static void device_added(void)
{
    if (present || mk3_find(&dev)) return;
    present = true;
    say("MK3 at %s", dev.hidraw);
    open_port();
    take();
}

static void device_removed(void)
{
    if (!present) return;
    engine_release_all(&engine);
    release();
    seq_close(seq);
    seq = NULL;
    present = false;
    say("MK3 removed");
}

/* an I/O error on an open device: unplugged, or taken away */
static void lost(void)
{
    engine_release_all(&engine);
    release();
    if (mk3_find(&dev) || access(dev.hidraw, F_OK)) device_removed();
}

static void engine_midi(void *ctx, const unsigned char *data, int len)
{
    if (seq) seq_send(seq, data, len);
}

/* MIDI mode on or off, kept across restarts */

static void save_mode(void)
{
    char dir[PATH_MAX], *p;
    FILE *f;

    snprintf(dir, sizeof(dir), "%s", state_path);
    for (p = strchr(dir + 1, '/'); p; p = strchr(p + 1, '/'))
    {
        *p = 0;
        mkdir(dir, 0755);
        *p = '/';
    }
    if ((f = fopen(state_path, "w")))
    {
        fprintf(f, "%s\n", engine.midi_mode ? "midi" : "idle");
        fclose(f);
    }
}

static void load_mode(void)
{
    char buf[16] = "";
    FILE *f;

    if ((f = fopen(state_path, "r")))
    {
        if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
        fclose(f);
    }
    engine_set_midi_mode(&engine, starts_with(buf, "midi"));
}

static void hid_input(void)
{
    bool midi_mode = engine.midi_mode;

    if (mk3_read(&dev, &engine))
    {
        lost();
        return;
    }
    if (engine.midi_mode != midi_mode) save_mode();
    refresh();
}

/* the template */

static void load_template(void)
{
    char path[PATH_MAX];
    struct templates t;

    if (!FORMAT(path, "%s/%s", template_dir, TEMPLATE_FILE) || templates_load(&t, path))
    {
        if (!access(path, F_OK)) say("%s: can't read it, using NI's default template", path);
        templates_default(&t);
    }
    engine_set_templates(&engine, &t);
    refresh();
}

/* the template's directory, or the nearest parent that exists until it does */
static void watch_template(void)
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

/* inotify: the template, and the USB node another program has open */

/* an event of the template watch; true if the template was written */
static bool template_event(const struct inotify_event *ev, bool *rewatch)
{
    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
    {
        *rewatch = true;
        return false;
    }
    /* a directory on the way to it appeared */
    if (!watching_template_dir)
    {
        if (ev->mask & IN_ISDIR) *rewatch = true;
        return false;
    }
    /* written, not just created */
    return ev->len && !strcmp(ev->name, TEMPLATE_FILE) && ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO);
}

static void inotify_input(void)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *ev;
    bool reload = false, rewatch = false, usb_closed = false, had_dir;
    ssize_t n, i;

    while ((n = read(inotify_fd, buf, sizeof(buf))) > 0)
        for (i = 0; i < n; i += sizeof(*ev) + ev->len)
        {
            ev = (const struct inotify_event *)(buf + i);
            if (usb_watch >= 0 && ev->wd == usb_watch)
            {
                if (ev->mask & IN_IGNORED) usb_watch = -1;
                else usb_closed = true;
            }
            /* events of replaced watches don't count */
            else if (ev->wd == template_watch && template_event(ev, &rewatch)) reload = true;
        }
    if (rewatch)
    {
        had_dir = watching_template_dir;
        watch_template();
        if (watching_template_dir && !had_dir) reload = true;
    }
    if (reload) load_template();
    /* the close comes just before the claim ends: try now and once more a bit later */
    if (usb_closed)
    {
        busy_retry = true;
        retry_in(100);
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
    struct mk3 now = {.hid = -1, .usbfs = -1};

    if (present && (mk3_find(&now) || strcmp(now.hidraw, dev.hidraw))) device_removed();
    device_added();
    take();
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

    end = buf + h->properties_off + h->properties_len;
    for (p = buf + h->properties_off; p < end; p += strlen(p) + 1)
    {
        if (starts_with(p, "ACTION=")) action = p + 7;
        else if (starts_with(p, "SUBSYSTEM=")) subsystem = p + 10;
        else if (starts_with(p, "DEVNAME=")) devname = p + 8;
    }
    if (strcmp(subsystem, "hidraw")) return;
    if (!strcmp(action, "add")) device_added();
    else if (!strcmp(action, "remove") && present && !strcmp(devname, dev.hidraw)) device_removed();
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

static void link_close(struct link *l)
{
    close(l->fd);
    l->fd = -1;
    if (l->port >= 0) say("NI's agent is gone");
    l->port = -1;
    take();
}

/* never blocks: a Wine side that doesn't read loses its link */
static void link_send(struct link *l, int type, const unsigned char *data, int len)
{
    unsigned char frame[FRAME_HEADER + FRAME_MAX];

    if (len > FRAME_MAX) return;
    frame[0] = type;
    frame[1] = l->port;
    frame[2] = len & 0xff;
    frame[3] = len >> 8;
    memcpy(frame + FRAME_HEADER, data, len);
    if (send(l->fd, frame, FRAME_HEADER + len, MSG_NOSIGNAL | MSG_DONTWAIT) != FRAME_HEADER + len)
        shutdown(l->fd, SHUT_RDWR);
}

static void link_frame(struct link *l, int type, int port, const unsigned char *data, int len)
{
    switch (type)
    {
    case FRAME_PORT_ADD:
        if (len < (int)strlen(PORT_NAME) || memcmp(data, PORT_NAME, strlen(PORT_NAME))) break;
        l->port = port;
        say("NI's agent takes the MK3");
        engine_release_all(&engine);
        release();
        break;
    case FRAME_PORT_REMOVE:
        if (port != l->port) break;
        l->port = -1;
        take();
        break;
    case FRAME_DATA:
        if (port != l->port) break;
        if (seq) seq_send(seq, data, len);
        /* the agent sends only in MIDI mode, keep it when the MK3 comes back */
        if (!engine.midi_mode)
        {
            engine_set_midi_mode(&engine, true);
            save_mode();
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
    int sock, tcp, i;

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
    links[i].port = -1;
    links[i].have = 0;
}

/* the DAW -> the engine, and NI's agent when it has the MK3 */
static void port_input(void)
{
    unsigned char data[4096];
    int n, i;

    while (seq && (n = seq_receive(seq, data, sizeof(data))) > 0)
    {
        engine_midi_in(&engine, data, n);
        for (i = 0; i < NIMIDI_LINKS; i++)
            if (links[i].fd >= 0 && links[i].port >= 0) link_send(&links[i], FRAME_DATA, data, n);
    }
    refresh();
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
    char buf[PATH_MAX];

    if (!home) home = "/";
    if ((!prefix || !*prefix) && FORMAT(buf, "%s/.wine-ni", home)) prefix = buf;
    if (!FORMAT(template_dir, "%s/%s", prefix, TEMPLATE_DIR)) return -1;
    if (state && *state) return FORMAT(state_path, "%s/ni-wine/maschine-mode", state) ? 0 : -1;
    return FORMAT(state_path, "%s/.local/state/ni-wine/maschine-mode", home) ? 0 : -1;
}

static int open_fds(const sigset_t *signals)
{
    const int timer_flags = TFD_CLOEXEC | TFD_NONBLOCK;

    if ((signal_fd = signalfd(-1, signals, SFD_CLOEXEC)) < 0) return -1;
    if ((udev_fd = udev_open()) < 0) return -1;
    if ((listen_fd = listen_socket()) < 0) return -1;
    if ((inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) < 0) return -1;
    if ((redraw_timer = timerfd_create(CLOCK_MONOTONIC, timer_flags)) < 0) return -1;
    if ((retry_timer = timerfd_create(CLOCK_MONOTONIC, timer_flags)) < 0) return -1;
    return 0;
}

enum { POLL_SIGNAL, POLL_UDEV, POLL_LISTEN, POLL_INOTIFY, POLL_REDRAW, POLL_RETRY, POLL_HID, POLL_SEQ };

/* until a signal */
static void run(void)
{
    struct pollfd fds[POLL_SEQ + SEQ_FDS + NIMIDI_LINKS];
    int n, i, seq_count, links_at;

    for (;;)
    {
        fds[POLL_SIGNAL] = (struct pollfd){.fd = signal_fd, .events = POLLIN};
        fds[POLL_UDEV] = (struct pollfd){.fd = udev_fd, .events = POLLIN};
        fds[POLL_LISTEN] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
        fds[POLL_INOTIFY] = (struct pollfd){.fd = inotify_fd, .events = POLLIN};
        fds[POLL_REDRAW] = (struct pollfd){.fd = redraw_timer, .events = POLLIN};
        fds[POLL_RETRY] = (struct pollfd){.fd = retry_timer, .events = POLLIN};
        fds[POLL_HID] = (struct pollfd){.fd = owned ? dev.hid : -1, .events = POLLIN};
        seq_count = seq ? seq_poll_fds(seq, fds + POLL_SEQ, SEQ_FDS) : 0;
        links_at = n = POLL_SEQ + seq_count;
        for (i = 0; i < NIMIDI_LINKS; i++) fds[n++] = (struct pollfd){.fd = links[i].fd, .events = POLLIN};

        if (poll(fds, n, -1) < 0)
        {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[POLL_SIGNAL].revents) return;
        if (fds[POLL_UDEV].revents) udev_input();
        if (fds[POLL_LISTEN].revents) accept_attach();
        if (fds[POLL_INOTIFY].revents) inotify_input();
        if (fds[POLL_REDRAW].revents && expired(redraw_timer)) redraw();
        if (fds[POLL_HID].revents && owned) hid_input();
        if (fds[POLL_RETRY].revents && expired(retry_timer))
        {
            open_port();
            take();
        }
        for (i = 0; i < seq_count; i++)
            if (fds[POLL_SEQ + i].revents)
            {
                port_input();
                break;
            }
        for (i = 0; i < NIMIDI_LINKS; i++)
            if (links[i].fd >= 0 && fds[links_at + i].revents) link_input(&links[i]);
    }
}

int main(int argc, char **argv)
{
    sigset_t signals;
    int i;

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

    if (setup_paths())
    {
        say("paths too long");
        return 1;
    }
    engine_init(&engine, engine_midi, NULL);
    load_mode();
    if (open_fds(&signals))
    {
        say("setup failed: %s", strerror(errno));
        return 1;
    }
    watch_template();
    load_template();
    device_added();

    run();

    engine_release_all(&engine);
    blank();
    release();
    seq_close(seq);
    engine_free(&engine);
    return 0;
}

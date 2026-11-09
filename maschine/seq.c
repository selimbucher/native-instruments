#include <stdlib.h>
#include <alsa/asoundlib.h>
#include "seq.h"

#define SYSEX_MAX 4096
#define MAX_PORTS 8

/* the MIDI byte stream of a port, each with its own running status and SysEx */
struct port
{
    int id;                     /* -1 if unused */
    snd_midi_event_t *encoder, *decoder;
};

struct seq
{
    snd_seq_t *seq;
    struct port ports[MAX_PORTS];
};

static struct port *find_port(struct seq *s, int id)
{
    int i;

    for (i = 0; i < MAX_PORTS; i++)
        if (s->ports[i].id >= 0 && s->ports[i].id == id) return &s->ports[i];
    return NULL;
}

static void clear_port(struct port *p)
{
    if (p->encoder) snd_midi_event_free(p->encoder);
    if (p->decoder) snd_midi_event_free(p->decoder);
    memset(p, 0, sizeof(*p));
    p->id = -1;
}

struct seq *seq_open(const char *client)
{
    struct seq *s = calloc(1, sizeof(*s));
    int i;

    if (!s) return NULL;
    for (i = 0; i < MAX_PORTS; i++) s->ports[i].id = -1;
    if (snd_seq_open(&s->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
    {
        free(s);
        return NULL;
    }
    snd_seq_set_client_name(s->seq, client);
    return s;
}

void seq_close(struct seq *s)
{
    int i;

    if (!s) return;
    for (i = 0; i < MAX_PORTS; i++)
        if (s->ports[i].id >= 0) seq_remove_port(s, s->ports[i].id);
    snd_seq_close(s->seq);
    free(s);
}

int seq_add_port(struct seq *s, const char *name)
{
    const unsigned int caps = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_READ
        | SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_DUPLEX;
    const unsigned int type = SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE
        | SND_SEQ_PORT_TYPE_APPLICATION;
    struct port *p = NULL;
    int i;

    for (i = 0; i < MAX_PORTS && !p; i++)
        if (s->ports[i].id < 0) p = &s->ports[i];
    if (!p) return -1;
    if (snd_midi_event_new(SYSEX_MAX, &p->encoder) < 0 || snd_midi_event_new(SYSEX_MAX, &p->decoder) < 0
            || (p->id = snd_seq_create_simple_port(s->seq, name, caps, type)) < 0)
    {
        clear_port(p);
        return -1;
    }
    snd_midi_event_no_status(p->decoder, 1);
    return p->id;
}

void seq_remove_port(struct seq *s, int id)
{
    struct port *p = find_port(s, id);

    if (!p) return;
    snd_seq_delete_simple_port(s->seq, id);
    clear_port(p);
}

void seq_send(struct seq *s, int id, const unsigned char *data, int len)
{
    struct port *p = find_port(s, id);
    snd_seq_event_t ev;
    long used;

    if (!p) return;
    while (len > 0)
    {
        snd_seq_ev_clear(&ev);
        if ((used = snd_midi_event_encode(p->encoder, data, len, &ev)) <= 0)
        {
            snd_midi_event_reset_encode(p->encoder);
            return;
        }
        data += used;
        len -= used;
        if (ev.type == SND_SEQ_EVENT_NONE) continue;
        snd_seq_ev_set_source(&ev, id);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(s->seq, &ev);
    }
}

int seq_receive(struct seq *s, int *id, unsigned char *buf, int size)
{
    snd_seq_event_t *ev;
    struct port *p;
    long n;

    while (snd_seq_event_input(s->seq, &ev) >= 0)
    {
        if (!(p = find_port(s, ev->dest.port))) continue;
        if ((n = snd_midi_event_decode(p->decoder, buf, size, ev)) > 0)
        {
            *id = p->id;
            return n;
        }
    }
    return 0;
}

int seq_poll_fds(struct seq *s, struct pollfd *fds, int max)
{
    return snd_seq_poll_descriptors(s->seq, fds, max, POLLIN);
}

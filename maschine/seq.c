#include <stdlib.h>
#include <alsa/asoundlib.h>
#include "seq.h"

#define SYSEX_MAX 4096

struct seq
{
    snd_seq_t *seq;
    int port;
    snd_midi_event_t *encoder, *decoder;
};

struct seq *seq_open(const char *client, const char *port)
{
    const unsigned int caps = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_READ
        | SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_DUPLEX;
    const unsigned int type = SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE
        | SND_SEQ_PORT_TYPE_APPLICATION;
    struct seq *s = calloc(1, sizeof(*s));

    if (!s) return NULL;
    if (snd_seq_open(&s->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
    {
        free(s);
        return NULL;
    }
    snd_seq_set_client_name(s->seq, client);
    s->port = snd_seq_create_simple_port(s->seq, port, caps, type);
    if (s->port < 0 || snd_midi_event_new(SYSEX_MAX, &s->encoder) < 0
            || snd_midi_event_new(SYSEX_MAX, &s->decoder) < 0)
    {
        seq_close(s);
        return NULL;
    }
    snd_midi_event_no_status(s->decoder, 1);
    return s;
}

void seq_close(struct seq *s)
{
    if (!s) return;
    if (s->encoder) snd_midi_event_free(s->encoder);
    if (s->decoder) snd_midi_event_free(s->decoder);
    snd_seq_close(s->seq);
    free(s);
}

void seq_send(struct seq *s, const unsigned char *data, int len)
{
    snd_seq_event_t ev;
    long used;

    while (len > 0)
    {
        snd_seq_ev_clear(&ev);
        if ((used = snd_midi_event_encode(s->encoder, data, len, &ev)) <= 0)
        {
            snd_midi_event_reset_encode(s->encoder);
            return;
        }
        data += used;
        len -= used;
        if (ev.type == SND_SEQ_EVENT_NONE) continue;
        snd_seq_ev_set_source(&ev, s->port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(s->seq, &ev);
    }
}

int seq_receive(struct seq *s, unsigned char *buf, int size)
{
    snd_seq_event_t *ev;
    long n;

    while (snd_seq_event_input(s->seq, &ev) >= 0)
    {
        if (ev->dest.port != s->port) continue;
        if ((n = snd_midi_event_decode(s->decoder, buf, size, ev)) > 0) return n;
    }
    return 0;
}

int seq_poll_fds(struct seq *s, struct pollfd *fds, int max)
{
    return snd_seq_poll_descriptors(s->seq, fds, max, POLLIN);
}

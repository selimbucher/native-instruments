/* The MK3's Ctrl MIDI port as an ALSA sequencer port. */

#ifndef SEQ_H
#define SEQ_H

#include <poll.h>

struct seq;

struct seq *seq_open(const char *client, const char *port);
void seq_close(struct seq *s);
void seq_send(struct seq *s, const unsigned char *data, int len);
/* the next MIDI message sent to the port, 0 if there is none */
int seq_receive(struct seq *s, unsigned char *buf, int size);
int seq_poll_fds(struct seq *s, struct pollfd *fds, int max);

#endif

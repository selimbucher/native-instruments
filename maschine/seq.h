/* The ALSA sequencer client "Native Instruments" with a port per controller. */

#ifndef SEQ_H
#define SEQ_H

#include <poll.h>

struct seq;

struct seq *seq_open(const char *client);
void seq_close(struct seq *s);
/* a duplex port, -1 on failure */
int seq_add_port(struct seq *s, const char *name);
void seq_remove_port(struct seq *s, int port);
void seq_send(struct seq *s, int port, const unsigned char *data, int len);
/* the next MIDI message sent to one of the ports and that port, 0 if there is none */
int seq_receive(struct seq *s, int *port, unsigned char *buf, int size);
int seq_poll_fds(struct seq *s, struct pollfd *fds, int max);

#endif

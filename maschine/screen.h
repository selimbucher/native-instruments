/* What the displays show in MIDI mode, drawn from the engine's state: the MK3's two
 * colour screens, the Mikro MK3's 128x32 monochrome one. */

#ifndef SCREEN_DRAW_H
#define SCREEN_DRAW_H

#include "engine.h"

#define SCREEN_W 480
#define SCREEN_H 272
#define MONO_W 128
#define MONO_H 32

/* RGB565 */
struct canvas
{
    unsigned short px[SCREEN_H][SCREEN_W];
};

/* 1 = lit */
struct mono_canvas
{
    unsigned char px[MONO_H][MONO_W];
};

void screen_draw(const struct engine *e, struct canvas screens[2]);
void screen_draw_mono(const struct engine *e, struct mono_canvas *c);

#endif

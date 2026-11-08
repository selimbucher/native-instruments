/* The two MK3 screens in MIDI mode, drawn from the engine's state. */

#ifndef SCREEN_DRAW_H
#define SCREEN_DRAW_H

#include "engine.h"

#define SCREEN_W 480
#define SCREEN_H 272

/* RGB565 */
struct canvas
{
    unsigned short px[SCREEN_H][SCREEN_W];
};

void screen_draw(const struct engine *e, struct canvas screens[2]);

#endif

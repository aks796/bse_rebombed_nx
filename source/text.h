/* Dynamic text rendering with FreeType over the console's shared fonts.
 *
 * BombSquad ships bitmap font atlases for Latin glyphs and asks the platform
 * to rasterize anything else -- CJK player names, Korean chat, symbols. On
 * Android that is a Canvas call; here it is FreeType driven by the system
 * fonts the console already provides.
 */

#ifndef BSNX_TEXT_H
#define BSNX_TEXT_H

#include <stdbool.h>

/* Returns false when no usable font could be opened; the caller then falls
 * back to blank bitmaps rather than failing the frame. */
bool text_init(void);

/* Ink bounds and advance for one string at the engine's base font size,
 * written as {left, right, top, bottom, advance}. Top is above the baseline
 * and bottom is below it (negative), matching the engine's Rect. */
void text_measure(const char *utf8, float out[5]);

/* Rasterize `count` strings into an RGBA_8888 buffer of width*height.
 * positions holds an (x, baseline_y) pair per string, in pixels from the
 * top-left of the bitmap. */
void text_render(void *pixels, int width, int height, const char *const *strings,
                 const float *positions, int count, float scale);

#endif

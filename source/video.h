/* EGL context on the console's default window. */

#ifndef BSNX_VIDEO_H
#define BSNX_VIDEO_H

#include <stdbool.h>

void video_init(void);
void video_swap(void);
void video_shutdown(void);

int video_width(void);
int video_height(void);

/* True once, after the console changes between docked and handheld; the
 * caller is expected to tell the engine about the new surface size. */
bool video_take_resize(void);

#endif

/* A minimal OpenSL ES 1.0.1 object model backed by SDL2 audio.
 *
 * BombSquad reaches audio through Oboe, which dlopens libOpenSLES.so and
 * resolves slCreateEngine plus the SL_IID_* interface tokens by name.
 */

#ifndef BSNX_OPENSLES_H
#define BSNX_OPENSLES_H

#include <stdint.h>

uint32_t slCreateEngine(void **engine, uint32_t num_options,
                        const void *options, uint32_t num_interfaces,
                        const void *interface_ids, const void *required);

void opensles_shutdown(void);

/* Resolve one of the SL_IID_* data symbols by name, for the fake dlsym. */
void *opensles_lookup(const char *name);

#endif

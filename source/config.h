/* Port-wide paths, sizes and tunables. */

#ifndef BSNX_CONFIG_H
#define BSNX_CONFIG_H

/* Everything the port owns lives under one SD directory. */
#define GAME_ROOT      "sdmc:/switch/bse_rebombed_nx"
#define GAME_ROOT_UNIX "/switch/bse_rebombed_nx"

/* The Android shared library, copied out of the player's own APK. This port
 * targets BombSquad Explodinary, which ships its own engine build; see
 * patch.c for the exact one and why the number differs from stock. */
#define SO_PATH GAME_ROOT "/libmain.so"

/* Android storage contract. The engine appends "/ballistica_files" to the
 * no-backup directory, so game assets end up at
 * <NO_BACKUP_PATH>/ballistica_files/{ba_data,pylib,payload_info}. */
#define FILES_PATH     GAME_ROOT "/files"
#define NO_BACKUP_PATH GAME_ROOT "/no_backup"
#define EXTERNAL_PATH  GAME_ROOT "/external"
#define CACHE_PATH     GAME_ROOT "/cache"
#define ASSETS_PATH    NO_BACKUP_PATH "/ballistica_files"

/* Unix-flavoured twins handed to the guest, which must not see "sdmc:". */
#define FILES_PATH_UNIX     GAME_ROOT_UNIX "/files"
#define NO_BACKUP_PATH_UNIX GAME_ROOT_UNIX "/no_backup"
#define EXTERNAL_PATH_UNIX  GAME_ROOT_UNIX "/external"
#define CACHE_PATH_UNIX     GAME_ROOT_UNIX "/cache"

#define PORT_CONFIG_PATH GAME_ROOT "/bse_rebombed_nx.cfg"
#define FATAL_LOG_PATH   GAME_ROOT "/fatal.txt"
#define TRACE_LOG_PATH   GAME_ROOT "/trace.txt"

/* Where an unmodified APK extraction leaves the pieces we need. */
#define APK_LIB_PATH    GAME_ROOT "/lib/arm64-v8a/libmain.so"
#define APK_ASSETS_PATH GAME_ROOT "/assets/ballistica_files"

/* Reserved code-memory window for the guest module. libmain.so needs about
 * 26 MB of mapped image; leave headroom for future game builds. */
#define SO_REGION_BYTES (48u * 1024u * 1024u)

/* Native surface sizes. Docked and handheld differ, and the engine is told
 * about the switch through nativeOnSurfaceChanged. */
#define SURFACE_W_HANDHELD 1280
#define SURFACE_H_HANDHELD 720
#define SURFACE_W_DOCKED   1920
#define SURFACE_H_DOCKED   1080

/* Android device id space. Keyboard/text events do not use one. */
#define TOUCH_DEVICE_ID   0
#define JOY_DEVICE_ID_BASE 1
#define MAX_PADS 8

#endif

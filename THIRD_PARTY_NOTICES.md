# Third-party notices

## Vendored source

### `source/jni.h`

The JNI interface declarations come from the Android Open Source Project and
are licensed under the Apache License, Version 2.0.

```text
Copyright (C) 2006 The Android Open Source Project
Licensed under the Apache License, Version 2.0
http://www.apache.org/licenses/LICENSE-2.0
```

Only the header is used. Nothing in this port implements the Android runtime;
`source/jni_env.c` is an original in-process stand-in that answers the subset
of JNI the game actually calls.

### `source/so_util.c` and `source/so_util.h`

Adapted from the MIT-licensed Android so-loader by TheOfficialFloW
(Andy Nguyen) and the Switch port of it by fgsfds.

```text
Copyright (C) 2021 Andy Nguyen, fgsfds
MIT License
```

Local changes: absolute-branch hooking for AArch64, staging-buffer symbol
lookup, and a stricter architecture check.

### `source/opensles.c`

The OpenSL ES object model follows the structure of fgsfds' MIT-licensed
Switch so-loader work. The mixer is written for this port.

## Linked libraries

Provided by devkitPro and linked at build time, not redistributed here:

| Library | License |
| --- | --- |
| libnx | ISC |
| newlib | BSD-style, per file |
| Mesa (EGL, GLESv2, nouveau) | MIT |
| SDL2 | Zlib |
| FreeType | FTL or GPLv2 |
| HarfBuzz | MIT |
| libpng | PNG Reference Library License |
| zlib | Zlib |

## Not included

BombSquad and the Ballistica engine are the work of Eric Froemling. This
repository contains no part of either: no `libmain.so`, no `ba_data`, no
`pylib`, no APK. Players supply those from their own copy of the game.

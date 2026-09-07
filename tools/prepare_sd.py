#!/usr/bin/env python3
"""Build the SD-card folder for bombsquad_nx from your own BombSquad APK.

Usage:
    python3 tools/prepare_sd.py BombSquad_Android_Generic_1.7.62.apk out/

Copy the contents of the output directory to /switch/bombsquad_nx/ on your
SD card, alongside bombsquad_nx.nro.

Nothing from the game is redistributed with this port: the APK is yours and
stays yours.
"""

import argparse
import hashlib
import os
import shutil
import struct
import sys
import zipfile

LIB_MEMBER = "lib/arm64-v8a/libmain.so"
ASSET_PREFIX = "assets/ballistica_files/"

# The supported build, checked in the extracted library rather than in the
# APK, so re-packed but identical downloads still work.
SUPPORTED_BUILD = 22837
BUILD_NUMBER_RVA = 0x4F09FC
KNOWN_LIB_SHA256 = "afdf7c97d52a419de38c3d82c7dfabb055a6d37e920cbcc7233bc5b00aaf0103"

REQUIRED_ASSETS = ("ba_data", "pylib", "payload_info")


def check_library(path):
    with open(path, "rb") as handle:
        head = handle.read(24)
        if head[:4] != b"\x7fELF":
            raise SystemExit("error: %s is not an ELF file" % path)
        if head[4] != 2:
            raise SystemExit("error: %s is 32-bit; extract lib/arm64-v8a" % path)
        machine = struct.unpack_from("<H", head, 18)[0]
        if machine != 183:  # EM_AARCH64
            raise SystemExit("error: %s is not an AArch64 library" % path)

        handle.seek(BUILD_NUMBER_RVA)
        build = struct.unpack("<i", handle.read(4))[0]

    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    checksum = digest.hexdigest()

    if build != SUPPORTED_BUILD:
        raise SystemExit(
            "error: this is engine build %d; the port supports build %d "
            "(BombSquad 1.7.62)" % (build, SUPPORTED_BUILD))

    print("  engine build %d" % build)
    if checksum == KNOWN_LIB_SHA256:
        print("  library checksum matches the reference build")
    else:
        print("  note: library checksum %s differs from the reference build,\n"
              "        but the engine build number matches" % checksum[:16])


def verify_against_manifest(assets_root):
    """Check the extracted assets against the game's own payload_info.

    The game ships an MD5 for most of its data files, so a truncated or
    corrupted copy can be caught here rather than showing up later as a
    baffling runtime error.
    """
    manifest = os.path.join(assets_root, "payload_info")
    if not os.path.exists(manifest):
        return

    checked = 0
    bad = []
    with open(manifest, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 2 or len(parts[1]) != 32:
                continue
            relative, expected = parts
            path = os.path.join(assets_root, relative)
            if not os.path.exists(path):
                bad.append((relative, "missing"))
                continue
            digest = hashlib.md5()
            with open(path, "rb") as data:
                for chunk in iter(lambda: data.read(1 << 20), b""):
                    digest.update(chunk)
            checked += 1
            if digest.hexdigest() != expected:
                bad.append((relative, "checksum mismatch"))

    print("Verified %d files against payload_info" % checked)
    for relative, why in bad:
        print("  WARNING: %s (%s)" % (relative, why))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apk", help="your BombSquad 1.7.62 Android APK")
    parser.add_argument("output", help="directory to build (created if absent)")
    parser.add_argument("--force", action="store_true",
                        help="overwrite an existing output directory")
    args = parser.parse_args()

    if not zipfile.is_zipfile(args.apk):
        raise SystemExit("error: %s is not an APK" % args.apk)

    out = os.path.abspath(args.output)
    if os.path.exists(out) and os.listdir(out):
        if not args.force:
            raise SystemExit(
                "error: %s already exists and is not empty (use --force)" % out)
        shutil.rmtree(out)
    os.makedirs(out, exist_ok=True)

    print("Reading %s" % args.apk)
    with zipfile.ZipFile(args.apk) as apk:
        names = set(apk.namelist())
        if LIB_MEMBER not in names:
            raise SystemExit("error: %s has no %s" % (args.apk, LIB_MEMBER))

        lib_path = os.path.join(out, "libmain.so")
        with apk.open(LIB_MEMBER) as source, open(lib_path, "wb") as target:
            shutil.copyfileobj(source, target, 1 << 20)
        print("Extracted libmain.so")
        check_library(lib_path)

        assets_root = os.path.join(out, "no_backup", "ballistica_files")
        os.makedirs(assets_root, exist_ok=True)

        members = [n for n in names if n.startswith(ASSET_PREFIX) and not n.endswith("/")]
        if not members:
            raise SystemExit("error: %s has no %s" % (args.apk, ASSET_PREFIX))

        print("Extracting %d asset files" % len(members))
        for index, name in enumerate(sorted(members)):
            relative = name[len(ASSET_PREFIX):]
            destination = os.path.join(assets_root, relative)
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            with apk.open(name) as source, open(destination, "wb") as target:
                shutil.copyfileobj(source, target, 1 << 20)
            if index % 500 == 0:
                print("  %d/%d" % (index, len(members)))

    verify_against_manifest(assets_root)

    for required in REQUIRED_ASSETS:
        if not os.path.exists(os.path.join(assets_root, required)):
            raise SystemExit("error: the APK is missing assets/ballistica_files/%s"
                             % required)

    for writable in ("files", "external", "cache"):
        os.makedirs(os.path.join(out, writable), exist_ok=True)

    print()
    print("Done. Copy the contents of")
    print("  %s" % out)
    print("to /switch/bombsquad_nx/ on your SD card, next to bombsquad_nx.nro.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

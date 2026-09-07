#!/usr/bin/env python3
"""Check source/imports.c against a game library.

Every symbol the library imports must have a binding, and every binding must
correspond to a real import. Run this after regenerating, and in CI.
"""

import os
import re
import subprocess
import sys


def library_imports(path):
    readelf = os.environ.get("READELF", "aarch64-none-elf-readelf")
    raw = subprocess.check_output([readelf, "-W", "--dyn-syms", path], text=True)
    names = set()
    for line in raw.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[6] == "UND":
            name = parts[7].split("@")[0]
            if name:
                names.add(name)
    return names


def table_bindings(path):
    with open(path) as handle:
        text = handle.read()
    body = text.split("DynLibFunction dynlib_functions[] = {", 1)
    if len(body) != 2:
        raise SystemExit("could not find the binding table in %s" % path)
    body = body[1].split("\n};", 1)[0]
    return dict(re.findall(r'\{"([^"]+)",\s*([^}]+)\}', body))


def main():
    if len(sys.argv) < 2:
        print("usage: verify_imports.py <libmain.so> [imports.c]", file=sys.stderr)
        return 2
    lib = sys.argv[1]
    table_path = sys.argv[2] if len(sys.argv) > 2 else "source/imports.c"

    imports = library_imports(lib)
    bindings = table_bindings(table_path)

    missing = sorted(imports - set(bindings))
    extra = sorted(set(bindings) - imports)
    empty = sorted(name for name, target in bindings.items() if not target.strip())

    ok = True
    if missing:
        ok = False
        print("MISSING bindings (%d):" % len(missing))
        for name in missing:
            print("  " + name)
    if extra:
        ok = False
        print("STALE bindings not imported by this library (%d):" % len(extra))
        for name in extra:
            print("  " + name)
    if empty:
        ok = False
        print("EMPTY bindings (%d): %s" % (len(empty), ", ".join(empty)))

    if ok:
        print("OK: %d imports, all bound." % len(imports))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Generate the SQLite amalgamation (sqlite3.c + sqlite3.h) from the SQLite
source tree that is the parent of this directory, then copy the result
together with sqlite3_stdio.{c,h} into sqldiff/sqlite_src/.

Usage
-----
    python generate_amalgamation.py          # default: use sibling source tree
    python generate_amalgamation.py /path/to/sqlite-src

The script must be run from the sqldiff_python/ directory (or any location
where it can find the SQLite source tree).

Prerequisites
-------------
  - A C compiler (cc / gcc / clang) on PATH
  - tclsh on PATH
  - The standard Unix build tools (make, etc.)

The generated files are written to sqldiff/sqlite_src/ and should be
committed to the repository so that wheels can be built without needing
the full SQLite source tree in CI.
"""

import os
import sys
import shutil
import subprocess
import tempfile

_HERE     = os.path.dirname(os.path.abspath(__file__))
_DST      = os.path.join(_HERE, "sqldiff", "sqlite_src")


def find_sqlite_root(argv):
    """Return the path to the SQLite source tree root."""
    if len(argv) > 1:
        root = os.path.abspath(argv[1])
    else:
        # Assume this script lives inside <sqlite-root>/sqldiff_python/
        root = os.path.dirname(_HERE)

    required = [
        os.path.join(root, "src", "main.c"),
        os.path.join(root, "tool", "mksqlite3c.tcl"),
        os.path.join(root, "configure"),
    ]
    for path in required:
        if not os.path.exists(path):
            sys.exit(
                f"ERROR: {path!r} not found.\n"
                "Please pass the path to the SQLite source tree as an argument:\n"
                "    python generate_amalgamation.py /path/to/sqlite-src"
            )
    return root


def check_tool(name):
    if shutil.which(name) is None:
        sys.exit(f"ERROR: {name!r} is not on PATH.  Please install it.")


def run(*cmd, cwd=None):
    print("+", " ".join(str(c) for c in cmd))
    subprocess.check_call(list(cmd), cwd=cwd)


def main():
    sqlite_root = find_sqlite_root(sys.argv)
    print(f"SQLite source root : {sqlite_root}")
    print(f"Destination        : {_DST}")
    print()

    for tool in ("tclsh",):
        check_tool(tool)

    os.makedirs(_DST, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="sqldiff_amalg_") as build_dir:
        print(f"Build directory    : {build_dir}")
        print()

        # Configure
        configure = os.path.join(sqlite_root, "configure")
        run(configure, "--quiet", cwd=build_dir)

        # Build the amalgamation target
        run("make", "sqlite3.c", cwd=build_dir)

        for name in ("sqlite3.c", "sqlite3.h"):
            src  = os.path.join(build_dir, name)
            dst  = os.path.join(_DST, name)
            shutil.copy2(src, dst)
            size = os.path.getsize(dst)
            print(f"Copied {name:12s}  ({size:,} bytes)")

    # Copy sqldiff.c from tool/ and sqlite3_stdio.{c,h} from ext/misc/
    extras = [
        (os.path.join(sqlite_root, "tool", "sqldiff.c"),        "sqldiff.c"),
        (os.path.join(sqlite_root, "ext", "misc", "sqlite3_stdio.c"), "sqlite3_stdio.c"),
        (os.path.join(sqlite_root, "ext", "misc", "sqlite3_stdio.h"), "sqlite3_stdio.h"),
    ]
    for src, name in extras:
        dst = os.path.join(_DST, name)
        shutil.copy2(src, dst)
        size = os.path.getsize(dst)
        print(f"Copied {name:16s}  ({size:,} bytes)")

    print()
    print("Done.  Files in sqldiff/sqlite_src/:")
    for f in sorted(os.listdir(_DST)):
        print(f"  {f}")


if __name__ == "__main__":
    main()

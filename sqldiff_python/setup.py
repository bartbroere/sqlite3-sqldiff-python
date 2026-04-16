"""
Build script for the sqldiff Python extension.

Self-contained build
--------------------
The extension bundles the SQLite amalgamation (sqlite3.c / sqlite3.h) so
that no system SQLite library is required at build or runtime.  The
vendored amalgamation lives in sqldiff/sqlite_src/ and is compiled
directly into the shared library.

On Windows, sqlite3_stdio.c (also in sqlite_src/) is compiled in too,
providing UTF-8-safe stdio helpers used by sqldiff.

Regenerating the amalgamation
------------------------------
If you need to update the bundled SQLite (e.g. after pulling new commits
from the SQLite source tree), run:

    python generate_amalgamation.py

from this directory.  That script produces sqldiff/sqlite_src/sqlite3.c
and sqlite3.h from the sibling SQLite source tree.
"""

import os
import sys
import platform
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext as _build_ext

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

_HERE = os.path.dirname(os.path.abspath(__file__))
_SQLITE_SRC = os.path.join(_HERE, "sqldiff", "sqlite_src")
_AMALGAMATION = os.path.join(_SQLITE_SRC, "sqlite3.c")


# ---------------------------------------------------------------------------
# Guard: make sure the amalgamation is present before trying to build.
# ---------------------------------------------------------------------------

class build_ext(_build_ext):
    def run(self):
        if not os.path.isfile(_AMALGAMATION):
            raise RuntimeError(
                "The SQLite amalgamation is missing.\n"
                "Run  python generate_amalgamation.py  to generate it, or\n"
                "copy a pre-built sqlite3.c / sqlite3.h into:\n"
                f"  {_SQLITE_SRC}"
            )
        super().run()


# ---------------------------------------------------------------------------
# Extension sources
# ---------------------------------------------------------------------------

sources = [
    "sqldiff/_sqldiffmodule.c",
    # Compile the SQLite amalgamation directly into the extension — no
    # system libsqlite3 needed.
    "sqldiff/sqlite_src/sqlite3.c",
]

# On Windows, sqlite3_stdio.c provides UTF-8-safe wrappers for the C
# stdio functions used by sqldiff.  On all other platforms the header
# maps them to the standard library via preprocessor macros, so the .c
# file is not needed.
if sys.platform == "win32":
    sources.append("sqldiff/sqlite_src/sqlite3_stdio.c")

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

compile_args = []
define_macros = [
    # Build SQLite without the TCL interface and test-only features.
    ("SQLITE_OMIT_DEPRECATED",   None),
    # Enable the session / changeset extension used by changeset_one_table.
    ("SQLITE_ENABLE_SESSION",    None),
    ("SQLITE_ENABLE_PREUPDATE_HOOK", None),
]

if sys.platform != "win32":
    compile_args += ["-std=c11", "-O2"]
    # Suppress the sign-compare warning that comes from sqldiff.c itself.
    compile_args += ["-Wno-sign-compare", "-Wno-unused-but-set-variable"]
else:
    # MSVC
    compile_args += ["/O2"]

# ---------------------------------------------------------------------------
# Extension definition
# ---------------------------------------------------------------------------

ext = Extension(
    name="sqldiff._sqldiff",
    sources=sources,
    include_dirs=[_SQLITE_SRC],
    define_macros=define_macros,
    extra_compile_args=compile_args,
)

# ---------------------------------------------------------------------------
# setup()
# ---------------------------------------------------------------------------

setup(
    name="sqldiff",
    version="0.1.0",
    description="Python bindings for the SQLite sqldiff utility",
    packages=["sqldiff"],
    package_data={"sqldiff": ["sqlite_src/*.h", "sqlite_src/*.c"]},
    ext_modules=[ext],
    cmdclass={"build_ext": build_ext},
    python_requires=">=3.8",
)

"""
Build script for the sqldiff Python extension.

The C extension includes ../../tool/sqldiff.c directly, so the SQLite
source tree must be present at the expected relative path (i.e. this
setup.py must live inside <sqlite-root>/sqldiff_python/).

External dependencies
---------------------
  libsqlite3-dev   (Ubuntu/Debian: apt install libsqlite3-dev)
"""

import os
from setuptools import setup, Extension

# Root of the SQLite source tree (parent of this directory).
_SQLITE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# sqldiff.c includes "sqlite3_stdio.h" which lives in ext/misc/.
_EXT_MISC = os.path.join(_SQLITE_ROOT, "ext", "misc")

ext = Extension(
    name="sqldiff._sqldiff",
    sources=["sqldiff/_sqldiffmodule.c"],
    include_dirs=[_EXT_MISC],
    libraries=["sqlite3"],
    extra_compile_args=["-std=c11"],
)

setup(
    name="sqldiff",
    version="0.1.0",
    description="Python bindings for the SQLite sqldiff utility",
    packages=["sqldiff"],
    ext_modules=[ext],
    python_requires=">=3.8",
)

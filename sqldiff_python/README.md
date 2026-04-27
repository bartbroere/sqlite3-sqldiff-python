# sqlite3-sqldiff

Python bindings for the [SQLite `sqldiff` utility](https://www.sqlite.org/sqldiff.html).

`sqldiff` compares two SQLite databases and produces either SQL statements that
transform the first into the second, or a binary changeset that can be applied
via the SQLite session extension API.  This package wraps that logic as a native
Python extension — no subprocess, no system `libsqlite3-dev`, no shell tool.

The SQLite amalgamation (`sqlite3.c`) is compiled directly into the extension,
so wheels are fully self-contained.

---

## Installation

```
pip install sqlite3-sqldiff
```

Wheels are available for CPython 3.10–3.14 on Linux (x86_64, aarch64),
macOS (x86_64, arm64), and Windows (AMD64, ARM64).

---

## Quick start

```python
import sqldiff

# SQL text that transforms old.db into new.db
sql = sqldiff.diff("old.db", "new.db")
print(sql)

# Binary changeset (bytes) describing the same difference
data = sqldiff.changeset("old.db", "new.db")
```

---

## API reference

### `sqldiff.diff`

```
diff(db1, db2, *, table=None, schema_only=False, primary_key=False,
     vtab=False, rbu=False, summary=False, transaction=False) -> str
```

Return SQL text that would transform the SQLite database at path `db1` into the
one at `db2`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `db1`, `db2` | `str` | Filesystem paths to the two SQLite databases. |
| `table` | `str \| None` | Limit output to a single named table. |
| `schema_only` | `bool` | Show only schema (DDL) differences. |
| `primary_key` | `bool` | Use the schema-defined `PRIMARY KEY` rather than the rowid. |
| `vtab` | `bool` | Handle fts3/4/5 and rtree virtual tables. |
| `rbu` | `bool` | Produce [RBU-format](https://www.sqlite.org/rbu.html) update SQL instead of plain SQL. |
| `summary` | `bool` | Produce a human-readable summary instead of SQL. |
| `transaction` | `bool` | Wrap the output SQL in `BEGIN`/`COMMIT`. |

**Returns** `str` — SQL statements (may be empty if the databases are identical).

**Raises** `RuntimeError` if either path is not a valid SQLite database or
another sqldiff error occurs.

---

### `sqldiff.changeset`

```
changeset(db1, db2, *, table=None) -> bytes
```

Return a binary SQLite changeset describing how `db1` differs from `db2`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `db1`, `db2` | `str` | Filesystem paths to the two SQLite databases. |
| `table` | `str \| None` | Limit the changeset to a single named table. |

**Returns** `bytes` — a binary changeset that can be passed directly to the
SQLite session extension API (e.g. `sqlite3session_changeset_apply`).

**Raises** `RuntimeError` if either path is not a valid SQLite database or
another sqldiff error occurs.

---

## Examples

### Compare all tables, wrap in a transaction

```python
sql = sqldiff.diff("before.db", "after.db", transaction=True)
# "BEGIN TRANSACTION;\n...\nCOMMIT;\n"
```

### Compare a single table

```python
sql = sqldiff.diff("before.db", "after.db", table="orders")
```

### Schema-only diff

```python
ddl = sqldiff.diff("v1.db", "v2.db", schema_only=True)
```

### Get a binary changeset and apply it

```python
import sqlite3

data = sqldiff.changeset("old.db", "new.db")

con = sqlite3.connect("old.db")
# Apply with the standard library's session API (Python 3.12+)
con.deserialize  # ... or use the ctypes / apsw session API
```

---

## Version policy

The package version tracks the SQLite version it was built against (e.g.
`3.54.0`).  A wheel built from SQLite 3.54.0 is published as
`sqlite3-sqldiff==3.54.0`.

---

## Updating for a new SQLite release

This package lives inside the SQLite source tree at `<sqlite-root>/sqldiff_python/`.

### Step 1 — pull new SQLite sources

Update the SQLite checkout in the parent directory (`..`) to the desired release
tag or snapshot.

### Step 2 — regenerate the amalgamation

From the `sqldiff_python/` directory:

```bash
python generate_amalgamation.py
```

This script:

1. Runs `./configure && make sqlite3.c` in a temporary directory (requires
   a C compiler, `make`, and `tclsh` on `PATH`).
2. Copies the resulting `sqlite3.c` and `sqlite3.h` into
   `sqldiff/sqlite_src/`.
3. Copies `tool/sqldiff.c` and `ext/misc/sqlite3_stdio.{c,h}` from the
   parent SQLite tree into `sqldiff/sqlite_src/`.

On Windows (where `tclsh`/`make` may not be available), run the script inside
MSYS2, or copy pre-built files from an official SQLite release archive.

### Step 3 — commit the updated files

```bash
git add sqldiff_python/sqldiff/sqlite_src/
git commit -m "Update SQLite amalgamation to $(cat VERSION)"
```

The committed `sqlite_src/` files are what gets compiled into wheels on all
platforms (especially Windows, where the amalgamation cannot be regenerated
easily in CI).

### Step 4 — build wheels

Push a version tag to trigger the GitHub Actions workflow:

```bash
git tag v3.XX.Y
git push origin v3.XX.Y
```

The workflow at `.github/workflows/build-wheels.yml`:

- Regenerates the amalgamation on Linux and macOS runners (using the committed
  files as a fallback on Windows).
- Builds wheels for CPython 3.10–3.14 on Linux (manylinux2014 + musllinux,
  x86_64 + aarch64), macOS (x86_64 + arm64), and Windows (AMD64 + ARM64)
  via [cibuildwheel](https://cibuildwheel.pypa.io/).
- Publishes all wheels to PyPI automatically (requires a PyPI trusted-publisher
  environment named `pypi` configured in your repository settings).

---

## Building locally

### Linux / macOS

```bash
cd sqldiff_python
python generate_amalgamation.py   # regenerate sqlite3.c if needed
pip install cibuildwheel
cibuildwheel --platform linux     # or macos
```

Wheels land in `wheelhouse/`.

### Windows

Build with the committed `sqlite_src/` files (no amalgamation step needed):

```bash
cd sqldiff_python
pip install cibuildwheel
cibuildwheel --platform windows
```

### Quick development build (current Python only)

```bash
cd sqldiff_python
pip install -e .
python -c "import sqldiff; print(sqldiff.diff.__doc__)"
```

---

## License

SQLite itself is in the public domain.  The Python glue code in this package
(`sqldiff/_sqldiffmodule.c`, `setup.py`, etc.) is also released into the public
domain / [CC0](https://creativecommons.org/publicdomain/zero/1.0/).

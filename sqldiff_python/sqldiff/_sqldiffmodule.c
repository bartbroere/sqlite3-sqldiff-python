/*
** Python C extension providing bindings to the sqldiff utility logic.
**
** Exposes two callables:
**
**   diff(db1, db2, *, table=None, schema_only=False, primary_key=False,
**        vtab=False, rbu=False, summary=False, transaction=False) -> str
**
**   changeset(db1, db2, *, table=None) -> bytes
**
** Both accept filesystem paths to two SQLite databases and return either
** the SQL text that would transform db1 into db2, or a binary changeset
** object describing those differences.
**
** Implementation strategy
** -----------------------
** sqldiff.c is #included directly.  Before the include we:
**
**   1. #define exit(code)  ->  longjmp back to a setjmp guard in the
**      Python wrapper, so that the cmdlineError()/runtimeError() helpers
**      inside sqldiff.c never actually terminate the process.
**
**   2. #define main  ->  an unused symbol, preventing a duplicate
**      definition of main().
**
** Output that the sqldiff functions write to a FILE * is captured via
** open_memstream() and converted to a Python str (diff) or bytes
** (changeset) before being returned to the caller.
**
** Error messages printed by sqldiff to stderr are captured via a
** dup2/pipe trick and surfaced as Python RuntimeError exceptions.
*/

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* =========================================================
 * Error-recovery state (module-level globals, protected by
 * Python's GIL – these functions are not reentrant).
 * ========================================================= */

static jmp_buf   _sd_jmpbuf;
static volatile int _sd_exit_code;

/* Pipe used to capture sqldiff's stderr output ([0]=read, [1]=write). */
static int _sd_err_pipe[2] = {-1, -1};
static int _sd_stderr_save  = -1;   /* saved copy of STDERR_FILENO */

/* =========================================================
 * Redirect exit() and rename main() BEFORE including sqldiff.c
 * ========================================================= */

/* Prevent stdlib's exit() from terminating the process.  The code
 * inside cmdlineError()/runtimeError() in sqldiff.c calls exit(1)
 * after printing a message; we replace that with a longjmp back to
 * the setjmp guard in the Python wrapper. */
#define exit(code) \
    do { _sd_exit_code = (code); \
         longjmp(_sd_jmpbuf, (_sd_exit_code) ? (_sd_exit_code) : 1); \
    } while (0)

/* Rename main() so it doesn't clash with Python's own main(). */
#define main sqldiff_main_unused__

/* sqldiff.c includes "sqlite3_stdio.h" (found via -I in setup.py)
 * and "sqlite3.h" (resolved from the system include path). */
#include "../../tool/sqldiff.c"

#undef exit
#undef main

/* =========================================================
 * Stderr-capture helpers
 * ========================================================= */

/* Redirect fd 2 to a pipe so we can read error messages later.
 * Returns 0 on success, -1 on failure (in which case stderr is
 * unchanged and _sd_stderr_save / _sd_err_pipe stay at -1). */
static int begin_stderr_capture(void)
{
    if (pipe(_sd_err_pipe) < 0)
        return -1;
    _sd_stderr_save = dup(STDERR_FILENO);
    if (_sd_stderr_save < 0) {
        close(_sd_err_pipe[0]); close(_sd_err_pipe[1]);
        _sd_err_pipe[0] = _sd_err_pipe[1] = -1;
        return -1;
    }
    if (dup2(_sd_err_pipe[1], STDERR_FILENO) < 0) {
        close(_sd_stderr_save); _sd_stderr_save = -1;
        close(_sd_err_pipe[0]); close(_sd_err_pipe[1]);
        _sd_err_pipe[0] = _sd_err_pipe[1] = -1;
        return -1;
    }
    close(_sd_err_pipe[1]);
    _sd_err_pipe[1] = -1;
    return 0;
}

/* Restore fd 2 from the saved copy and read whatever was written to
 * the pipe.  If buf is non-NULL, up to bufsz-1 bytes are copied there
 * and NUL-terminated; trailing whitespace is stripped. */
static void end_stderr_capture(char *buf, size_t bufsz)
{
    fflush(stderr);
    if (_sd_stderr_save >= 0) {
        dup2(_sd_stderr_save, STDERR_FILENO);
        close(_sd_stderr_save);
        _sd_stderr_save = -1;
    }
    if (_sd_err_pipe[0] >= 0) {
        if (buf && bufsz > 0) {
            /* Make non-blocking so we can drain without hanging. */
            int fl = fcntl(_sd_err_pipe[0], F_GETFL, 0);
            fcntl(_sd_err_pipe[0], F_SETFL, fl | O_NONBLOCK);
            ssize_t n = read(_sd_err_pipe[0], buf, (ssize_t)(bufsz - 1));
            if (n < 0) n = 0;
            buf[n] = '\0';
            /* Strip trailing newlines / spaces. */
            while (n > 0 &&
                   (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
                buf[--n] = '\0';
        }
        close(_sd_err_pipe[0]);
        _sd_err_pipe[0] = -1;
    }
}

/* =========================================================
 * Global-state management
 * ========================================================= */

/* Close any open database and zero the sqldiff global struct. */
static void reset_globals(void)
{
    if (g.db) {
        sqlite3_close(g.db);
        g.db = NULL;
    }
    memset(&g, 0, sizeof(g));
    g.zArgv0 = "sqldiff";
}

/* =========================================================
 * Error-path helper: raise RuntimeError from captured stderr.
 * Strips the leading "sqldiff: " prefix that sqldiff always adds.
 * ========================================================= */
static void raise_from_errbuf(const char *errbuf)
{
    const char *msg = errbuf;
    if (strncmp(msg, "sqldiff: ", 9) == 0) msg += 9;
    if (*msg)
        PyErr_SetString(PyExc_RuntimeError, msg);
    else
        PyErr_Format(PyExc_RuntimeError,
                     "sqldiff exited with code %d", _sd_exit_code);
}

/* =========================================================
 * open_db: open db1 as g.db (READONLY) and attach db2 as "aux".
 * Both runtimeError() calls here will longjmp on failure.
 * ========================================================= */
static void open_db(const char *zDb1, const char *zDb2)
{
    int rc;
    char *zErrMsg = NULL;

    rc = sqlite3_open_v2(zDb1, &g.db, SQLITE_OPEN_READONLY, 0);
    if (rc)
        runtimeError("cannot open database file \"%s\"", zDb1);

    rc = sqlite3_exec(g.db, "SELECT * FROM sqlite_schema", 0, 0, &zErrMsg);
    if (rc || zErrMsg)
        runtimeError("\"%s\" does not appear to be a valid SQLite database",
                     zDb1);

    /* Verify db2 is readable before attaching. */
    {
        sqlite3 *db2 = NULL;
        rc = sqlite3_open_v2(zDb2, &db2, SQLITE_OPEN_READONLY, 0);
        if (rc) runtimeError("cannot open database file \"%s\"", zDb2);
        sqlite3_close(db2);
    }

    char *zSql = sqlite3_mprintf("ATTACH %Q AS aux;", zDb2);
    rc = sqlite3_exec(g.db, zSql, 0, 0, &zErrMsg);
    sqlite3_free(zSql);
    if (rc || zErrMsg)
        runtimeError("cannot attach database \"%s\"", zDb2);

    rc = sqlite3_exec(g.db, "SELECT * FROM aux.sqlite_schema",
                      0, 0, &zErrMsg);
    if (rc || zErrMsg)
        runtimeError("\"%s\" does not appear to be a valid SQLite database",
                     zDb2);
}

/* =========================================================
 * run_diff: run a text-output diff and return a Python str.
 * ========================================================= */
static PyObject *run_diff(
    const char *zDb1, const char *zDb2, const char *zTab,
    int schema_only, int primary_key, int vtab,
    int rbu, int summary, int transaction)
{
    char *out_buf  = NULL;
    size_t out_sz  = 0;
    FILE  *out     = open_memstream(&out_buf, &out_sz);
    if (!out) { PyErr_SetFromErrno(PyExc_OSError); return NULL; }

    begin_stderr_capture();
    _sd_exit_code = 0;
    reset_globals();

    if (setjmp(_sd_jmpbuf)) {
        /* --- error recovery --- */
        char errbuf[4096];
        end_stderr_capture(errbuf, sizeof(errbuf));
        fclose(out);
        free(out_buf);
        reset_globals();
        raise_from_errbuf(errbuf);
        return NULL;
    }

    /* Configure sqldiff globals. */
    g.bSchemaOnly = schema_only;
    g.bSchemaPK   = primary_key;
    g.bHandleVtab = vtab;
    if (zTab) {
        g.bSchemaCompare =
            sqlite3_stricmp(zTab, "sqlite_schema") == 0 ||
            sqlite3_stricmp(zTab, "sqlite_master") == 0;
    }

    open_db(zDb1, zDb2);

    /* Choose the diff function. */
    void (*xDiff)(const char *, FILE *) = diff_one_table;
    if (summary) xDiff = summarize_one_table;
    else if (rbu) xDiff = rbudiff_one_table;

    if (transaction)
        sqlite3_fprintf(out, "BEGIN TRANSACTION;\n");
    if (rbu)
        sqlite3_fprintf(out,
            "CREATE TABLE IF NOT EXISTS rbu_count"
            "(tbl TEXT PRIMARY KEY COLLATE NOCASE, cnt INTEGER)"
            " WITHOUT ROWID;\n");

    if (zTab) {
        xDiff(zTab, out);
    } else {
        sqlite3_stmt *pStmt = db_prepare("%s", all_tables_sql());
        while (SQLITE_ROW == sqlite3_step(pStmt))
            xDiff((const char *)sqlite3_column_text(pStmt, 0), out);
        sqlite3_finalize(pStmt);
    }

    if (transaction)
        sqlite3_fprintf(out, "COMMIT;\n");

    sqlite3_close(g.db);
    memset(&g, 0, sizeof(g));

    end_stderr_capture(NULL, 0);
    fclose(out);

    PyObject *result = PyUnicode_FromStringAndSize(out_buf, (Py_ssize_t)out_sz);
    free(out_buf);
    return result;
}

/* =========================================================
 * run_changeset: run a binary changeset diff and return bytes.
 * ========================================================= */
static PyObject *run_changeset(
    const char *zDb1, const char *zDb2, const char *zTab)
{
    char *out_buf = NULL;
    size_t out_sz = 0;
    FILE  *out    = open_memstream(&out_buf, &out_sz);
    if (!out) { PyErr_SetFromErrno(PyExc_OSError); return NULL; }

    begin_stderr_capture();
    _sd_exit_code = 0;
    reset_globals();

    if (setjmp(_sd_jmpbuf)) {
        char errbuf[4096];
        end_stderr_capture(errbuf, sizeof(errbuf));
        fclose(out);
        free(out_buf);
        reset_globals();
        raise_from_errbuf(errbuf);
        return NULL;
    }

    open_db(zDb1, zDb2);

    if (zTab) {
        changeset_one_table(zTab, out);
    } else {
        sqlite3_stmt *pStmt = db_prepare("%s", all_tables_sql());
        while (SQLITE_ROW == sqlite3_step(pStmt))
            changeset_one_table(
                (const char *)sqlite3_column_text(pStmt, 0), out);
        sqlite3_finalize(pStmt);
    }

    sqlite3_close(g.db);
    memset(&g, 0, sizeof(g));

    end_stderr_capture(NULL, 0);
    fclose(out);

    PyObject *result = PyBytes_FromStringAndSize(out_buf, (Py_ssize_t)out_sz);
    free(out_buf);
    return result;
}

/* =========================================================
 * Python-callable wrappers
 * ========================================================= */

PyDoc_STRVAR(diff_doc,
"diff(db1, db2, *, table=None, schema_only=False, primary_key=False,\n"
"     vtab=False, rbu=False, summary=False, transaction=False) -> str\n"
"\n"
"Return SQL text that would transform the SQLite database at path *db1*\n"
"into the one at *db2*.\n"
"\n"
"Parameters\n"
"----------\n"
"db1, db2     : str  Filesystem paths to the two SQLite databases.\n"
"table        : str  Limit output to a single named table.\n"
"schema_only  : bool Show only schema (DDL) differences.\n"
"primary_key  : bool Use the schema-defined PRIMARY KEY rather than\n"
"                    the true (rowid) primary key.\n"
"vtab         : bool Handle fts3/4/5 and rtree virtual tables.\n"
"rbu          : bool Produce RBU-format update SQL instead of plain SQL.\n"
"summary      : bool Produce a human-readable summary instead of SQL.\n"
"transaction  : bool Wrap the output SQL in BEGIN/COMMIT.\n"
);

static PyObject *
py_diff(PyObject *self, PyObject *args, PyObject *kwargs)
{
    const char *db1, *db2;
    const char *table     = NULL;
    int schema_only       = 0;
    int primary_key       = 0;
    int vtab              = 0;
    int rbu               = 0;
    int summary           = 0;
    int transaction       = 0;

    static char *kwlist[] = {
        "db1", "db2", "table",
        "schema_only", "primary_key", "vtab",
        "rbu", "summary", "transaction",
        NULL
    };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "ss|zpppppp:diff", kwlist,
            &db1, &db2, &table,
            &schema_only, &primary_key, &vtab,
            &rbu, &summary, &transaction))
        return NULL;

    return run_diff(db1, db2, table,
                    schema_only, primary_key, vtab,
                    rbu, summary, transaction);
}

PyDoc_STRVAR(changeset_doc,
"changeset(db1, db2, *, table=None) -> bytes\n"
"\n"
"Return a binary SQLite changeset describing how *db1* differs from\n"
"*db2*.  The bytes object can be passed directly to the SQLite session\n"
"extension API (e.g. sqlite3session_changeset_apply).\n"
"\n"
"Parameters\n"
"----------\n"
"db1, db2 : str  Filesystem paths to the two SQLite databases.\n"
"table    : str  Limit the changeset to a single named table.\n"
);

static PyObject *
py_changeset(PyObject *self, PyObject *args, PyObject *kwargs)
{
    const char *db1, *db2;
    const char *table = NULL;

    static char *kwlist[] = {"db1", "db2", "table", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "ss|z:changeset", kwlist, &db1, &db2, &table))
        return NULL;

    return run_changeset(db1, db2, table);
}

/* =========================================================
 * Module definition
 * ========================================================= */

PyDoc_STRVAR(module_doc,
"Low-level bindings to the SQLite sqldiff utility.\n"
"\n"
"Use sqldiff.diff() to get SQL and sqldiff.changeset() for binary\n"
"changesets.  See the individual function docstrings for details.\n"
);

static PyMethodDef sqldiff_methods[] = {
    {"diff",      (PyCFunction)py_diff,
     METH_VARARGS | METH_KEYWORDS, diff_doc},
    {"changeset", (PyCFunction)py_changeset,
     METH_VARARGS | METH_KEYWORDS, changeset_doc},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef sqldiff_moduledef = {
    PyModuleDef_HEAD_INIT,
    "_sqldiff",
    module_doc,
    -1,
    sqldiff_methods
};

PyMODINIT_FUNC
PyInit__sqldiff(void)
{
    return PyModule_Create(&sqldiff_moduledef);
}

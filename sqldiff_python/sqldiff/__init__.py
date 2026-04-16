"""
sqldiff – Python bindings for the SQLite sqldiff utility.

Quick start
-----------
>>> import sqldiff
>>> sql = sqldiff.diff("old.db", "new.db")
>>> data = sqldiff.changeset("old.db", "new.db")

Both functions accept filesystem paths to two SQLite databases.

diff()       returns a str containing SQL that transforms the first
             database into the second.

changeset()  returns bytes containing a binary SQLite changeset that
             can be fed to the session-extension apply API.
"""

from ._sqldiff import diff, changeset

__all__ = ["diff", "changeset"]

"""Optional exact, bounded, disposable history for the canonical trace reader.

This index is not resumable evidence. Any storage error invalidates validation;
no existing database is opened and no approximation or memory fallback exists.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import sqlite3

PAGE_BYTES = 4096
CACHE_KIB = 8192
BATCH_WRITES = 4096


class HistoryStorageError(ValueError):
    def __init__(self, message, evidence=None):
        super().__init__(message)
        self.history_evidence = evidence


def history_options(path=None, records=None, byte_limit=None):
    values = (path, records, byte_limit)
    if all(value is None for value in values):
        return None
    if any(value is None for value in values):
        raise ValueError("disk history requires path, record budget and byte budget together")
    if any(type(value) is not int or value <= 0 for value in (records, byte_limit)):
        raise ValueError("disk history budgets must be positive integers")
    if not isinstance(path, (str, os.PathLike)) or not Path(path).is_absolute():
        raise ValueError("disk history path must be an explicit absolute fresh path")
    return dict(path=Path(path), max_records=records, max_bytes=byte_limit)


def _key(value):
    if type(value) is not int or value < 0:
        raise ValueError("history identity must be an exact nonnegative integer")
    return str(value).encode("ascii")


class _Tasks:
    def __init__(self, owner):
        self.owner = owner

    def __len__(self):
        return self.owner.task_count

    def __contains__(self, key):
        return self.owner._one("SELECT 1 FROM tasks WHERE identity=?", (_key(key),)) is not None

    def get(self, key, default=None):
        row = self.owner._one("SELECT kind,name,stamp,parent FROM tasks WHERE identity=?", (_key(key),))
        return default if row is None else (row[0], row[1], json.loads(row[2]), int(row[3]))

    def __setitem__(self, key, value):
        kind, name, timestamp, parent = value
        self.owner._admit()
        self.owner._write("INSERT INTO tasks(identity,kind,name,stamp,parent,completed) VALUES(?,?,?,?,?,0)",
                          (_key(key), kind, name, json.dumps(timestamp, allow_nan=False), _key(parent)), record="task")

    def items(self):
        cursor = self.owner._execute("SELECT identity,kind,name,stamp,parent FROM tasks ORDER BY ordinal")
        try:
            for key, kind, name, stamp, parent in cursor:
                yield int(key), (kind, name, json.loads(stamp), int(parent))
        except (sqlite3.Error, OSError) as error:
            self.owner._fail(error)
        finally:
            self.owner._close_cursor(cursor)


class _Membership:
    def __init__(self, owner, sessions=False):
        self.owner, self.sessions = owner, sessions
        self.max_records = owner.max_records

    def __len__(self):
        return self.owner.session_count if self.sessions else self.owner.completed_count

    def __contains__(self, key):
        sql = ("SELECT 1 FROM sessions WHERE identity=?" if self.sessions else
               "SELECT 1 FROM tasks WHERE identity=? AND completed=1")
        return self.owner._one(sql, (_key(key),)) is not None

    def add(self, key):
        if self.sessions:
            self.owner._admit()
            self.owner._write("INSERT INTO sessions(identity) VALUES(?)", (_key(key),), record="session")
        else:
            changed = self.owner._write("UPDATE tasks SET completed=1 WHERE identity=? AND completed=0", (_key(key),), record="completed")
            if changed != 1:
                self.owner._fail("missing or repeated completed service identity")


class SQLiteHistory:
    """One invocation owns one fresh scratch DB; RAM holds no identity mirror."""
    def __init__(self, path, max_records, max_bytes):
        options = history_options(path, max_records, max_bytes)
        self.path, self.max_records, self.max_bytes = options.values()
        self.connection = self.fd = None
        self.created = self.closed = self.failed = False
        self.connection_close_returned = self.fd_close_returned = True
        self.close_errors = []
        self.task_count = self.session_count = self.completed_count = self.pending_writes = 0
        self.file_bytes = self.max_pages = 0
        self.identity = None
        self.tasks, self.completed_services, self.terminal_sessions = _Tasks(self), _Membership(self), _Membership(self, True)
        try:
            if max_bytes < PAGE_BYTES:
                self._fail("disk history byte budget cannot hold one page")
            for candidate in (self.path, *(Path(str(self.path) + suffix) for suffix in ("-journal", "-wal", "-shm"))):
                if candidate.exists() or candidate.is_symlink():
                    self._fail("disk history path or sidecar already exists; preserve it")
            self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
            self.created = True
            self.fd_close_returned = False
            info = os.fstat(self.fd)
            self.identity = (info.st_dev, info.st_ino)
            self.connection = sqlite3.connect(self.path.as_uri() + "?mode=rw", uri=True, timeout=0, isolation_level=None)
            self.connection_close_returned = False
            self._execute("PRAGMA page_size=4096")
            if self._one("PRAGMA journal_mode=OFF")[0].lower() != "off":
                self._fail("disk history could not disable scratch journaling")
            self._execute("PRAGMA synchronous=OFF")
            self._execute("PRAGMA mmap_size=0")
            self._execute("PRAGMA cache_size=-8192")
            self._execute("PRAGMA cache_spill=ON")
            self._execute("PRAGMA temp_store=FILE")
            self.max_pages = self._one("PRAGMA max_page_count=" + str(max_bytes // PAGE_BYTES))[0]
            if not 0 < self.max_pages <= max_bytes // PAGE_BYTES:
                self._fail("disk history page budget was not enforced")
            self._execute("CREATE TABLE tasks(ordinal INTEGER PRIMARY KEY,identity BLOB UNIQUE NOT NULL,kind INTEGER NOT NULL,name TEXT NOT NULL,stamp TEXT NOT NULL,parent BLOB NOT NULL,completed INTEGER NOT NULL)")
            self._execute("CREATE TABLE sessions(identity BLOB PRIMARY KEY) WITHOUT ROWID")
            self._execute("BEGIN")
        except (sqlite3.Error, OSError, ValueError) as error:
            self.failed = True
            try:
                self.close(commit=False)
            except HistoryStorageError:
                pass
            raise HistoryStorageError(str(error), self.evidence()) from error

    def evidence(self):
        # Cached scalar accounting only: safe after any DB error/close.
        return dict(backend="sqlite-disposable-exact", path=str(self.path), created=self.created,
                    closed=self.closed, failed=self.failed, connection_close_returned=self.connection_close_returned,
                    fd_close_returned=self.fd_close_returned, close_errors=list(self.close_errors),
                    count_scope="SQL-accepted rows/updates; not durable after failure", task_records=self.task_count,
                    terminal_session_records=self.session_count, completed_services=self.completed_count,
                    records=self.task_count + self.session_count, max_records=self.max_records,
                    max_bytes=self.max_bytes, database_bytes=self.file_bytes,
                    page_bytes=PAGE_BYTES, max_pages=self.max_pages, cache_kib=CACHE_KIB,
                    batch_writes=BATCH_WRITES, mmap_bytes=0, journal_mode="off",
                    durability="scratch only; errors/crash invalidate; never reopen as evidence")

    def _fail(self, error):
        self.failed = True
        raise HistoryStorageError("disk history storage failure: " + str(error), self.evidence())

    def _execute(self, sql, parameters=()):
        if self.failed or self.closed:
            self._fail("index unavailable after failure/close")
        try:
            return self.connection.execute(sql, parameters)
        except (sqlite3.Error, OSError) as error:
            self._fail(error)

    def _one(self, sql, parameters=()):
        cursor = self._execute(sql, parameters)
        try:
            return cursor.fetchone()
        except (sqlite3.Error, OSError) as error:
            self._fail(error)
        finally:
            self._close_cursor(cursor)

    def _close_cursor(self, cursor):
        try:
            cursor.close()
        except (sqlite3.Error, OSError) as error:
            self._fail(error)

    def _admit(self):
        if self.task_count + self.session_count >= self.max_records:
            self._fail("disk history record limit exceeded")

    def _write(self, sql, parameters, record):
        cursor = self._execute(sql, parameters)
        changed = cursor.rowcount
        if changed == 1:
            if record == "task": self.task_count += 1
            elif record == "session": self.session_count += 1
            else: self.completed_count += 1
        self._close_cursor(cursor)
        self.pending_writes += 1
        if self.pending_writes >= BATCH_WRITES:
            self._execute("COMMIT")
            self.pending_writes = 0
            self._execute("BEGIN")
        return changed

    def close(self, commit=True):
        if self.closed:
            return
        error = None
        try:
            if self.connection is not None and commit and not self.failed:
                self._execute("COMMIT")
        except (sqlite3.Error, OSError, HistoryStorageError) as caught:
            error = caught
        finally:
            if self.connection is not None:
                for _attempt in range(2):
                    try:
                        self.connection.close()
                        self.connection = None
                        self.connection_close_returned = True
                        break
                    except (sqlite3.Error, OSError) as caught:
                        self.close_errors.append(str(caught))
                        error = error or caught
            if self.fd is not None:
                try:
                    info = os.fstat(self.fd)
                    self.file_bytes = info.st_size
                    current = self.path.stat()
                    if self.identity != (current.st_dev, current.st_ino) or self.identity != (info.st_dev, info.st_ino):
                        error = error or ValueError("owned history path identity changed")
                    if self.file_bytes > self.max_bytes:
                        error = error or ValueError("disk history byte limit exceeded")
                except (OSError, ValueError) as caught:
                    error = error or caught
                finally:
                    try:
                        os.close(self.fd)
                        self.fd_close_returned = True
                    except OSError as caught:
                        error = error or caught
                    self.fd = None
            self.closed = self.connection_close_returned and self.fd_close_returned
        if error is not None:
            self._fail(error)

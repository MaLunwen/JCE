"""Cooperative kernel locks for first-party build resources (not dependencies)."""
from contextlib import contextmanager
import errno
import hashlib
import math
import os
from pathlib import Path
import threading
import time

_GUARD = threading.RLock()
_HELD = {}


def canonical(path):
    resolved = str(Path(path).resolve())
    if os.name == "nt":
        # realpath may add an extended prefix while another thread creates
        # the resource. Both spellings must identify the same kernel lock.
        if resolved[:8].lower() == "\\\\?\\unc\\":
            resolved = "\\\\" + resolved[8:]
        elif resolved.startswith("\\\\?\\"):
            resolved = resolved[4:]
    return os.path.normcase(resolved)


def _kernel(stream, acquire):
    if os.name == "nt":
        import msvcrt
        stream.seek(0)
        msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK if acquire else msvcrt.LK_UNLCK, 1)
    else:
        import fcntl
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB if acquire else fcntl.LOCK_UN)


@contextmanager
def cache_locks(resources, directory, *, timeout=1800.0, notify=None):
    """Acquire sorted canonical resources, with same-thread reentrant ownership.

    Persistent files must not be removed: a replacement inode could grant a
    second writer ownership. Kernel ownership expires when the process dies.
    A nested scope may reuse held resources; acquiring new resources is refused
    rather than risking cross-process lock-order inversion.
    """
    if not math.isfinite(timeout) or timeout < 0:
        raise ValueError("cache lock timeout must be finite and nonnegative")
    keys = sorted(set(canonical(path) for path in resources))
    owner = threading.get_ident()
    acquired = []
    deadline = time.monotonic() + timeout
    try:
        with _GUARD:
            if any(record[1] == owner for record in _HELD.values()) and any(_HELD.get(key, [None, None])[1] != owner for key in keys):
                raise RuntimeError("nested cache scope must declare all resources at its outer boundary")
        for key in keys:
            announced = False
            stream = None
            while True:
                with _GUARD:
                    record = _HELD.get(key)
                    if record and record[1] == owner:
                        record[2] += 1
                        acquired.append(key)
                        break
                    if record is None:
                        if stream is None:
                            Path(directory).mkdir(parents=True, exist_ok=True)
                            name = hashlib.sha256(key.encode("utf-8")).hexdigest() + ".lock"
                            stream = open(Path(directory) / name, "a+b", buffering=0)
                            os.set_inheritable(stream.fileno(), False)
                            if os.fstat(stream.fileno()).st_size == 0:
                                stream.write(b"\0")
                        try:
                            _kernel(stream, True)
                        except OSError as error:
                            if error.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                                raise
                        else:
                            _HELD[key] = [stream, owner, 1]
                            acquired.append(key)
                            stream = None
                            break
                if not announced and notify:
                    notify("waiting for build cache: " + key)
                    announced = True
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    if stream:
                        stream.close()
                        stream = None
                    raise TimeoutError("build cache ownership timed out: " + key)
                time.sleep(min(.1, remaining))
        yield
    finally:
        if 'stream' in locals() and stream:
            stream.close()
        with _GUARD:
            for key in reversed(acquired):
                record = _HELD[key]
                record[2] -= 1
                if record[2] == 0:
                    try:
                        _kernel(record[0], False)
                    finally:
                        record[0].close()
                        del _HELD[key]

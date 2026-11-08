"""Bounded explicit data inputs for the XP VM transport; never launch selection."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re
import stat

MAX_FILES = 4
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_TOTAL_BYTES = 16 * 1024 * 1024
RESERVED = frozenset(("editor-profile", ".jce-xp-owner"))
OWNED_OUTPUT = re.compile(r"jce-(?:launch-[0-9a-f]{32}\.json|child-[0-9a-f]{32}\.(?:stdout|stderr)\.bin)\Z", re.I)


class InputError(ValueError):
    def __init__(self, state):
        super().__init__(state)
        self.state = state


def _identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


@dataclass(frozen=True)
class InputFile:
    name: str
    path: Path
    info: os.stat_result
    sha256: str

    def receipt(self):
        return dict(name=self.name, local_path=str(self.path), size_bytes=self.info.st_size,
                    sha256=self.sha256, source_stat=dict(device=self.info.st_dev, inode=self.info.st_ino,
                        mtime_ns=self.info.st_mtime_ns, ctime_ns=self.info.st_ctime_ns),
                    state="SNAPSHOTTED", upload_verified=False)


def reject_collisions(inputs, reserved):
    names = RESERVED | {name.casefold() for name in reserved}
    if any(item.name.casefold() in names or OWNED_OUTPUT.fullmatch(item.name) for item in inputs):
        raise InputError("INPUT_NAME_COLLISION")


def unchanged(item, no_aliases):
    try:
        no_aliases(item.path)
        if _identity(item.path.stat()) != _identity(item.info):
            raise InputError("INPUT_CHANGED")
    except (OSError, ValueError):
        raise InputError("INPUT_CHANGED") from None


def prepare(values, *, validate_names, no_aliases, reserved=()):
    """Snapshot ordinary files before any connection; preserve caller bytes."""
    if type(values) not in (list, tuple) or len(values) > MAX_FILES:
        raise InputError("INVALID_INPUT_REQUEST")
    requests = []
    for value in values:
        if not isinstance(value, str):
            raise InputError("INVALID_INPUT_REQUEST")
        name, equals, local = value.partition("=")
        if not equals or not local:
            raise InputError("INVALID_INPUT_REQUEST")
        path = Path(local)
        if ".." in path.parts:
            raise InputError("INPUT_SOURCE_UNSAFE")
        requests.append((name, path.absolute()))
    try:
        validate_names([name for name, _path in requests])
    except (TypeError, ValueError):
        raise InputError("INVALID_INPUT_REQUEST") from None
    inputs, total = [], 0
    for name, path in requests:
        if ".." in path.parts:
            raise InputError("INPUT_SOURCE_UNSAFE")
        try:
            no_aliases(path)
            info = path.stat()
            if not stat.S_ISREG(info.st_mode):
                raise InputError("INPUT_SOURCE_NOT_REGULAR")
            if not 0 < info.st_size <= MAX_FILE_BYTES or total + info.st_size > MAX_TOTAL_BYTES:
                raise InputError("INPUT_SIZE_LIMIT")
            with path.open("rb") as source:
                opened = os.fstat(source.fileno())
                # Windows path and held-file ctime observations can differ.
                # Keep complete before/after fingerprints within each channel.
                if _identity(opened)[:4] != _identity(info)[:4]:
                    raise InputError("INPUT_CHANGED")
                raw = source.read(MAX_FILE_BYTES + 1)
                after = os.fstat(source.fileno())
            item = InputFile(name, path, info, hashlib.sha256(raw).hexdigest())
            if len(raw) != info.st_size or _identity(after) != _identity(opened):
                raise InputError("INPUT_CHANGED")
            unchanged(item, no_aliases)
        except InputError:
            raise
        except ValueError:
            raise InputError("INPUT_SOURCE_UNSAFE") from None
        except OSError:
            raise InputError("INPUT_SOURCE_UNAVAILABLE") from None
        inputs.append(item)
        total += info.st_size
    reject_collisions(inputs, reserved)
    return inputs

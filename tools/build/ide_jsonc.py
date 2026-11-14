"""Bounded JSONC edits and recoverable local IDE configuration publication.

Only selected values/entries are changed. Comments and all other source slices
remain byte-for-byte. This owner never stages files or writes package caches.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import datetime
import hashlib
import json
import os
from pathlib import Path
import secrets
import stat
import tempfile

MAX_BYTES = 1024 * 1024
MAX_DEPTH = 64
MAX_NODES = 20000


def _same_value(left, right):
    # Python considers False == 0 and True == 1, including inside dictionaries.
    # Those are different JSON values and cannot authorize a legacy migration.
    options = {"sort_keys": True, "separators": (",", ":"), "ensure_ascii": True}
    return json.dumps(left, **options) == json.dumps(right, **options)


@dataclass
class Node:
    value: object
    start: int
    end: int
    members: dict = field(default_factory=dict)
    items: list = field(default_factory=list)
    trailing_comma: bool = False


class Document:
    """Parse JSONC while retaining exact positions, including a UTF-8 BOM."""

    def __init__(self, raw: bytes):
        if len(raw) > MAX_BYTES:
            raise ValueError("IDE configuration exceeds 1 MiB")
        self.bom = raw.startswith(b"\xef\xbb\xbf")
        self.text = raw.decode("utf-8-sig")
        self.pos = 0
        self.count = 0
        self.root = self._value(0)
        self._space()
        if self.pos != len(self.text):
            raise ValueError("unexpected content after JSONC root")

    def _space(self):
        while self.pos < len(self.text):
            if self.text[self.pos].isspace():
                self.pos += 1
            elif self.text.startswith("//", self.pos):
                end = self.text.find("\n", self.pos + 2)
                self.pos = len(self.text) if end < 0 else end + 1
            elif self.text.startswith("/*", self.pos):
                end = self.text.find("*/", self.pos + 2)
                if end < 0:
                    raise ValueError("unterminated JSONC comment")
                self.pos = end + 2
            else:
                return

    def _value(self, depth):
        self.count += 1
        if depth > MAX_DEPTH or self.count > MAX_NODES:
            raise ValueError("IDE JSONC structure budget exceeded")
        self._space()
        start = self.pos
        if start >= len(self.text):
            raise ValueError("missing JSONC value")
        opener = self.text[start]
        if opener not in "[{":
            try:
                value, self.pos = json.JSONDecoder().raw_decode(self.text, start)
            except json.JSONDecodeError as exc:
                raise ValueError("invalid JSONC scalar") from exc
            if isinstance(value, float) and not (-float("inf") < value < float("inf")):
                raise ValueError("non-finite JSONC number")
            return Node(value, start, self.pos)
        self.pos += 1
        close = "}" if opener == "{" else "]"
        members, items = {}, []
        trailing = False
        while True:
            self._space()
            if self.pos >= len(self.text):
                raise ValueError("unterminated JSONC container")
            if self.text[self.pos] == close:
                self.pos += 1
                value = ({key: node.value for key, node in members.items()}
                         if opener == "{" else [node.value for node in items])
                return Node(value, start, self.pos, members, items, trailing)
            if opener == "{":
                key = self._value(depth + 1)
                if not isinstance(key.value, str) or key.members or key.items:
                    raise ValueError("JSONC object key must be a string")
                if key.value in members:
                    raise ValueError("duplicate JSONC key: " + key.value)
                self._space()
                if self.pos >= len(self.text) or self.text[self.pos] != ":":
                    raise ValueError("missing JSONC colon")
                self.pos += 1
                members[key.value] = self._value(depth + 1)
            else:
                items.append(self._value(depth + 1))
            self._space()
            if self.pos < len(self.text) and self.text[self.pos] == ",":
                self.pos += 1
                trailing = True
            elif self.pos < len(self.text) and self.text[self.pos] == close:
                trailing = False
            else:
                raise ValueError("missing JSONC comma or closing delimiter")

    def encode(self, edits):
        text = self.text
        previous = len(text) + 1
        for start, end, replacement in sorted(edits, reverse=True):
            if not 0 <= start <= end <= previous:
                raise ValueError("overlapping JSONC edits")
            text = text[:start] + replacement + text[end:]
            previous = start
        raw = (b"\xef\xbb\xbf" if self.bom else b"") + text.encode("utf-8")
        Document(raw)
        return raw

    def _indent(self, node):
        begin = self.text.rfind("\n", 0, node.start) + 1
        prefix = self.text[begin:node.start]
        return prefix if not prefix.strip() else ""

    def _render(self, value, indent):
        newline = "\r\n" if "\r\n" in self.text else "\n"
        return json.dumps(value, ensure_ascii=False, indent=4).replace("\n", newline + indent)

    def set_values(self, node, values):
        if not isinstance(node.value, dict):
            raise ValueError("IDE JSONC object required")
        edits, additions = [], {}
        for key, value in values.items():
            current = node.members.get(key)
            if current is None:
                additions[key] = value
            elif not _same_value(current.value, value):
                edits.append((current.start, current.end,
                              self._render(value, self._indent(node) + "    ")))
        if additions:
            indent = self._indent(node) + "    "
            newline = "\r\n" if "\r\n" in self.text else "\n"
            lines = [json.dumps(key) + ": " + self._render(value, indent)
                     for key, value in additions.items()]
            text = newline + indent + ("," + newline + indent).join(lines) + newline + self._indent(node)
            if node.members and not node.trailing_comma:
                last = next(reversed(node.members.values()))
                if last.end == node.end - 1:
                    text = "," + text
                else:
                    edits.append((last.end, last.end, ","))
            edits.append((node.end - 1, node.end - 1, text))
        return edits

    def append(self, node, value):
        if not isinstance(node.value, list):
            raise ValueError("IDE JSONC array required")
        newline = "\r\n" if "\r\n" in self.text else "\n"
        indent = self._indent(node) + "    "
        text = newline + indent + self._render(value, indent) + newline + self._indent(node)
        edits = [(node.end - 1, node.end - 1, text)]
        if node.items and not node.trailing_comma:
            if node.items[-1].end == node.end - 1:
                edits[0] = (node.end - 1, node.end - 1, "," + text)
            else:
                edits.append((node.items[-1].end, node.items[-1].end, ","))
        return edits

    def remove_item(self, array, item):
        """Remove one array value and its separator, retaining all comments."""
        index = next((i for i, node in enumerate(array.items) if node is item), None)
        if index is None:
            raise ValueError("JSONC item does not belong to the selected array")
        edits = [(item.start, item.end, "")]
        saved = self.pos
        try:
            self.pos = item.end
            self._space()
            if self.text[self.pos] == ",":
                edits.append((self.pos, self.pos + 1, ""))
            elif index:
                self.pos = array.items[index - 1].end
                self._space()
                if self.text[self.pos] != ",":
                    raise ValueError("missing JSONC item separator")
                edits.append((self.pos, self.pos + 1, ""))
        finally:
            self.pos = saved
        return edits


def set_object(raw: bytes, values: dict) -> bytes:
    doc = Document(raw)
    return doc.encode(doc.set_values(doc.root, values))


def _entry_hash(entry):
    copied = json.loads(json.dumps(entry))
    vendor = copied.get("vendor", {})
    owner = vendor.get("jce/ide/1.0") if isinstance(vendor, dict) else None
    if not isinstance(owner, dict):
        return None
    owner.pop("sha256", None)
    return hashlib.sha256(json.dumps(copied, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True).encode("ascii")).hexdigest()


def stamp_entry(entry):
    """Record exact generated contents so later user edits cannot be overwritten."""
    copied = json.loads(json.dumps(entry))
    copied.setdefault("vendor", {})["jce/ide/1.0"] = {"managed": True}
    copied["vendor"]["jce/ide/1.0"]["sha256"] = _entry_hash(copied)
    return copied


def owned_entry(entry):
    """True only for an unchanged complete entry published by this owner."""
    if not isinstance(entry, dict):
        return False
    vendor = entry.get("vendor", {})
    owner = vendor.get("jce/ide/1.0") if isinstance(vendor, dict) else None
    return (isinstance(owner, dict) and set(owner) == {"managed", "sha256"}
            and owner["managed"] is True and owner["sha256"] == _entry_hash(entry))


def named_entry(raw: bytes, field: str, key: str, entry: dict, *, replace_owned=False) -> bytes:
    doc = Document(raw)
    array = doc.root.members.get(field)
    if array is None:
        return doc.encode(doc.set_values(doc.root, {field: [entry]}))
    if not isinstance(array.value, list):
        raise ValueError("IDE configuration requires array: " + field)
    found = [node for node in array.items if isinstance(node.value, dict)
             and node.value.get(key) == entry[key]]
    if len(found) > 1:
        raise ValueError("duplicate IDE entry: " + str(entry[key]))
    if found:
        node = found[0]
        if _same_value(node.value, entry):
            return raw
        if not replace_owned or not owned_entry(node.value):
            raise ValueError("existing IDE entry has user changes: " + str(entry[key]))
        return doc.encode([(node.start, node.end, doc._render(entry, doc._indent(node)))])
    return doc.encode(doc.append(array, entry))


def migrate_named_entry(raw: bytes, field: str, key: str, entry: dict, legacy_entries=()) -> bytes:
    """Migrate an exact old generated shape without claiming user-edited rows.

    VS Code task/launch schemas have no portable vendor stamp. The complete
    known previous value is the migration identity; an equal label is not one.
    """
    doc = Document(raw)
    array = doc.root.members.get(field)
    if array is None:
        return named_entry(raw, field, key, entry)
    if not isinstance(array.value, list):
        raise ValueError("IDE configuration requires array: " + field)
    legacy_names = {old[key] for old in legacy_entries}
    current = [node for node in array.items if isinstance(node.value, dict)
               and node.value.get(key) == entry[key]]
    legacy = [node for node in array.items if isinstance(node.value, dict)
              and node.value.get(key) in legacy_names]
    if len(current) > 1 or len(legacy) > 1:
        raise ValueError("duplicate IDE entry during migration: " + str(entry[key]))
    if current and not _same_value(current[0].value, entry):
        raise ValueError("existing IDE entry has user changes: " + str(entry[key]))
    if legacy:
        old = legacy[0]
        if not any(_same_value(old.value, previous) for previous in legacy_entries):
            raise ValueError("legacy IDE entry has user changes: " + str(old.value[key]))
        if current:
            return doc.encode(doc.remove_item(array, old))
        return doc.encode([(old.start, old.end, doc._render(entry, doc._indent(old)))])
    return named_entry(raw, field, key, entry)


def _safe_path(root: Path, relative: str) -> Path:
    path = root / relative
    if path.is_absolute() and not path.is_relative_to(root):
        raise ValueError("IDE configuration path escaped source root")
    if ".." in Path(relative).parts:
        raise ValueError("IDE configuration path traversal")
    current = root
    for part in Path(relative).parts:
        current /= part
        if current.exists() or current.is_symlink():
            found = current.lstat()
            if stat.S_ISLNK(found.st_mode) or getattr(found, "st_file_attributes", 0) & 0x400:
                raise ValueError("IDE configuration aliases are refused: " + relative)
            if current != path and not stat.S_ISDIR(found.st_mode):
                raise ValueError("IDE configuration parent is not a directory")
    return path


def read_file(root: Path, relative: str) -> bytes | None:
    path = _safe_path(root, relative)
    if not path.exists():
        return None
    found = path.stat()
    if not stat.S_ISREG(found.st_mode) or found.st_size > MAX_BYTES:
        raise ValueError("IDE configuration type/size refused: " + relative)
    with path.open("rb") as stream:
        raw = stream.read(MAX_BYTES + 1)
        held = os.fstat(stream.fileno())
    after = _safe_path(root, relative).stat()
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
    if len(raw) > MAX_BYTES or len(raw) != found.st_size or identity(found) != identity(held) or identity(held) != identity(after):
        raise ValueError("IDE configuration changed while reading: " + relative)
    return raw


def _atomic(path: Path, raw: bytes, mode=None, guard=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".jce-ide-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        if mode is not None:
            os.chmod(temporary, stat.S_IMODE(mode))
        if guard is not None and not guard():
            raise ValueError("IDE file changed immediately before atomic publication: " + str(path))
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Transaction:
    """Back up exact preimages, publish atomically per file and guard rollback."""

    def __init__(self, root: Path, changes: dict, *, validate_json=True):
        if type(validate_json) is not bool:
            raise ValueError("IDE transaction validation policy must be boolean")
        self.validate_json = validate_json
        self.root = root.resolve()
        self.changes = {key: value for key, value in changes.items() if value[0] != value[1]}
        self.written = []
        self.metadata = {}
        self.created_directories = set()
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        self.backup = _safe_path(self.root, "build/validation/ide/" + stamp + "-" + secrets.token_hex(4))
        self.receipt = {"schema": 1, "owner": "jce-ide-local", "root": str(self.root),
                        "status": "PREPARED", "files": []}

    def apply(self):
        for relative, (before, after) in self.changes.items():
            if read_file(self.root, relative) != before:
                raise ValueError("IDE input changed before publication: " + relative)
            if after is not None and self.validate_json:
                Document(after)
        if not self.changes:
            return
        self.backup.mkdir(parents=True, exist_ok=False)
        for relative, (before, after) in self.changes.items():
            path = _safe_path(self.root, relative)
            self.metadata[relative] = path.stat() if before is not None else None
            if before is not None:
                saved = self.backup / "before" / relative
                saved.parent.mkdir(parents=True, exist_ok=True)
                saved.write_bytes(before)
            self.receipt["files"].append({"path": relative,
                "before_sha256": hashlib.sha256(before).hexdigest() if before is not None else None,
                "after_sha256": hashlib.sha256(after).hexdigest() if after is not None else None})
        self._receipt()
        try:
            for relative, (before, after) in self.changes.items():
                if read_file(self.root, relative) != before:
                    raise ValueError("IDE input changed during publication: " + relative)
                original = self.metadata[relative]
                parent = _safe_path(self.root, relative).parent
                while parent != self.root and not parent.exists():
                    self.created_directories.add(parent.relative_to(self.root).as_posix())
                    parent = parent.parent
                if after is None:
                    if read_file(self.root, relative) != before:
                        raise ValueError("IDE input changed before local removal: " + relative)
                    _safe_path(self.root, relative).unlink()
                else:
                    _atomic(_safe_path(self.root, relative), after, original.st_mode if original else None,
                            guard=lambda key=relative, expected=before: read_file(self.root, key) == expected)
                self.written.append(relative)
                if read_file(self.root, relative) != after:
                    raise ValueError("IDE output readback differs: " + relative)
        except BaseException:
            self.rollback()
            raise

    def _receipt(self):
        _atomic(self.backup / "receipt.json",
                (json.dumps(self.receipt, indent=2) + "\n").encode("utf-8"))

    def commit(self, status="APPLIED_NOT_IDE_EXECUTED"):
        if self.changes:
            self.receipt["status"] = status
            self._receipt()

    def rollback(self):
        conflicts = []
        for relative in reversed(self.written):
            try:
                before, after = self.changes[relative]
                if read_file(self.root, relative) != after:
                    conflicts.append(relative)
                    continue
                path = _safe_path(self.root, relative)
                if before is None:
                    path.unlink()
                else:
                    original = self.metadata[relative]
                    _atomic(path, before, original.st_mode,
                            guard=lambda key=relative, expected=after: read_file(self.root, key) == expected)
                    os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))
            except (OSError, ValueError):
                conflicts.append(relative)
        for relative in sorted(self.created_directories, key=lambda value: len(Path(value).parts), reverse=True):
            try:
                path = _safe_path(self.root, relative)
                if path.exists():
                    path.rmdir()  # Only an ordinary, still-empty directory created by this transaction.
            except (OSError, ValueError):
                conflicts.append(relative + "/")
        self.receipt["status"] = "ROLLBACK_CONFLICT" if conflicts else "ROLLED_BACK"
        self.receipt["conflicts"] = conflicts
        if self.changes and self.backup.exists():
            self._receipt()
        if conflicts:
            raise ValueError("IDE rollback preserved concurrent edits: " + ", ".join(conflicts))


def restore(root: Path, backup: Path, *, dry_run=False):
    """Restore one successful local publication, refusing changed current bytes."""
    root = root.resolve()
    if not backup.is_absolute():
        backup = root / backup
    if backup.parent != root / "build/validation/ide":
        raise ValueError("local rollback requires one backup under build/validation/ide")
    relative = backup.relative_to(root).as_posix()
    _safe_path(root, relative)
    raw = read_file(root, relative + "/receipt.json")
    if raw is None:
        raise ValueError("local rollback receipt is missing")
    receipt = json.loads(raw.decode("utf-8-sig"))
    if (receipt.get("schema") != 1 or receipt.get("owner") != "jce-ide-local" or
            receipt.get("root") != str(root) or receipt.get("status") != "APPLIED_NOT_IDE_EXECUTED"):
        raise ValueError("local rollback needs a successful receipt for this source root")
    allowed = {"CMakeUserPresets.json", ".vscode/settings.json", ".vscode/c_cpp_properties.json",
               ".vscode/tasks.json", ".vscode/launch.json"}
    rows = receipt.get("files")
    if not isinstance(rows, list) or not 1 <= len(rows) <= len(allowed):
        raise ValueError("local rollback receipt has an invalid file roster")
    desired, current, expected = {}, {}, {}
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"path", "before_sha256", "after_sha256"}:
            raise ValueError("local rollback receipt entry is invalid")
        key = row["path"]
        if not isinstance(key, str) or key not in allowed or key in desired:
            raise ValueError("local rollback receipt has a duplicate or unowned path")
        saved = read_file(root, relative + "/before/" + key)
        digest = hashlib.sha256(saved).hexdigest() if saved is not None else None
        if digest != row["before_sha256"]:
            raise ValueError("local rollback backup identity differs: " + key)
        after = row["after_sha256"]
        if not isinstance(after, str) or len(after) != 64 or any(c not in "0123456789abcdef" for c in after):
            raise ValueError("local rollback published identity is invalid: " + key)
        desired[key], current[key], expected[key] = saved, read_file(root, key), after
    if all(current[key] == value for key, value in desired.items()):
        return {"status": "ALREADY_RESTORED", "backup": str(backup), "files": len(rows)}
    for key, raw in current.items():
        digest = hashlib.sha256(raw).hexdigest() if raw is not None else None
        if digest != expected[key]:
            raise ValueError("local rollback refused unknown edits after preparation: " + key)
    if dry_run:
        return {"status": "DRY_RUN", "backup": str(backup), "files": len(rows)}
    transaction = Transaction(root, {key: (current[key], value) for key, value in desired.items()})
    transaction.apply()
    try:
        transaction.commit("RESTORED_LOCAL_CONFIG_ONLY")
    except BaseException:
        transaction.rollback()
        raise
    return {"status": "RESTORED_LOCAL_CONFIG_ONLY", "backup": str(transaction.backup), "files": len(rows)}

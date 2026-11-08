#!/usr/bin/env python3
"""Explicit, pinned SSH/SFTP delivery and bounded Windows XP process smoke.

Paramiko is an optional runtime dependency, imported only when connecting.
Receipts describe process smoke, never complete Editor/graphics qualification.
Timeout closes only this SSH channel: remote process termination is unknown.
"""
from __future__ import annotations

import argparse
import base64
import codecs
import hashlib
import hmac
import importlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import threading
import time
import uuid
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
FLOORS = {"x86": (5, 1), "x64": (5, 2)}
MACHINES = {"x86": (0x014c, 0x10b), "x64": (0x8664, 0x20b)}
PROBE_MARKER = "JCE_XP_A1_PROBE_OK"
MAX_ARTIFACT_BYTES = 512 * 1024 * 1024
MAX_COLLECT_FILES = 16
MAX_COLLECT_BYTES = 64 * 1024 * 1024
EDITOR_BACKGROUND_SETTINGS = b'{"player":{"run_in_background":true}}\n'
EDITOR_LOCALES = ("en", "zh_cn")


class SmokeError(ValueError):
    """A finite, secret-free failure reason suitable for a receipt."""

    def __init__(self, state):
        super().__init__(state)
        self.state = state


def fingerprint(value):
    """Accept OpenSSH SHA256 notation, with canonical optional base64 padding."""
    token = value.removeprefix("SHA256:")
    try:
        raw = base64.b64decode(token + "=" * (-len(token) % 4), validate=True)
    except (ValueError, TypeError):
        raise SmokeError("INVALID_HOST_KEY_PIN") from None
    canonical = base64.b64encode(raw).decode("ascii")
    if len(raw) != 32 or token not in (canonical, canonical.rstrip("=")):
        raise SmokeError("INVALID_HOST_KEY_PIN")
    return "SHA256:" + canonical.rstrip("=")


class PinnedHostKey:
    """No trust-on-first-use or persistent known_hosts mutation."""

    def __init__(self, expected):
        self.expected = fingerprint(expected)

    def verify(self, key):
        observed = "SHA256:" + base64.b64encode(
            hashlib.sha256(key.asbytes()).digest()).decode("ascii").rstrip("=")
        if not hmac.compare_digest(observed, self.expected):
            raise SmokeError("HOST_KEY_MISMATCH")

    def missing_host_key(self, client, hostname, key):
        self.verify(key)


def _new_client():
    return importlib.import_module("paramiko").SSHClient()


def _no_aliases(path):
    for item in (path, *path.parents):
        if item.exists() or item.is_symlink():
            info = item.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise SmokeError("ALIASED_LOCAL_PATH")


def output_path(out, output_root=None, repo=ROOT):
    """Only a fresh strict child of an explicitly bounded build output root."""
    build = repo.absolute() / "build"
    selected = Path(output_root) if output_root else build / "validation"
    selected = selected if selected.is_absolute() else repo / selected
    target = Path(out)
    target = target if target.is_absolute() else repo / target
    if ".." in selected.parts or ".." in target.parts:
        raise SmokeError("UNSAFE_OUTPUT_PATH")
    _no_aliases(build)
    _no_aliases(selected)
    _no_aliases(target)
    build, selected, target = build.resolve(), selected.resolve(), target.resolve()
    if not selected.is_relative_to(build) or selected == build:
        raise SmokeError("UNSAFE_OUTPUT_ROOT")
    if not target.is_relative_to(selected) or target == selected or target.exists():
        raise SmokeError("UNSAFE_OR_OCCUPIED_OUTPUT")
    return target


def _windows_path(value):
    value = value.strip().replace("/", "\\")
    if (not re.match(r"^[A-Za-z]:\\", value) or len(value) > 1024
            or any(ch in value for ch in '\x00\r\n"%!^&|<>()')
            or any(part in (".", "..") for part in value.split("\\"))):
        raise SmokeError("UNSAFE_GUEST_PATH")
    return value.rstrip("\\")


def _sftp_path(value):
    value = value.replace("\\", "/").rstrip("/")
    if (not value or len(value) > 1024 or value.startswith("~")
            or any(ch in value for ch in '\x00\r\n"%!^&|<>()')
            or any(part in (".", "..") for part in value.split("/"))):
        raise SmokeError("UNSAFE_SFTP_PATH")
    return value


def environment(values, kind, timeout, *, editor_window=None):
    if editor_window is not None and editor_window not in ("hidden", "visible", "interactive"):
        raise SmokeError("INVALID_EDITOR_WINDOW_MODE")
    if editor_window is not None and kind != "editor":
        raise SmokeError("EDITOR_WINDOW_EDITOR_REQUIRED")
    hidden = "0" if editor_window in ("visible", "interactive") else "1"
    not_focusable = "0" if editor_window == "interactive" else "1"
    result = {}
    if kind == "editor":
        result = {"JCE_WINDOW_HIDDEN": hidden, "JCE_WINDOW_NOT_FOCUSABLE": not_focusable,
                  "JCE_MAX_SECONDS": str(max(1, int(timeout) - 5))}
    seen = set()
    for value in values:
        name, separator, content = value.partition("=")
        if name == "JCE_EDITOR_PROFILE_DIR":
            raise SmokeError("EDITOR_PROFILE_OVERRIDE_FORBIDDEN")
        pattern = r"[A-Za-z0-9_./:,+ =-]*"
        if kind == "editor" and name == "JCE_DBG_PICK":
            # The editor accepts a bounded name followed by @ and a frame.
            # Keep @ out of the generic remote environment grammar.
            pattern = r"[A-Za-z0-9_. -]{1,63}@[0-9]{1,9}"
        if (not separator or not re.fullmatch(r"JCE_[A-Z0-9_]{1,80}", name)
                or re.search(r"PASSWORD|SECRET|TOKEN|KEY", name)
                or name in seen or len(content) > 256
                or not re.fullmatch(pattern, content)):
            raise SmokeError("INVALID_ENVIRONMENT")
        seen.add(name)
        result[name] = content
    if kind == "editor":
        if result["JCE_WINDOW_HIDDEN"] != hidden or result["JCE_WINDOW_NOT_FOCUSABLE"] != not_focusable:
            raise SmokeError("EDITOR_WINDOW_POLICY_MISMATCH" if editor_window in ("visible", "interactive")
                             else "EDITOR_WINDOW_MUST_BE_HIDDEN")
        try:
            seconds = float(result["JCE_MAX_SECONDS"])
        except ValueError:
            raise SmokeError("INVALID_ENVIRONMENT") from None
        if not 0 < seconds < timeout:
            raise SmokeError("INVALID_ENVIRONMENT")
    return result


def pe_identity(path, arch):
    """Bounded common PE header identity; this is not the probe import policy."""
    with path.open("rb") as stream:
        raw = stream.read(64)
        if len(raw) != 64 or raw[:2] != b"MZ":
            raise SmokeError("INVALID_PE_HEADER")
        offset = struct.unpack_from("<I", raw, 0x3c)[0]
        if not 64 <= offset <= 1024 * 1024:
            raise SmokeError("INVALID_PE_HEADER")
        stream.seek(offset)
        raw = stream.read(96)
    if len(raw) != 96 or raw[:4] != b"PE\x00\x00":
        raise SmokeError("INVALID_PE_HEADER")
    machine = struct.unpack_from("<H", raw, 4)[0]
    magic = struct.unpack_from("<H", raw, 24)[0]
    optional_size = struct.unpack_from("<H", raw, 20)[0]
    if optional_size < (112 if arch == "x64" else 96):
        raise SmokeError("INVALID_PE_HEADER")
    if (machine, magic) != MACHINES[arch]:
        raise SmokeError("ARTIFACT_ARCH_MISMATCH")
    os_version = struct.unpack_from("<HH", raw, 24 + 40)
    subsystem_version = struct.unpack_from("<HH", raw, 24 + 48)
    subsystem = struct.unpack_from("<H", raw, 24 + 68)[0]
    if os_version != FLOORS[arch] or subsystem_version != FLOORS[arch]:
        raise SmokeError("ARTIFACT_HEADER_FLOOR_MISMATCH")
    return {"machine": f"0x{machine:04x}", "optional_magic": f"0x{magic:04x}",
            "os_version": ".".join(map(str, os_version)),
            "subsystem_version": ".".join(map(str, subsystem_version)),
            "subsystem": subsystem,
            "subsystem_name": {1: "NATIVE", 2: "WINDOWS_GUI", 3: "WINDOWS_CUI"}.get(subsystem, "UNKNOWN")}


def launch_command(directory, executable, env, *, arguments=()):
    """Use validated owned paths and the XP cmd contract for GUI/console waits."""
    statements = ['cd /d "' + directory + '"']
    statements += ['set "' + name + "=" + value + '"' for name, value in sorted(env.items())]
    if any(not isinstance(value, str) or not 1 <= len(value) <= 1024
            or any(ord(ch) < 32 or ch in '\"%!^&|<>()' for ch in value) for value in arguments):
        raise SmokeError("INVALID_LAUNCH_ARGUMENTS")
    tail = "".join(' "' + value + '"' for value in arguments)
    statements.append('start "" /b /wait "' + executable + '"' + tail)
    # XP/Bitvise can return zero after a final START despite a failed child.
    # Delayed expansion observes its completed ERRORLEVEL, not a pre-launch value.
    # The unconditional tail also propagates a failed cd/set without launching.
    return ('cmd /d /q /v:on /s /c "' + " && ".join(statements)
            + ' & exit /b !errorlevel!' + '"')


def _audit_probe(path, arch):
    spec = importlib.util.spec_from_file_location("jce_xp_vm_pe", Path(__file__).with_name("xp_compat.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.audit_pe(path, arch)


def _audit_native(path, arch, export_receipt, *, host, user, host_key_sha256):
    spec = importlib.util.spec_from_file_location("jce_xp_vm_native_policy",
                                                 Path(__file__).with_name("xp_native_probe.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.audit_pe(path, arch, export_receipt, host=host, user=user, host_key_sha256=host_key_sha256)


def _control_call(call, timeout, close_owned, *, clock=time.monotonic, sleep=time.sleep):
    """Bound Paramiko request acknowledgements, which ignore channel timeout.

    Closing only the owned channel/client releases normal Paramiko waiters.
    A daemon retains cleanup ownership if an underlying resolver cannot cancel.
    """
    finished, abandoned = threading.Event(), threading.Event()
    result = {}

    def close():
        try:
            close_owned()
        except Exception:
            pass

    def worker():
        try:
            result["value"] = call()
        except BaseException as exc:
            result["error"] = exc
        finally:
            if abandoned.is_set():
                value = result.get("value")
                if hasattr(value, "close"):
                    try:
                        value.close()
                    except Exception:
                        pass
                close()
            finished.set()

    started = clock()
    threading.Thread(target=worker, name="jce-xp-ssh-request", daemon=True).start()
    while not finished.is_set():
        remaining = timeout - (clock() - started)
        if remaining <= 0:
            abandoned.set()
            close()
            raise TimeoutError()
        # Give injected/real transport threads a scheduling opportunity too.
        if not finished.wait(min(0.002, remaining)):
            sleep(min(0.01, remaining))
    if "error" in result:
        raise result["error"]
    if clock() - started >= timeout:
        abandoned.set()
        close()
        raise TimeoutError()
    return result.get("value")


def command(client, text, timeout, limit, *, clock=time.monotonic, sleep=time.sleep):
    """Drain both streams cooperatively, keeping a bounded combined raw prefix."""
    result = {"state": "SSH_FAILURE", "exit_code": None, "stdout": b"", "stderr": b"",
              "output_truncated": False}
    channel = None
    start = clock()
    try:
        channel = client.get_transport().open_session(timeout=timeout)
        channel.settimeout(max(0.001, timeout - (clock() - start)))
        _control_call(lambda: channel.exec_command(text),
                      max(0.001, timeout - (clock() - start)), channel.close,
                      clock=clock, sleep=sleep)
        while True:
            if clock() - start >= timeout:
                result["state"] = "TIMEOUT"
                break
            progress = False
            for ready, read, name in ((channel.recv_ready, channel.recv, "stdout"),
                                      (channel.recv_stderr_ready, channel.recv_stderr, "stderr")):
                if ready():
                    remaining = limit - len(result["stdout"]) - len(result["stderr"])
                    if remaining <= 0:
                        result.update(state="OUTPUT_LIMIT", output_truncated=True)
                        return result
                    chunk = read(min(65536, remaining))
                    result[name] += chunk
                    progress = progress or bool(chunk)
            if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                result.update(state="EXITED", exit_code=channel.recv_exit_status())
                break
            if getattr(channel, "closed", False):
                break
            if not progress:
                sleep(min(0.02, timeout))
    except (TimeoutError, OSError) as exc:
        # Do not persist exception text: native/SSH errors may contain credentials.
        result["state"] = "TIMEOUT" if isinstance(exc, TimeoutError) else "SSH_FAILURE"
    except Exception:
        result["state"] = "SSH_FAILURE"
    finally:
        result["elapsed_seconds"] = max(0, clock() - start)
        if channel is not None:
            try:
                channel.close()
            except Exception:
                result["state"] = "SSH_FAILURE"
    return result


def _redact(raw, password, guest_encoding):
    result = raw
    for encoding in {"utf-8", "utf-16le", guest_encoding}:
        try:
            encoded = password.encode(encoding)
        except UnicodeError:
            continue
        result = result.replace(encoded, b"[REDACTED]")
    return result


def _secret_free(value, password):
    if not password:
        return value
    if isinstance(value, str):
        return value.replace(password, "[REDACTED]")
    if isinstance(value, dict):
        # Constants and computed public digests are independent of credentials.
        trusted = {"schema", "state", "sha256", "source_sha256", "host_key_sha256", "expected_arch", "kind",
                   "machine", "optional_magic", "os_version", "subsystem_version", "import_policy",
                   "import_audit_scope", "modal_window_state", "remote_process_cleanup", "warnings"}
        return {key: item if key in trusted else _secret_free(item, password)
                for key, item in value.items()}
    if isinstance(value, list):
        return [_secret_free(item, password) for item in value]
    return value


def _file_identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)



def _upload_checked(path, info, digest, remote_artifact, sftp, bounded):
    """One byte-verified upload owner for both the target and explicit helper."""
    try:
        with path.open("rb") as source, bounded(sftp.open, remote_artifact, "wx") as destination:
            remaining = info.st_size
            while remaining:
                chunk = source.read(min(1024 * 1024, remaining))
                if not chunk or len(chunk) > remaining:
                    raise SmokeError("UPLOAD_HASH_MISMATCH")
                bounded(destination.write, chunk)
                remaining -= len(chunk)
            if source.read(1):
                raise SmokeError("UPLOAD_HASH_MISMATCH")
        observed, size = hashlib.sha256(), 0
        with bounded(sftp.open, remote_artifact, "rb") as source:
            while True:
                chunk = bounded(source.read, min(1024 * 1024, info.st_size + 1 - size))
                if not chunk:
                    break
                size += len(chunk)
                if size > info.st_size:
                    raise SmokeError("UPLOAD_HASH_MISMATCH")
                observed.update(chunk)
    except FileExistsError:
        raise SmokeError("REMOTE_ARTIFACT_EXISTS") from None
    if size != info.st_size or observed.hexdigest() != digest:
        raise SmokeError("UPLOAD_HASH_MISMATCH")
    if _file_identity(path.stat()) != _file_identity(info):
        raise SmokeError("ARTIFACT_CHANGED")
    return dict(upload_verified=True, sha256=observed.hexdigest(), size_bytes=size,
                sftp_artifact=remote_artifact)


def collection_names(values):
    """Collect explicit flat filenames only; never enumerate user directories."""
    if len(values) > MAX_COLLECT_FILES:
        raise SmokeError("INVALID_COLLECTION_REQUEST")
    names = set()
    for value in values:
        if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", value)
                or value.endswith(".") or value.casefold() in names
                or re.fullmatch(r"(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", value, re.I)):
            raise SmokeError("INVALID_COLLECTION_REQUEST")
        names.add(value.casefold())
    return list(values)


def _remote_metadata(attributes):
    mode, size, modified = attributes.st_mode, attributes.st_size, attributes.st_mtime
    if (not all(isinstance(value, int) and not isinstance(value, bool) for value in (mode, size, modified))
            or size < 0):
        raise SmokeError("COLLECTION_METADATA_UNAVAILABLE")
    return {"mode": mode, "size_bytes": size, "mtime": modified,
            "uid": getattr(attributes, "st_uid", None), "gid": getattr(attributes, "st_gid", None)}


def _owned_canonical(sftp, bounded, remote):
    attributes = bounded(sftp.lstat, remote)
    if not stat.S_ISDIR(attributes.st_mode):
        raise SmokeError("COLLECTION_OWNED_DIRECTORY_ALIAS")
    return _sftp_path(bounded(sftp.normalize, remote))


def _editor_fixture_directory(sftp, bounded, remote, canonical, name, prefix):
    try:
        current = _owned_canonical(sftp, bounded, remote)
        directory = _owned_canonical(sftp, bounded, remote + "/" + name)
    except SmokeError:
        raise SmokeError(prefix + "_PATH_ALIAS") from None
    if current.casefold() != canonical.casefold() or directory.casefold() != (canonical + "/" + name).casefold():
        raise SmokeError(prefix + "_PATH_ALIAS")
    return directory


def _upload_editor_fixture(local, target, payload, sftp, bounded, entry):
    with local.open("xb") as stream:
        stream.write(payload)
    entry.update(local_relative=local.name)
    entry.update(_upload_checked(local, local.stat(), hashlib.sha256(payload).hexdigest(),
                                 target, sftp, bounded))


def _stage_editor_background(out, remote, sftp, bounded, inventory, entry):
    canonical = _owned_canonical(sftp, bounded, remote)
    directory = remote + "/.jce"
    try:
        bounded(sftp.mkdir, directory)
    except OSError:
        raise SmokeError("EDITOR_SETTINGS_COLLISION_OR_DENIED") from None
    entry["canonical_directory"] = _editor_fixture_directory(sftp, bounded, remote, canonical,
                                                              ".jce", "EDITOR_SETTINGS")
    if inventory(directory, 2):
        raise SmokeError("EDITOR_SETTINGS_INVENTORY_UNVERIFIED")
    _upload_editor_fixture(out / "editor-project-settings.json", directory + "/project-settings.json",
                           EDITOR_BACKGROUND_SETTINGS, sftp, bounded, entry)
    return canonical


def _verify_editor_fixture(remote, canonical, sftp, bounded, inventory, entry, payload, prefix):
    """Recheck the sole fixed file in a tool-owned subtree before launch."""
    relative = entry["relative_path"]
    name, filename = relative.split("/")
    directory = remote + "/" + name
    target = remote + "/" + relative
    _editor_fixture_directory(sftp, bounded, remote, canonical, name, prefix)
    entries = inventory(directory, 2)
    entry["inventory"] = entries
    if entries != [{"name": filename, "kind": "file", "size_bytes": len(payload)}]:
        raise SmokeError(prefix + "_INVENTORY_UNVERIFIED")
    attributes = bounded(sftp.lstat, target)
    if not stat.S_ISREG(attributes.st_mode):
        raise SmokeError(prefix + "_NOT_REGULAR_FILE")
    before = _remote_metadata(attributes)
    expected = canonical + "/" + relative
    normalized = _sftp_path(bounded(sftp.normalize, target))
    if normalized.casefold() != expected.casefold():
        raise SmokeError(prefix + "_PATH_ALIAS")
    entry.update(before=before, canonical_before=normalized)
    with bounded(sftp.open, target, "rb") as source:
        raw = bounded(source.read, len(payload) + 1)
    attributes = bounded(sftp.lstat, target)
    after = _remote_metadata(attributes)
    normalized = _sftp_path(bounded(sftp.normalize, target))
    entry.update(after=after, canonical_after=normalized)
    _editor_fixture_directory(sftp, bounded, remote, canonical, name, prefix)
    if normalized.casefold() != expected.casefold():
        raise SmokeError(prefix + "_PATH_ALIAS")
    if (raw != payload or before != after or not stat.S_ISREG(attributes.st_mode)
            or before["size_bytes"] != len(payload)):
        raise SmokeError(prefix + "_BYTES_UNVERIFIED")
    entry.update(verified=True, state="VERIFIED_BEFORE_LAUNCH",
                 path_observation="LSTAT_AND_CANONICAL_BEFORE_AFTER_NOT_ATOMIC")


def collect_outputs(sftp, remote, canonical, names, out, timeout, password, encoding, *, clock=time.monotonic):
    """Observe only requested regular files after the owned command has exited.

    SFTP v3 metadata/canonical checks cannot atomically exclude concurrent path
    replacement. Record that limitation rather than claim a no-TOCTOU proof.
    """
    result = {"state": "COLLECTION_FAILED", "verified": False, "files": [], "bytes_read": 0,
              "requested": list(names), "owned_canonical": canonical,
              "max_files": MAX_COLLECT_FILES, "max_bytes": MAX_COLLECT_BYTES,
              "path_observation": "LSTAT_AND_CANONICAL_BEFORE_AFTER_NOT_ATOMIC"}
    started = clock()

    def bounded(call, *values):
        remaining = timeout - (clock() - started)
        if remaining <= 0:
            raise SmokeError("COLLECTION_TIMEOUT")
        sftp.get_channel().settimeout(remaining)
        return call(*values)

    collected = out / "collected"
    collected.mkdir(exist_ok=False)
    for name in names:
        entry = {"name": name, "local_relative": "collected/" + name, "state": "NOT_COLLECTED",
                 "verified": False, "before": None, "after": None, "bytes_read": 0}
        result["files"].append(entry)
        raw = bytearray()
        opened = False
        target = remote + "/" + name
        try:
            if _owned_canonical(sftp, bounded, remote).casefold() != canonical.casefold():
                raise SmokeError("COLLECTION_OWNED_DIRECTORY_CHANGED")
            attributes = bounded(sftp.lstat, target)
            if not stat.S_ISREG(attributes.st_mode):
                raise SmokeError("COLLECTION_NOT_REGULAR_FILE")
            entry["before"] = _remote_metadata(attributes)
            expected = canonical + "/" + name
            normalized = _sftp_path(bounded(sftp.normalize, target))
            entry["canonical_before"] = normalized
            if normalized.casefold() != expected.casefold():
                raise SmokeError("COLLECTION_PATH_ALIAS")
            size = entry["before"]["size_bytes"]
            if size > MAX_COLLECT_BYTES - result["bytes_read"]:
                raise SmokeError("COLLECTION_BYTE_LIMIT")
            with bounded(sftp.open, target, "rb") as source:
                opened = True
                while len(raw) < size:
                    requested = min(1024 * 1024, size - len(raw))
                    chunk = bounded(source.read, requested)
                    if not chunk:
                        raise SmokeError("COLLECTION_FILE_CHANGED")
                    if len(chunk) > requested:
                        raise SmokeError("COLLECTION_BYTE_LIMIT")
                    raw.extend(chunk)
                    result["bytes_read"] += len(chunk)
            after = bounded(sftp.lstat, target)
            entry["after"] = _remote_metadata(after)
            normalized = _sftp_path(bounded(sftp.normalize, target))
            entry["canonical_after"] = normalized
            if (normalized.casefold() != expected.casefold()
                    or _owned_canonical(sftp, bounded, remote).casefold() != canonical.casefold()
                    or entry["before"] != entry["after"] or not stat.S_ISREG(after.st_mode)):
                raise SmokeError("COLLECTION_FILE_CHANGED")
            entry.update(state="COLLECTED", verified=True)
        except FileNotFoundError:
            entry["state"] = "COLLECTION_MISSING_FILE"
        except SmokeError as exc:
            entry["state"] = exc.state
        except TimeoutError:
            entry["state"] = "COLLECTION_TIMEOUT"
        except Exception:
            entry["state"] = "COLLECTION_IO_FAILURE"
        if opened:
            payload = bytes(raw)
            credential_present = False
            for codec in {"utf-8", "utf-16le", encoding}:
                try:
                    credential_present = credential_present or password.encode(codec) in payload
                except UnicodeError:
                    pass
            stored = b"[REDACTED: credential bytes detected in collected file]\n" if credential_present else payload
            entry.update(bytes_read=len(payload), source_sha256=hashlib.sha256(payload).hexdigest(),
                         sha256=hashlib.sha256(stored).hexdigest(), stored_bytes=len(stored),
                         credential_redacted=stored != payload)
            try:
                with (collected / name).open("xb") as destination:
                    destination.write(stored)
            except Exception:
                entry.update(state="COLLECTION_LOCAL_WRITE_FAILURE", verified=False)
            if stored != payload:
                entry.update(state="COLLECTION_CREDENTIAL_REDACTED", verified=False)
    if all(entry["verified"] for entry in result["files"]):
        result.update(state="COLLECTED", verified=True)
    return result


def _delivery_executable_name(role, digest, password):
    """Keep generated paths out of the credential filter's match set."""
    candidate = f"{role}-{digest}.exe"
    if password not in candidate:
        return candidate
    # Each run already owns a unique directory and verifies the uploaded hash.
    candidate = f"{role}.exe"
    if password in candidate:
        raise SmokeError("DELIVERY_NAME_CREDENTIAL_COLLISION")
    return candidate


def _terminal_request(args, password, env, names, out):
    """Prepare only the explicit Editor witness, after ordinary env admission."""
    requested = getattr(args, "require_terminal_ack", False)
    if type(requested) is not bool:
        raise SmokeError("INVALID_TERMINAL_ACK_REQUEST")
    if not requested:
        return None
    if args.kind != "editor":
        raise SmokeError("TERMINAL_ACK_EDITOR_REQUIRED")
    if getattr(args, "owned_launcher", None) is None:
        raise SmokeError("TERMINAL_ACK_OWNED_LAUNCH_REQUIRED")
    # The direct script entry must share the same canonical package reader.
    if str(ROOT) not in sys.path:
        sys.path.insert(0, str(ROOT))
    terminal = importlib.import_module("tools.terminal_ack_observation")
    try:
        stream = env.get("JCE_TRACE_STREAM", "")
        if stream:
            collection_names([stream])
            if stream not in names:
                raise ValueError("trace must be explicitly collected")
        trace_path = terminal.requested_trace_path(env, out / "collected")
    except ValueError:
        raise SmokeError("TERMINAL_ACK_TRACE_REQUEST_INVALID") from None
    # This is a public correlation witness, never an authentication credential.
    # Do not weaken redaction for short passwords that overlap a candidate.
    for _attempt in range(16):
        token = uuid.uuid4().hex
        if (isinstance(token, str) and terminal.TOKEN.fullmatch(token)
                and _secret_free(token, password) == token
                and _redact(token.encode("ascii"), password, args.guest_encoding) == token.encode("ascii")):
            env["JCE_TERMINAL_ACK_TOKEN"] = token
            return terminal, token, trace_path
    raise SmokeError("TERMINAL_ACK_TOKEN_UNAVAILABLE")


def _observe_terminal(request, result, out, metadata, collection=None):
    """Admit exact retained child stderr only after the actual successful wait."""
    terminal, token, trace_path = request
    if (result["state"] != "EXITED" or type(result["exit_code"]) is not int
            or result["exit_code"] != 0 or result["output_truncated"]
            or metadata["stderr_redacted"]):
        raise SmokeError("TERMINAL_ACK_REFUSED")
    stderr_path = Path(result.get("stderr_path", out / "artifact.stderr.bin"))
    process = dict(timed_out=False, returncode=result["exit_code"], stderr_path=str(stderr_path),
                   process_identity=result.get("process_identity"))
    try:
        observation = terminal.observe_terminal_ack(process, token,
            expected_stderr_path=stderr_path, trace_path=trace_path)
        if observation["stderr"]["sha256"] != hashlib.sha256(result["stderr"]).hexdigest():
            raise ValueError("retained stderr differs from the observed stream")
        if trace_path is not None:
            entries = (collection or {}).get("files", [])
            entry = next((item for item in entries if item["name"] == trace_path.name), None)
            if (entry is None or entry["verified"] is not True
                    or observation["trace"]["sha256"] != entry["source_sha256"]
                    or observation["trace"]["input_bytes"] != entry["bytes_read"]):
                raise ValueError("retained trace differs from the verified collection")
        return observation
    except (ValueError, OSError):
        raise SmokeError("TERMINAL_ACK_REFUSED") from None


def run_smoke(args, password, *, repo=ROOT, client_factory=None,
              clock=time.monotonic, sleep=time.sleep, nonce_factory=lambda: uuid.uuid4().hex):
    """Run explicit inputs, with injectable transport/clock ports for CPU controls.

    No password is accepted in argv or written into any receipt/command log.
    Each --out is fresh; failure evidence and owned remote files are retained.
    """
    out = output_path(args.out, args.output_root, repo)
    out.mkdir(parents=True, exist_ok=False)
    receipt = {"schema": "jce.xp.vm-smoke.v1", "kind": args.kind,
               "timestamp_utc": datetime.now(timezone.utc).isoformat(),
               "expected_arch": args.expected_arch, "host": args.host, "user": args.user,
               "host_key_sha256": "UNVALIDATED", "passed": False, "state": "NOT_RUN",
               "commands": {}, "editor_graphics_verified": False,
               "modal_window_state": "UNKNOWN", "remote_process_cleanup": "NOT_ATTEMPTED",
               "warnings": ["Process smoke does not prove pixels, graphics capability or complete Editor functionality.",
                            "Closing an SSH channel does not certify remote process termination."]}
    if args.kind == "native-probe":
        receipt.update(graphics_context_verified=False, physical_driver_verified=False,
                       interactive_graphics_verified=False,
                       native_scope="Collector-bound static imports and process smoke; runtime/session/context requires separate review")
    terminal_request = None
    owned_tool = launcher = launcher_info = launcher_hash = None
    owned_selected = getattr(args, "owned_launcher", None) is not None
    input_tool, inputs = None, []
    background_requested = getattr(args, "editor_run_in_background", False)
    editor_locale = getattr(args, "editor_locale", None)
    locale_payload = None
    if getattr(args, "require_terminal_ack", False) is not False:
        receipt.update(terminal_ack_required=True, terminal_ack_state="NOT_OBSERVED")
    client = sftp = None

    def observe(name, text, budget):
        result = command(client, text, budget, args.max_output_bytes, clock=clock, sleep=sleep)
        metadata = {key: value for key, value in result.items() if key not in ("stdout", "stderr")}
        for stream in ("stdout", "stderr"):
            raw = _redact(result[stream], password, args.guest_encoding)[:args.max_output_bytes]
            (out / (name + "." + stream + ".bin")).write_bytes(raw)
            metadata[stream + "_bytes"] = len(raw)
            metadata[stream + "_redacted"] = raw != result[stream]
        receipt["commands"][name] = metadata
        return result

    def metadata_command(name, text):
        result = observe(name, text, args.probe_timeout)
        if result["state"] != "EXITED":
            raise SmokeError("GUEST_PROBE_" + result["state"])
        if result["exit_code"] != 0:
            raise SmokeError("GUEST_PROBE_NONZERO_EXIT")
        return result["stdout"].decode(args.guest_encoding, "strict").strip()

    try:
        if args.kind not in ("core-probe", "native-probe", "editor"):
            raise SmokeError("INVALID_ARTIFACT_KIND")
        if type(background_requested) is not bool:
            raise SmokeError("INVALID_EDITOR_BACKGROUND_REQUEST")
        if background_requested and args.kind != "editor":
            raise SmokeError("EDITOR_BACKGROUND_EDITOR_REQUIRED")
        if background_requested:
            receipt["editor_background"] = dict(requested=True, state="NOT_STAGED", verified=False,
                upload_verified=False, relative_path=".jce/project-settings.json", effective_setting="UNOBSERVED",
                scope="PROJECTLESS_CWD_OR_MATCHING_PROJECT_ROOT_ONLY")
        if editor_locale is not None:
            if type(editor_locale) is not str or editor_locale not in EDITOR_LOCALES:
                raise SmokeError("INVALID_EDITOR_LOCALE_REQUEST")
            if args.kind != "editor":
                raise SmokeError("EDITOR_LOCALE_EDITOR_REQUIRED")
            locale_payload = (json.dumps({"language": editor_locale}, separators=(",", ":")) + "\n").encode("ascii")
            receipt["editor_locale"] = dict(requested=editor_locale, state="NOT_STAGED", verified=False,
                upload_verified=False, relative_path="editor-profile/editor-preferences.json",
                effective_locale="UNOBSERVED", scope="ISOLATED_EDITOR_PROFILE_ONLY")
        if owned_selected and args.kind != "editor":
            raise SmokeError("OWNED_LAUNCH_EDITOR_REQUIRED")
        pin = PinnedHostKey(args.host_key_sha256)
        receipt["host_key_sha256"] = pin.expected
        if not isinstance(password, str) or not password:
            raise SmokeError("EMPTY_CREDENTIAL")
        if not (1 <= args.timeout <= 600 and 1 <= args.probe_timeout <= 60
                and 1 <= args.connect_timeout <= 60 and 1 <= args.transfer_timeout <= 600
                and 1 <= args.max_output_bytes <= 8 * 1024 * 1024 and 1 <= args.port <= 65535):
            raise SmokeError("INVALID_BUDGET")
        editor_window = getattr(args, "editor_window", None)
        env = environment(args.env, args.kind, args.timeout, editor_window=editor_window)
        if editor_window is not None:
            receipt["editor_window_request"] = editor_window
        collect_names = collection_names(getattr(args, "collect_relative", []))
        try:
            codecs.lookup(args.guest_encoding)
        except LookupError:
            raise SmokeError("INVALID_GUEST_ENCODING") from None
        terminal_request = _terminal_request(args, password, env, collect_names, out)
        if owned_selected and getattr(args, "export_receipt", None) is None:
            raise SmokeError("OWNED_LAUNCH_EXPORT_RECEIPT_REQUIRED")
        if args.remote_dir:
            _sftp_path(args.remote_dir)
        marker = args.expect_marker
        if args.kind == "native-probe":
            if marker is None:
                raise SmokeError("NATIVE_PROBE_MARKER_REQUIRED")
            if getattr(args, "export_receipt", None) is None:
                raise SmokeError("NATIVE_PROBE_EXPORT_RECEIPT_REQUIRED")
        if marker is None and args.kind == "core-probe":
            marker = PROBE_MARKER
        if marker is not None and not re.fullmatch(r"[\x21-\x7e ]{1,256}", marker):
            raise SmokeError("INVALID_EXPECTED_MARKER")
        artifact = Path(args.artifact).absolute()
        _no_aliases(artifact)
        info = artifact.stat()
        if not stat.S_ISREG(info.st_mode) or artifact.suffix.lower() != ".exe" or not 0 < info.st_size <= MAX_ARTIFACT_BYTES:
            raise SmokeError("INVALID_ARTIFACT")
        pe = pe_identity(artifact, args.expected_arch)
        if args.kind == "core-probe":
            try:
                pe["probe_import_audit"] = _audit_probe(artifact, args.expected_arch)
            except Exception:
                raise SmokeError("PROBE_STATIC_AUDIT_FAILED") from None
        elif args.kind == "native-probe":
            try:
                pe["native_import_audit"] = _audit_native(artifact, args.expected_arch, args.export_receipt,
                    host=args.host, user=args.user, host_key_sha256=pin.expected)
            except Exception:
                raise SmokeError("NATIVE_STATIC_AUDIT_FAILED") from None
        elif owned_selected:
            if pe["subsystem"] != 2:
                raise SmokeError("EDITOR_GUI_SUBSYSTEM_REQUIRED")
            try:
                pe["owned_import_audit"] = _audit_native(artifact, args.expected_arch, args.export_receipt,
                    host=args.host, user=args.user, host_key_sha256=pin.expected)
            except Exception:
                raise SmokeError("EDITOR_STATIC_AUDIT_FAILED") from None
        else:
            pe["import_policy"] = "UNVERIFIED_EDITOR_REQUIRES_BROADER_POLICY"
        digest = hashlib.sha256()
        with artifact.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        local_hash = digest.hexdigest()
        filename = _delivery_executable_name("artifact", local_hash, password)
        launcher_name = None
        if _file_identity(artifact.stat()) != _file_identity(info):
            raise SmokeError("ARTIFACT_CHANGED")
        receipt["artifact"] = {"path": str(artifact), "size_bytes": info.st_size,
                               "sha256": local_hash, "pe": pe}
        if owned_selected:
            if str(ROOT) not in sys.path:
                sys.path.insert(0, str(ROOT))
            owned_tool = importlib.import_module("tools.build.xp_vm_owned")
            launcher = Path(args.owned_launcher).absolute()
            _no_aliases(launcher)
            launcher_info = launcher.stat()
            if (not stat.S_ISREG(launcher_info.st_mode) or launcher.suffix.lower() != ".exe"
                    or not 0 < launcher_info.st_size <= MAX_ARTIFACT_BYTES):
                raise SmokeError("INVALID_OWNED_LAUNCHER")
            launcher_pe = pe_identity(launcher, args.expected_arch)
            try:
                launcher_audit = _audit_native(launcher, args.expected_arch, args.export_receipt,
                    host=args.host, user=args.user, host_key_sha256=pin.expected)
            except Exception:
                raise SmokeError("OWNED_LAUNCHER_STATIC_AUDIT_FAILED") from None
            digest = hashlib.sha256()
            with launcher.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
            launcher_hash = digest.hexdigest()
            launcher_name = _delivery_executable_name("launcher", launcher_hash, password)
            if _file_identity(launcher.stat()) != _file_identity(launcher_info):
                raise SmokeError("ARTIFACT_CHANGED")
            receipt["owned_launch"] = dict(state="NOT_RUN", requested_desktop=owned_tool.DESKTOP,
                actual_child_desktop="UNOBSERVED", process_tree_cleanup="UNKNOWN",
                launcher=dict(path=str(launcher), sha256=launcher_hash, size_bytes=launcher_info.st_size,
                              pe=launcher_pe, import_audit=launcher_audit))
        input_values = getattr(args, "input_relative", [])
        if type(input_values) not in (list, tuple):
            raise SmokeError("INVALID_INPUT_REQUEST")
        if input_values:
            if str(ROOT) not in sys.path:
                sys.path.insert(0, str(ROOT))
            input_tool = importlib.import_module("tools.build.xp_vm_inputs")
            reserved = [filename, *collect_names]
            if launcher_name is not None:
                reserved.append(launcher_name)
            try:
                inputs = input_tool.prepare(input_values, validate_names=collection_names,
                                            no_aliases=_no_aliases, reserved=reserved)
            except input_tool.InputError as error:
                raise SmokeError(error.state) from None
            receipt["inputs"] = [item.receipt() for item in inputs]
        client = (client_factory or _new_client)()
        client.set_missing_host_key_policy(pin)
        _control_call(lambda: client.connect(
            hostname=args.host, port=args.port, username=args.user, password=password,
            timeout=args.connect_timeout, banner_timeout=args.connect_timeout,
            auth_timeout=args.connect_timeout, channel_timeout=args.connect_timeout,
            allow_agent=False, look_for_keys=False), args.connect_timeout, client.close,
            clock=clock, sleep=sleep)
        pin.verify(client.get_transport().get_remote_server_key())
        version = metadata_command("guest-os", "cmd /d /q /c ver")
        found = re.search(r"(?<!\d)(\d+)\.(\d+)\.\d+(?!\d)", version)
        if not found or tuple(map(int, found.groups())) != FLOORS[args.expected_arch]:
            raise SmokeError("GUEST_OS_MISMATCH")
        guest_arch = metadata_command("guest-arch", "cmd /d /q /c echo %PROCESSOR_ARCHITECTURE%")
        if guest_arch.upper() != {"x86": "X86", "x64": "AMD64"}[args.expected_arch]:
            raise SmokeError("GUEST_ARCH_MISMATCH")
        profile = _windows_path(metadata_command("guest-profile", "cmd /d /q /c echo %USERPROFILE%"))
        receipt["guest"] = {"version": version, "arch": guest_arch, "userprofile": profile,
                            "desktop": None, "desktop_source": None, "desktop_attempts": []}
        desktop = None
        registry = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\"
        for folder, name in (("Shell Folders", "guest-desktop-resolved"),
                             ("User Shell Folders", "guest-desktop-user")):
            source = registry + folder
            result = observe(name, 'cmd /d /q /c reg query "' + source + '" /v Desktop', args.probe_timeout)
            attempt = {"source": source, "command": name, "state": result["state"],
                       "exit_code": result["exit_code"], "value_state": "UNAVAILABLE"}
            receipt["guest"]["desktop_attempts"].append(attempt)
            if result["state"] != "EXITED" or result["exit_code"] != 0:
                continue
            try:
                value = result["stdout"].decode(args.guest_encoding, "strict")
            except UnicodeError:
                attempt["value_state"] = "INVALID_ENCODING"
                continue
            found = list(re.finditer(r"^[ \t]*Desktop[ \t]+(REG_(?:EXPAND_)?SZ)[ \t]+([^\r\n]+?)[ \t]*\r?$",
                                     value, re.M | re.I))
            if len(found) != 1:
                attempt["value_state"] = "MALFORMED_REGISTRY_VALUE"
                continue
            value_type, value = found[0].groups()
            if folder == "Shell Folders" and value_type.upper() != "REG_SZ":
                attempt["value_state"] = "UNRESOLVED_REGISTRY_VALUE"
                continue
            if value_type.upper() == "REG_EXPAND_SZ":
                value = re.sub(r"%USERPROFILE%", lambda match: profile, value, flags=re.I)
            try:
                desktop = _windows_path(value)
            except SmokeError:
                attempt["value_state"] = "UNSAFE_GUEST_PATH"
                continue
            attempt["value_state"] = "SELECTED"
            receipt["guest"].update(desktop=desktop, desktop_source=source)
            break
        if desktop is None:
            raise SmokeError("GUEST_DESKTOP_UNAVAILABLE")
        try:
            sftp = _control_call(client.open_sftp, args.connect_timeout, client.close,
                                 clock=clock, sleep=sleep)
        except TimeoutError:
            raise SmokeError("SFTP_HANDSHAKE_TIMEOUT") from None
        transfer_start = clock()

        def bounded(call, *values):
            remaining = args.transfer_timeout - (clock() - transfer_start)
            if remaining <= 0:
                raise SmokeError("TRANSFER_TIMEOUT")
            sftp.get_channel().settimeout(remaining)
            return call(*values)

        if args.remote_dir:
            candidates = [args.remote_dir]
        else:
            home = _sftp_path(bounded(sftp.normalize, "."))
            basename = desktop.rsplit("\\", 1)[-1]
            candidates = [home + "/" + basename, "/" + desktop.replace("\\", "/"),
                          desktop.replace("\\", "/"), "/" + basename, basename]
        remote = None
        for candidate in dict.fromkeys(candidates):
            candidate = _sftp_path(candidate)
            try:
                attributes = bounded(sftp.stat, candidate)
            except OSError:
                continue
            if stat.S_ISDIR(attributes.st_mode):
                remote = candidate
                break
        if remote is None:
            raise SmokeError("SFTP_DESKTOP_UNAVAILABLE")
        nonce = nonce_factory()
        if not re.fullmatch(r"[a-f0-9]{32}", nonce):
            raise SmokeError("INVALID_NONCE")
        owned = "jce-xp-smoke-" + nonce
        remote = remote + "/" + owned
        windows_owned = desktop + "\\" + owned
        try:
            bounded(sftp.mkdir, remote)
        except OSError:
            raise SmokeError("REMOTE_OWNERSHIP_COLLISION_OR_DENIED") from None
        receipt["delivery"] = {"sftp_dir": remote, "windows_dir": windows_owned,
                               "desktop_mapping_verified": False, "upload_verified": False,
                               "mapping_marker_removed": False}
        mapping_marker = remote + "/.jce-xp-owner"
        with bounded(sftp.open, mapping_marker, "wx") as destination:
            bounded(destination.write, nonce.encode("ascii"))
        proof = observe("desktop-mapping", 'cmd /d /q /c type "' + windows_owned + '\\.jce-xp-owner"', args.probe_timeout)
        if proof["state"] != "EXITED" or proof["exit_code"] != 0 or proof["stdout"].strip() != nonce.encode("ascii"):
            raise SmokeError("SFTP_DESKTOP_MAPPING_UNVERIFIED")
        receipt["delivery"]["desktop_mapping_verified"] = True
        # Remove only the exact marker created in this fresh, proven owned folder.
        bounded(sftp.remove, mapping_marker)
        receipt["delivery"]["mapping_marker_removed"] = True
        receipt["launch"] = {"working_dir": windows_owned, "profile_dir": None,
                             "profile_dir_fresh": False, "inventory": [], "inventory_verified": False,
                             "wait_policy": "CMD_START_B_WAIT_THEN_EXIT_B_DELAYED_ERRORLEVEL",
                             "wait_contract_completed": False,
                             "wait_scope": "EXPLICIT_OS_WAIT_AND_EXIT_CONTRACT_NO_INDEPENDENT_CHILD_PID_OBSERVATION"}
        if args.kind == "editor":
            profile_name = "editor-profile"
            try:
                bounded(sftp.mkdir, remote + "/" + profile_name)
            except OSError:
                raise SmokeError("EDITOR_PROFILE_COLLISION_OR_DENIED") from None
            profile_dir = windows_owned.replace("\\", "/") + "/" + profile_name
            env["JCE_EDITOR_PROFILE_DIR"] = profile_dir
            receipt["launch"]["profile_dir"] = profile_dir
        remote_artifact = remote + "/" + filename
        receipt["delivery"].update(_upload_checked(
            artifact, info, local_hash, remote_artifact, sftp, bounded))
        if owned_selected:
            receipt["owned_launch"]["launcher"].update(_upload_checked(
                launcher, launcher_info, launcher_hash, remote + "/" + launcher_name, sftp, bounded))
            names = owned_tool.output_names(nonce)
            collect_names = collection_names(collect_names + list(names.values()))
            receipt["owned_launch"].update(nonce=nonce, output_names=names)
        for item, metadata in zip(inputs, receipt.get("inputs", [])):
            try:
                input_tool.unchanged(item, _no_aliases)
                metadata.update(_upload_checked(item.path, item.info, item.sha256,
                                               remote + "/" + item.name, sftp, bounded), state="UPLOADED")
            except input_tool.InputError as error:
                metadata["state"] = error.state
                raise SmokeError(error.state) from None
            except SmokeError as error:
                metadata["state"] = error.state
                raise

        def inventory(path, limit):
            items = []
            iterator = bounded(sftp.listdir_iter, path, 1)
            try:
                while True:
                    try:
                        attributes = bounded(next, iterator)
                    except StopIteration:
                        return items
                    if len(items) >= limit:
                        raise SmokeError("LAUNCH_INVENTORY_LIMIT")
                    mode = attributes.st_mode
                    kind = "file" if stat.S_ISREG(mode) else "directory" if stat.S_ISDIR(mode) else "other"
                    items.append({"name": attributes.filename, "kind": kind,
                                  "size_bytes": attributes.st_size})
            finally:
                iterator.close()

        if background_requested:
            background_canonical = _stage_editor_background(out, remote, sftp, bounded, inventory,
                                                            receipt["editor_background"])
        if locale_payload is not None:
            locale_entry = receipt["editor_locale"]
            locale_canonical = _owned_canonical(sftp, bounded, remote)
            locale_entry["canonical_directory"] = _editor_fixture_directory(
                sftp, bounded, remote, locale_canonical, profile_name, "EDITOR_PROFILE")
            if inventory(remote + "/" + profile_name, 1):
                raise SmokeError("EDITOR_PROFILE_NOT_EMPTY")
            _upload_editor_fixture(out / "editor-preferences.json",
                                   remote + "/" + locale_entry["relative_path"],
                                   locale_payload, sftp, bounded, locale_entry)
        entries = inventory(remote, 8)
        receipt["launch"]["inventory"] = entries
        expected = {filename: "file"}
        expected_sizes = {filename: info.st_size}
        for item in inputs:
            expected[item.name] = "file"
            expected_sizes[item.name] = item.info.st_size
        if launcher_name is not None:
            expected[launcher_name] = "file"
            expected_sizes[launcher_name] = launcher_info.st_size
        if args.kind == "editor":
            expected[profile_name] = "directory"
            if locale_payload is not None:
                _verify_editor_fixture(remote, locale_canonical, sftp, bounded, inventory, locale_entry,
                                       locale_payload, "EDITOR_PROFILE")
            elif inventory(remote + "/" + profile_name, 1):
                raise SmokeError("EDITOR_PROFILE_NOT_EMPTY")
            receipt["launch"]["profile_dir_fresh"] = True
        if background_requested:
            expected[".jce"] = "directory"
            _verify_editor_fixture(remote, background_canonical, sftp, bounded, inventory,
                                   receipt["editor_background"], EDITOR_BACKGROUND_SETTINGS, "EDITOR_SETTINGS")
        if (len(entries) != len(expected)
                or {entry["name"]: entry["kind"] for entry in entries} != expected
                or any(next((entry["size_bytes"] for entry in entries if entry["name"] == name), None) != size
                       for name, size in expected_sizes.items())):
            raise SmokeError("UNEXPECTED_LAUNCH_INVENTORY")
        receipt["launch"]["inventory_verified"] = True
        if collect_names:
            receipt["collection"] = {"state": "WAITING_PROCESS_EXIT", "verified": False,
                                     "requested": collect_names}
            canonical_owned = _owned_canonical(sftp, bounded, remote)
        exe = windows_owned + "\\" + filename
        if owned_selected:
            text = launch_command(windows_owned, windows_owned + "\\" + launcher_name, env,
                                  arguments=owned_tool.arguments(exe, windows_owned, nonce, args.timeout))
            receipt["launch"].update(wait_policy="OWNED_CHILD_HANDLE_REPORT_AFTER_CONTROLLER_WAIT",
                wait_scope="HELD_CHILD_HANDLE_PID_CREATION_FILETIME_AND_BOUNDED_WAIT",
                requested_desktop=owned_tool.DESKTOP, actual_child_desktop="UNOBSERVED")
        else:
            text = launch_command(windows_owned, exe, env)
        result = observe("artifact", text, args.timeout + 15 if owned_selected else args.timeout)
        receipt["launch"]["wait_contract_completed"] = result["state"] == "EXITED"
        receipt["remote_process_cleanup"] = "CHANNEL_CLOSED_ONLY_TERMINATION_UNVERIFIED"
        if result["state"] != "EXITED":
            raise SmokeError(result["state"])
        if result["output_truncated"]:
            raise SmokeError("OUTPUT_LIMIT")
        controller_exit = result["exit_code"]
        process_state = "PROCESS_PASSED"
        if controller_exit != 0:
            process_state = "OWNED_LAUNCHER_NONZERO_EXIT" if owned_selected else "NONZERO_EXIT"
        if collect_names:
            receipt["process_state"] = process_state
            try:
                receipt["collection"] = collect_outputs(
                    sftp, remote, canonical_owned, collect_names, out, args.transfer_timeout,
                    password, args.guest_encoding, clock=clock)
            except Exception:
                receipt["collection"] = {"state": "COLLECTION_IO_FAILURE", "verified": False,
                                         "requested": collect_names}
        if owned_selected:
            try:
                result, observation = owned_tool.observe(out, receipt["collection"], nonce=nonce,
                    target=exe, directory=windows_owned, max_output_bytes=args.max_output_bytes)
            except (ValueError, OSError):
                receipt["owned_launch"]["state"] = "REFUSED"
                raise SmokeError("OWNED_LAUNCH_REPORT_REFUSED") from None
            receipt["owned_launch"].update(observation, state="OBSERVED", controller_exit_code=controller_exit)
            receipt["child_process"] = {key: value for key, value in result.items() if key not in ("stdout", "stderr")}
            if observation["child_cleanup_observed"]:
                receipt["remote_process_cleanup"] = "OWNED_CHILD_REAPED_HANDLES_CLOSED_PROCESS_TREE_UNVERIFIED"
            process_state = "PROCESS_PASSED" if result["state"] == "EXITED" and result["exit_code"] == 0 else result["state"]
            if controller_exit != 0 and process_state == "PROCESS_PASSED":
                process_state = "OWNED_LAUNCHER_NONZERO_EXIT"
        if process_state == "PROCESS_PASSED" and marker is not None:
            marker_bytes = marker.encode("ascii")
            if marker_bytes not in (result["stdout"] + b"\n" + result["stderr"]).splitlines():
                process_state = "MISSING_EXPECTED_MARKER"
        if collect_names:
            receipt["process_state"] = process_state
        if process_state != "PROCESS_PASSED":
            raise SmokeError(process_state)
        if collect_names and not receipt["collection"]["verified"]:
            raise SmokeError("COLLECTION_FAILED")
        if terminal_request is not None:
            try:
                receipt["terminal_ack"] = _observe_terminal(terminal_request, result, out,
                    dict(stderr_redacted=False), receipt.get("collection"))
                receipt["terminal_ack_state"] = "OBSERVED"
            except SmokeError:
                receipt["terminal_ack_state"] = "REFUSED"
                raise
        receipt.update(state="SMOKE_PASSED", passed=True)
    except SmokeError as exc:
        receipt["state"] = exc.state
    except TimeoutError:
        receipt["state"] = "SSH_TIMEOUT"
    except Exception:
        # In particular, never stringify Paramiko/native authentication errors.
        receipt["state"] = "SSH_OR_LOCAL_FAILURE"
    finally:
        for owner, handle in (("sftp", sftp), ("client", client)):
            if handle is not None:
                try:
                    handle.close()
                except Exception:
                    if "collection" in receipt:
                        receipt.setdefault("close_failures", []).append(owner)
                        receipt["passed"] = False
                        if receipt["state"] == "SMOKE_PASSED":
                            receipt["state"] = "CLOSE_FAILURE"
                    else:
                        receipt.update(state="CLOSE_FAILURE", passed=False)
        receipt = _secret_free(receipt, password if isinstance(password, str) else "")
        body = json.dumps(receipt, indent=2, ensure_ascii=True) + "\n"
        (out / "receipt.json").write_text(body, encoding="utf-8", newline="\n")
    return json.loads(body)


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--user", required=True)
    parser.add_argument("--host-key-sha256", required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--expected-arch", choices=FLOORS, required=True)
    parser.add_argument("--kind", choices=("core-probe", "native-probe", "editor"), required=True)
    parser.add_argument("--export-receipt", type=Path, help="Required for native-probe and owned Editor launch: collector receipt bound to this host/user/pin/architecture")
    parser.add_argument("--owned-launcher", type=Path,
                        help="Editor only: explicit XP native helper requesting WinSta0\\Default and reporting its held child")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, help="Explicit strict child of repository build/; default build/validation")
    parser.add_argument("--remote-dir", help="SFTP path to the guest Desktop; mapping is verified before EXE upload")
    parser.add_argument("--password-env", help="Read a password from this environment key; otherwise prompt without echo")
    parser.add_argument("--env", action="append", default=[], metavar="JCE_NAME=VALUE")
    parser.add_argument("--require-terminal-ack", action="store_true",
                        help="Editor only: require this run's exact Engine terminal acknowledgement after successful exit")
    parser.add_argument("--editor-window", choices=("hidden", "visible", "interactive"), default=None,
                        help="Editor only: hidden by default; visible is non-focusable; interactive permits human focus")
    parser.add_argument("--editor-locale", choices=EDITOR_LOCALES,
                        help="Editor only: seed this locale in the fresh isolated Editor profile")
    parser.add_argument("--editor-run-in-background", action="store_true",
                        help="Editor only: stage the public run_in_background=true project setting in this fresh owned working directory")
    parser.add_argument("--input-relative", action="append", default=[], metavar="BASENAME=LOCAL_PATH",
                        help="Upload an explicit flat data file; at most 4 files, 8 MiB each/16 MiB total")
    parser.add_argument("--collect-relative", action="append", default=[], metavar="FILENAME",
                        help="Collect this flat owned-run filename after exit; at most 16 files/64 MiB total")
    parser.add_argument("--expect-marker", help="Require this exact complete stdout/stderr line; mandatory for native-probe, core-probe defaults to A1 marker")
    parser.add_argument("--guest-encoding", default="utf-8")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--connect-timeout", type=float, default=15)
    parser.add_argument("--probe-timeout", type=float, default=15)
    parser.add_argument("--transfer-timeout", type=float, default=120)
    parser.add_argument("--max-output-bytes", type=int, default=1024 * 1024)
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.password_env:
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", args.password_env):
                raise SmokeError("INVALID_CREDENTIAL_ENV_NAME")
            password = os.environ.get(args.password_env, "")
        else:
            import getpass
            password = getpass.getpass("SSH password: ")
        receipt = run_smoke(args, password)
    except SmokeError as exc:
        print("xp-vm: " + exc.state, file=sys.stderr)
        return 1
    except (Exception, KeyboardInterrupt):
        print("xp-vm: FAILED (diagnostic text suppressed to protect credentials)", file=sys.stderr)
        return 1
    print("xp-vm: " + receipt["state"] + "; receipt stored.")
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())

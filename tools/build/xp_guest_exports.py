#!/usr/bin/env python3
"""Collect exact XP system-DLL export evidence through pinned read-only SFTP.

Only explicit approved DLL basenames and their export-forwarder closure are
read. No guest file is created or changed; metadata probes use existing cmd/reg.
Collected DLLs are never loaded. This is static guest-image evidence, never
Editor or graphical qualification.
"""
from __future__ import annotations

import argparse
import codecs
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
FLOORS = {"x86": (5, 1), "x64": (5, 2)}
MACHINES = {"x86": (0x14c, 0x10b), "x64": (0x8664, 0x20b)}
MAX_DLLS = 512
MAX_DLL_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024


class CollectionError(ValueError):
    """Finite diagnostics only; transport exception text may contain secrets."""
    def __init__(self, state):
        super().__init__(state)
        self.state = state


def _module(name):
    spec = importlib.util.spec_from_file_location("jce_xp_exports_" + name,
                                                 Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _allowed_dlls():
    return {name + ".dll" for name in _module("xp_editor").XP_SYSTEM_DLLS}


def _dll_name(value, allowed):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_]+\.dll", value, re.I):
        raise CollectionError("INVALID_SYSTEM_DLL_BASENAME")
    value = value.lower()
    if value not in allowed:
        raise CollectionError("DLL_OUTSIDE_XP_SYSTEM_POLICY")
    return value


def _forwarder(value, allowed):
    match = re.fullmatch(r"([A-Za-z0-9_]+)(?:\.dll)?\.(.+)", value, re.I)
    if not match:
        raise CollectionError("MALFORMED_EXPORT_FORWARDER")
    dll = _dll_name(match[1] + ".dll", allowed)
    symbol = match[2]
    if symbol.startswith("#"):
        if not re.fullmatch(r"#[1-9][0-9]{0,4}", symbol) or int(symbol[1:]) > 65535:
            raise CollectionError("INVALID_FORWARDED_ORDINAL")
        symbol = int(symbol[1:])
    elif any(ch in symbol for ch in "\\/\r\n\x00"):
        raise CollectionError("MALFORMED_EXPORT_FORWARDER")
    return {"dll": dll, "symbol": symbol}


def parse_exports(blob, arch, expected_dll, allowed=None):
    """Bounded PE32/PE32+ export-directory reader; never loads a DLL.

    Sparse EAT slots are excluded. Every named alias and actual ordinal of a
    forwarded slot receives its own mapping, matching xp_editor.audit_pe.
    """
    allowed = _allowed_dlls() if allowed is None else allowed
    expected_dll = _dll_name(expected_dll, allowed)
    if arch not in MACHINES or not 64 <= len(blob) <= MAX_DLL_BYTES:
        raise CollectionError("INVALID_DLL_SIZE_OR_ARCH")

    def unpack(fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(blob):
            raise CollectionError("TRUNCATED_PE_STRUCTURE")
        return struct.unpack_from(fmt, blob, offset)

    if blob[:2] != b"MZ":
        raise CollectionError("INVALID_PE_SIGNATURE")
    pe, = unpack("<I", 0x3c)
    if not 64 <= pe <= 1024 * 1024 or blob[pe:pe + 4] != b"PE\0\0":
        raise CollectionError("INVALID_PE_SIGNATURE")
    machine, section_count = unpack("<HH", pe + 4)
    optional_size, characteristics = unpack("<HH", pe + 20)
    optional = pe + 24
    magic, = unpack("<H", optional)
    if (machine, magic) != MACHINES[arch]:
        raise CollectionError("SYSTEM_DLL_ARCH_MISMATCH")
    if not characteristics & 0x2000 or not 1 <= section_count <= 96:
        raise CollectionError("INVALID_SYSTEM_DLL_HEADER")
    base = 112 if magic == 0x20b else 96
    if optional_size < base + 8 or optional + optional_size > len(blob):
        raise CollectionError("TRUNCATED_PE_OPTIONAL_HEADER")
    image_size, headers_size = unpack("<II", optional + 56)
    if (not 0 < headers_size <= len(blob) or not headers_size <= image_size <= 1024 * 1024 * 1024
            or optional + optional_size + section_count * 40 > len(blob)):
        raise CollectionError("INVALID_PE_IMAGE_OR_SECTION_HEADERS")
    count, = unpack("<I", optional + base - 4)
    if not 1 <= count <= (optional_size - base) // 8:
        raise CollectionError("INVALID_PE_DIRECTORY_COUNT")
    export_rva, export_size = unpack("<II", optional + base)
    if not export_rva or export_size < 40:
        raise CollectionError("MISSING_EXPORT_DIRECTORY")
    sections = []
    for index in range(section_count):
        virtual_size, address, raw_size, raw = unpack("<IIII", optional + optional_size + index * 40 + 8)
        if raw_size and (raw < 0 or raw + raw_size > len(blob)):
            raise CollectionError("TRUNCATED_PE_SECTION")
        if address + max(virtual_size, raw_size) > image_size or raw_size and raw < headers_size:
            raise CollectionError("INVALID_PE_SECTION_EXTENT")
        sections.append((address, max(virtual_size, raw_size), raw, raw_size))
    ordered = sorted(sections)
    if any(first[0] + first[1] > second[0] for first, second in zip(ordered, ordered[1:])):
        raise CollectionError("AMBIGUOUS_PE_SECTION_MAPPING")

    def span(address, needed=1):
        matches = [(raw + address - va, raw + size) for va, extent, raw, size in sections
                   if 0 <= address - va < extent and address - va + needed <= size]
        if len(matches) != 1:
            raise CollectionError("EXPORT_RVA_NOT_FILE_BACKED")
        offset, limit = matches[0]
        if offset < 0 or offset + needed > min(limit, len(blob)):
            raise CollectionError("EXPORT_RVA_NOT_FILE_BACKED")
        return offset, min(limit, len(blob))

    def text(address, maximum=4096):
        offset, limit = span(address)
        end = blob.find(b"\0", offset, min(limit, offset + maximum))
        if end < 0:
            raise CollectionError("UNTERMINATED_EXPORT_STRING")
        try:
            value = blob[offset:end].decode("ascii", "strict")
        except UnicodeError:
            raise CollectionError("INVALID_EXPORT_STRING") from None
        if not value:
            raise CollectionError("EMPTY_EXPORT_STRING")
        return value

    export_offset, _ = span(export_rva, export_size)
    row = unpack("<IIHHIIIIIII", export_offset)
    name_rva, ordinal_base, function_count, name_count, functions_rva, names_rva, ordinals_rva = row[4:]
    if text(name_rva, 260).lower() != expected_dll:
        raise CollectionError("SYSTEM_DLL_EXPORT_NAME_MISMATCH")
    if (not 1 <= function_count <= 65535 or name_count > 65535 or ordinal_base < 1
            or ordinal_base + function_count - 1 > 65535):
        raise CollectionError("EXPORT_TABLE_BUDGET_OR_ORDINAL_RANGE")
    functions, _ = span(functions_rva, function_count * 4)
    aliases = {}
    seen_names = set()
    if name_count:
        names, _ = span(names_rva, name_count * 4)
        ordinals, _ = span(ordinals_rva, name_count * 2)
        for index in range(name_count):
            slot, = unpack("<H", ordinals + index * 2)
            address, = unpack("<I", names + index * 4)
            name = text(address)
            if slot >= function_count or name in seen_names:
                raise CollectionError("INVALID_OR_DUPLICATE_EXPORT_NAME")
            seen_names.add(name)
            aliases.setdefault(slot, []).append(name)
    exported_ordinals, forwarders, holes = [], {}, 0
    for slot in range(function_count):
        address, = unpack("<I", functions + slot * 4)
        if not address:
            holes += 1
            if slot in aliases:
                raise CollectionError("EXPORT_NAME_POINTS_TO_HOLE")
            continue
        ordinal = ordinal_base + slot
        exported_ordinals.append(ordinal)
        if str(ordinal) in seen_names and str(ordinal) not in aliases.get(slot, []):
            raise CollectionError("UNREPRESENTABLE_EXPORT_ALIAS_COLLISION")
        if export_rva <= address < export_rva + export_size:
            target = _forwarder(text(address, min(4096, export_rva + export_size - address)), allowed)
            for key in [str(ordinal), *aliases.get(slot, [])]:
                if key in forwarders and forwarders[key] != target:
                    raise CollectionError("UNREPRESENTABLE_FORWARDER_ALIAS_COLLISION")
                forwarders[key] = target
        elif not any(va <= address < va + extent for va, extent, _raw, _size in sections):
            raise CollectionError("EXPORT_ADDRESS_OUTSIDE_IMAGE")
    return {"sha256": hashlib.sha256(blob).hexdigest(), "names": sorted(seen_names),
            "ordinals": exported_ordinals, "forwarders": dict(sorted(forwarders.items())),
            "pe": {"machine": hex(machine), "magic": hex(magic), "export_dll": expected_dll,
                   "eat_slots": function_count, "holes_excluded": holes}}


def validate_forwarders(entries):
    def visit(dll, symbol, seen):
        token = (dll, type(symbol).__name__, symbol)
        if token in seen or len(seen) >= 32:
            raise CollectionError("CYCLIC_OR_UNBOUNDED_EXPORT_FORWARDER")
        entry = entries.get(dll)
        if entry is None or symbol not in (entry["ordinals"] if type(symbol) is int else entry["names"]):
            raise CollectionError("FORWARDED_EXPORT_MISSING")
        target = entry["forwarders"].get(str(symbol))
        if target is not None:
            visit(target["dll"], target["symbol"], seen | {token})
    for dll, entry in entries.items():
        for key in entry["forwarders"]:
            symbol = int(key) if key.isdecimal() and int(key) in entry["ordinals"] else key
            visit(dll, symbol, set())


def _windows_mapping(value):
    normalized = value.replace("\\", "/").rstrip("/")
    match = re.fullmatch(r"/cygdrive/([A-Za-z])/(.+)", normalized)
    if match:
        return match[1].upper() + ":/" + match[2]
    match = re.fullmatch(r"/([A-Za-z])/(.+)", normalized)
    if match:
        return match[1].upper() + ":/" + match[2]
    if re.match(r"^/[A-Za-z]:/", normalized):
        normalized = normalized[1:]
    if not re.match(r"^[A-Za-z]:/", normalized):
        raise CollectionError("SYSTEM32_MAPPING_UNVERIFIED")
    return normalized


def _credential_bytes(raw, password, encoding):
    return any(password.encode(codec, "ignore") in raw for codec in {"utf-8", "utf-16le", encoding}
               if password.encode(codec, "ignore"))


def _read_dll(vm, sftp, directory, name, bounded, budget, observation, password, encoding, retain_dlls):
    path = directory + "/" + name
    before = vm._remote_metadata(bounded(sftp.lstat, path))
    if not stat.S_ISREG(before["mode"]) or not 0 < before["size_bytes"] <= budget["per_file"]:
        raise CollectionError("INVALID_SYSTEM_DLL_FILE_OR_SIZE")
    if 2 * before["size_bytes"] > budget["maximum"] - budget["read"]:
        raise CollectionError("TOTAL_TRANSFER_BYTE_LIMIT")
    canonical = vm._sftp_path(bounded(sftp.normalize, path))
    if canonical.casefold() != path.casefold():
        raise CollectionError("SYSTEM_DLL_PATH_ALIAS")
    observation.update(path=path, before=before, canonical_before=canonical)
    source = None
    raw = bytearray()
    digests = []
    primary = None
    try:
        source = bounded(sftp.open, path, "rb")
        held = vm._remote_metadata(bounded(source.stat))
        if held != before:
            raise CollectionError("SYSTEM_DLL_HANDLE_IDENTITY_CHANGED")
        for repeat in range(2):
            if repeat:
                bounded(source.seek, 0)
            digest = hashlib.sha256()
            remaining = before["size_bytes"]
            while remaining:
                requested = min(1024 * 1024, remaining)
                chunk = bounded(source.read, requested)
                if not isinstance(chunk, bytes) or not chunk or len(chunk) > requested:
                    raise CollectionError("SYSTEM_DLL_CHANGED_OR_INVALID_READ")
                budget["read"] += len(chunk)
                remaining -= len(chunk)
                digest.update(chunk)
                if not repeat:
                    raw.extend(chunk)
            extra = bounded(source.read, 1)
            budget["read"] += len(extra)
            if extra:
                raise CollectionError("SYSTEM_DLL_GREW_DURING_READ")
            digests.append(digest.hexdigest())
        if digests[0] != digests[1]:
            raise CollectionError("SYSTEM_DLL_CONTENT_CHANGED")
        if vm._remote_metadata(bounded(source.stat)) != before:
            raise CollectionError("SYSTEM_DLL_HANDLE_CHANGED")
    except BaseException as error:
        primary = error
        raise
    finally:
        if source is not None:
            try:
                bounded(source.close)
            except Exception:
                observation["close_failed"] = True
                if primary is None:
                    raise CollectionError("SYSTEM_DLL_CLOSE_FAILURE") from None
    after = vm._remote_metadata(bounded(sftp.lstat, path))
    root = vm._sftp_path(bounded(sftp.normalize, directory))
    canonical_after = vm._sftp_path(bounded(sftp.normalize, path))
    if before != after or root.casefold() != directory.casefold() or canonical_after.casefold() != path.casefold():
        raise CollectionError("SYSTEM_DLL_PATH_OR_METADATA_CHANGED")
    if retain_dlls and _credential_bytes(raw, password, encoding):
        raise CollectionError("CREDENTIAL_BYTES_IN_BINARY_CAPTURE")
    observation.update(after=after, canonical_after=canonical_after,
                       sha256=digests[0], verification_sha256=digests[1], bytes=len(raw), reads=2)
    return bytes(raw)


def _requested(args, allowed, vm):
    names = {_dll_name(value, allowed) for value in args.dll}
    artifact = None
    if args.artifact is not None:
        path = Path(args.artifact).absolute()
        vm._no_aliases(path)
        before = path.stat()
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= vm.MAX_ARTIFACT_BYTES:
            raise CollectionError("INVALID_IMPORT_ARTIFACT")
        vm.pe_identity(path, args.expected_arch)
        raw = path.read_bytes()
        if vm._file_identity(path.stat()) != vm._file_identity(before):
            raise CollectionError("IMPORT_ARTIFACT_CHANGED")
        records = _module("standalone")._pe_import_records(raw)
        names.update(_dll_name(row["dll"], allowed) for row in records)
        artifact = {"path": str(path), "sha256": hashlib.sha256(raw).hexdigest(), "imports": records}
    if not names:
        raise CollectionError("EXPLICIT_DLL_OR_IMPORT_ARTIFACT_REQUIRED")
    return sorted(names), artifact


def _inventory_text_has_credential(entries, guest, password):
    # Hashes and numeric ordinals are computed public identity data. Coincidental
    # digit sequences in those values are not credential-bearing source text.
    values = [json.dumps(guest, ensure_ascii=False)]
    for name, entry in entries.items():
        values.extend([name, *entry["names"]])
        for target in entry["forwarders"].values():
            values.append(target["dll"])
            if isinstance(target["symbol"], str):
                values.append(target["symbol"])
    return any(password in value for value in values)


def run_collection(args, password, *, repo=ROOT, client_factory=None,
                   clock=time.monotonic, sleep=time.sleep):
    vm = _module("xp_vm")
    if not isinstance(password, str) or not password:
        raise CollectionError("EMPTY_CREDENTIAL")
    if password in str(args.out):
        raise CollectionError("CREDENTIAL_IN_OUTPUT_PATH")
    out = vm.output_path(args.out, args.output_root, repo)
    out.mkdir(parents=True, exist_ok=False)
    receipt = {"schema": "jce.xp.guest-export-collection.v1", "state": "NOT_COLLECTED", "collected": False,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(), "host": args.host, "user": args.user,
        "expected_arch": args.expected_arch, "commands": {}, "files": [], "editor_qualified": False,
        "path_observation": "SFTP_LSTAT_REALPATH_FSTAT_AND_TWO_HASHES_NOT_ATOMIC",
        "inventory": None, "close_failures": [], "raw_capture_mode": "retain-dlls" if args.retain_dlls else "inventory-only",
        "raw_capture_retained": False}
    client = sftp = None
    entries = {}
    started = clock()
    budget = {"read": 0, "maximum": args.max_total_bytes, "per_file": args.max_dll_bytes}

    def remaining(maximum):
        value = min(maximum, args.timeout - (clock() - started))
        if value <= 0:
            raise CollectionError("COLLECTION_TIMEOUT")
        return value

    def bounded(call, *values):
        return vm._control_call(lambda: call(*values), remaining(args.transfer_timeout), client.close,
                                clock=clock, sleep=sleep)

    def probe(name, command, optional=False):
        result = vm.command(client, command, remaining(args.probe_timeout), args.max_output_bytes,
                            clock=clock, sleep=sleep)
        receipt["commands"][name] = {key: value for key, value in result.items() if key not in ("stdout", "stderr")}
        if _credential_bytes(result["stdout"] + result["stderr"], password, args.guest_encoding):
            raise CollectionError("CREDENTIAL_IN_GUEST_METADATA")
        if result["state"] != "EXITED" or result["output_truncated"]:
            raise CollectionError("GUEST_METADATA_UNAVAILABLE")
        if result["exit_code"]:
            if optional:
                return None
            raise CollectionError("GUEST_METADATA_NONZERO_EXIT")
        value = result["stdout"].decode(args.guest_encoding, "strict").strip()
        receipt["commands"][name]["stdout"] = value
        return value

    try:
        if (not 1 <= args.port <= 65535 or not 1 <= args.timeout <= 600
                or not 1 <= args.connect_timeout <= 60 or not 1 <= args.probe_timeout <= 60
                or not 1 <= args.transfer_timeout <= 120 or not 1 <= args.max_output_bytes <= 1024 * 1024
                or not 1 <= args.max_dlls <= MAX_DLLS or not 1 <= args.max_dll_bytes <= MAX_DLL_BYTES
                or not 1 <= args.max_total_bytes <= MAX_TOTAL_BYTES):
            raise CollectionError("INVALID_COLLECTION_BUDGET")
        codecs.lookup(args.guest_encoding)
        allowed = _allowed_dlls()
        requested, artifact = _requested(args, allowed, vm)
        receipt.update(requested=requested, artifact=artifact)
        remote = vm._sftp_path(args.remote_system32)
        pin = vm.PinnedHostKey(args.host_key_sha256)
        receipt["host_key_sha256"] = pin.expected
        client = (client_factory or vm._new_client)()
        client.set_missing_host_key_policy(pin)
        vm._control_call(lambda: client.connect(hostname=args.host, port=args.port, username=args.user,
            password=password, timeout=remaining(args.connect_timeout), banner_timeout=args.connect_timeout,
            auth_timeout=args.connect_timeout, channel_timeout=args.connect_timeout,
            allow_agent=False, look_for_keys=False), remaining(args.connect_timeout), client.close,
            clock=clock, sleep=sleep)
        pin.verify(client.get_transport().get_remote_server_key())
        version = probe("ver", "cmd /d /q /c ver")
        found = re.search(r"(?<!\d)(\d+)\.(\d+)\.(\d+)(?!\d)", version)
        if not found or tuple(map(int, found.groups()[:2])) != FLOORS[args.expected_arch]:
            raise CollectionError("GUEST_OS_VERSION_MISMATCH")
        architecture = probe("architecture", "cmd /d /q /c echo %PROCESSOR_ARCHITECTURE%")
        if architecture.upper() != {"x86": "X86", "x64": "AMD64"}[args.expected_arch]:
            raise CollectionError("GUEST_ARCHITECTURE_MISMATCH")
        system_root = vm._windows_path(probe("system-root", "cmd /d /q /c echo %SystemRoot%"))
        registry = {}
        for name in ("ProductName", "CurrentVersion", "CurrentBuildNumber", "CSDVersion"):
            value = probe("registry-" + name, 'cmd /d /q /c reg query "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion" /v ' + name,
                          optional=name == "CSDVersion")
            if value is None:
                registry[name] = None
                continue
            match = re.search(r"(?m)^\s*" + name + r"\s+REG_(?:SZ|EXPAND_SZ)\s+(.*?)\s*$", value)
            if not match:
                raise CollectionError("GUEST_REGISTRY_IDENTITY_UNAVAILABLE")
            registry[name] = match[1]
        if (not re.search(r"\bWindows\s+XP\b", registry["ProductName"], re.I)
                or registry["CurrentVersion"] != ".".join(found.groups()[:2])
                or registry["CurrentBuildNumber"] != found[3]):
            raise CollectionError("GUEST_REGISTRY_IDENTITY_MISMATCH")
        receipt["guest"] = {"arch": args.expected_arch, "os_version": list(FLOORS[args.expected_arch]),
            "version_observation": version, "processor_architecture": architecture,
            "system_root": system_root, "registry": registry}
        sftp = vm._control_call(client.open_sftp, remaining(args.connect_timeout), client.close,
                                clock=clock, sleep=sleep)
        sftp.get_channel().settimeout(remaining(args.transfer_timeout))
        directory_info = bounded(sftp.lstat, remote)
        if not stat.S_ISDIR(directory_info.st_mode):
            raise CollectionError("SYSTEM32_MAPPING_NOT_DIRECTORY")
        canonical = vm._sftp_path(bounded(sftp.normalize, remote))
        expected = system_root.replace("\\", "/") + "/System32"
        if (canonical.casefold() != remote.casefold()
                or _windows_mapping(canonical).casefold() != expected.casefold()):
            raise CollectionError("SYSTEM32_MAPPING_MISMATCH")
        receipt["mapping"] = {"requested_sftp": remote, "canonical_sftp": canonical,
                              "observed_windows": expected, "kernel32_arch_verified": False}
        pending = ["kernel32.dll", *[name for name in requested if name != "kernel32.dll"]]
        copied = out / "dlls"
        if args.retain_dlls:
            copied.mkdir()
        while pending:
            name = pending.pop(0)
            if name in entries:
                continue
            if len(entries) >= args.max_dlls:
                raise CollectionError("SYSTEM_DLL_COUNT_LIMIT")
            observation = {"dll": name, "state": "READING", "raw_capture_retained": False}
            receipt["files"].append(observation)
            raw = _read_dll(vm, sftp, canonical, name, bounded, budget, observation, password, args.guest_encoding, args.retain_dlls)
            entry = parse_exports(raw, args.expected_arch, name, allowed)
            if args.retain_dlls:
                target = copied / name
                try:
                    with target.open("xb") as destination:
                        destination.write(raw)
                    if hashlib.sha256(target.read_bytes()).hexdigest() != entry["sha256"]:
                        raise CollectionError("LOCAL_DLL_CAPTURE_CHANGED")
                except CollectionError:
                    raise
                except Exception:
                    raise CollectionError("LOCAL_DLL_CAPTURE_WRITE_FAILURE") from None
                observation.update(raw_capture_retained=True, local_relative="dlls/" + name)
            observation.update(state="COLLECTED", pe=entry["pe"])
            entries[name] = entry
            if name == "kernel32.dll":
                receipt["mapping"]["kernel32_arch_verified"] = True
            for target_dll in sorted({target["dll"] for target in entry["forwarders"].values()}):
                if target_dll not in entries and target_dll not in pending:
                    pending.append(target_dll)
        validate_forwarders(entries)
        receipt["state"] = "GUEST_EXPORTS_COLLECTED"
    except CollectionError as error:
        receipt["state"] = error.state
    except vm.SmokeError as error:
        receipt["state"] = error.state
    except TimeoutError:
        receipt["state"] = "COLLECTION_TIMEOUT"
    except (Exception, KeyboardInterrupt):
        receipt["state"] = "SSH_OR_LOCAL_COLLECTION_FAILURE"
    finally:
        for label, handle in (("sftp", sftp), ("client", client)):
            if handle is not None:
                try:
                    vm._control_call(handle.close, min(args.connect_timeout, 5), client.close,
                                     clock=clock, sleep=sleep)
                except Exception:
                    receipt["close_failures"].append(label)
        receipt["close_failures"].extend("dll:" + row["dll"] for row in receipt["files"] if row.get("close_failed"))
        if receipt["close_failures"]:
            receipt["primary_state"] = receipt["state"]
            if receipt["state"] == "GUEST_EXPORTS_COLLECTED":
                receipt["state"] = "COLLECTION_CLOSE_FAILURE"
        receipt["cleanup_state"] = "CLOSE_FAILURE" if receipt["close_failures"] else "CLOSED"
        receipt["raw_capture_retained"] = any(row["raw_capture_retained"] for row in receipt["files"])
        receipt["transfer_bytes"] = budget["read"]
        receipt["limits"] = {"dlls": args.max_dlls, "dll_bytes": args.max_dll_bytes,
                              "total_transfer_bytes": args.max_total_bytes, "seconds": args.timeout}
        if receipt["state"] == "GUEST_EXPORTS_COLLECTED":
            inventory = {"schema": "jce.xp.guest-exports.v1", "guest": receipt["guest"],
                         "dlls": dict(sorted(entries.items()))}
            encoded = (json.dumps(inventory, indent=2, ensure_ascii=True) + "\n").encode()
            if len(encoded) > 16 * 1024 * 1024 or _inventory_text_has_credential(entries, receipt["guest"], password):
                receipt["state"] = "INVENTORY_SIZE_OR_CREDENTIAL_REFUSAL"
            else:
                try:
                    with (out / "inventory.json.tmp").open("xb") as destination:
                        destination.write(encoded)
                    os.replace(out / "inventory.json.tmp", out / "inventory.json")
                    receipt.update(collected=True, inventory={"local_relative": "inventory.json",
                        "sha256": hashlib.sha256(encoded).hexdigest(), "dlls": len(entries)})
                except Exception:
                    receipt["state"] = "INVENTORY_PUBLICATION_FAILURE"
        receipt = vm._secret_free(receipt, password)
        (out / "receipt.json.tmp").write_text(json.dumps(receipt, indent=2, ensure_ascii=True) + "\n",
                                               encoding="utf-8", newline="\n")
        os.replace(out / "receipt.json.tmp", out / "receipt.json")
    return receipt


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--user", required=True)
    parser.add_argument("--host-key-sha256", required=True)
    parser.add_argument("--expected-arch", choices=FLOORS, required=True)
    parser.add_argument("--remote-system32", required=True, help="exact canonical SFTP mapping of observed SystemRoot/System32")
    parser.add_argument("--password-env", required=True, help="runtime environment variable; no password CLI value or prompt")
    parser.add_argument("--dll", action="append", default=[], help="explicit XP system DLL basename; may repeat")
    parser.add_argument("--artifact", type=Path, help="derive additional DLLs from this exact Editor's ordinary/delayed imports")
    parser.add_argument("--retain-dlls", action="store_true",
                        help="also retain verified raw DLLs; credential-byte matches deny raw retention")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--guest-encoding", default="utf-8")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--connect-timeout", type=float, default=15)
    parser.add_argument("--probe-timeout", type=float, default=15)
    parser.add_argument("--transfer-timeout", type=float, default=60)
    parser.add_argument("--max-output-bytes", type=int, default=65536)
    parser.add_argument("--max-dlls", type=int, default=MAX_DLLS)
    parser.add_argument("--max-dll-bytes", type=int, default=MAX_DLL_BYTES)
    parser.add_argument("--max-total-bytes", type=int, default=MAX_TOTAL_BYTES)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", args.password_env):
            raise CollectionError("INVALID_CREDENTIAL_ENV_NAME")
        receipt = run_collection(args, os.environ.get(args.password_env, ""))
    except CollectionError as error:
        print("xp-guest-exports: " + error.state, file=sys.stderr)
        return 1
    except (Exception, KeyboardInterrupt):
        print("xp-guest-exports: COLLECTION_FAILED", file=sys.stderr)
        return 1
    print("xp-guest-exports: " + receipt["state"])
    return 0 if receipt["collected"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

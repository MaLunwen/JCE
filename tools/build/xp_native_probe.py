"""Collector-bound static import admission for explicit XP native probes.

No transport, credentials or native execution. Captures are local observations,
not signatures or atomic filesystem snapshots; runtime context stays unqualified.
"""
from __future__ import annotations
from datetime import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import stat

MAX_RECEIPT_BYTES = 4 * 1024 * 1024
MAX_INVENTORY_BYTES = 16 * 1024 * 1024


def _module(name):
    spec = importlib.util.spec_from_file_location("jce_native_probe_private_" + name,
                                                 Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def _ordinary(path):
    path = Path(path).absolute()
    if ".." in path.parts:
        raise ValueError("native export evidence path traversal")
    for item in (path, *path.parents):
        if item.exists() or item.is_symlink():
            value = item.lstat()
            if stat.S_ISLNK(value.st_mode) or getattr(value, "st_file_attributes", 0) & 0x400:
                raise ValueError("native export evidence alias")
    return path.resolve()


def _identity(value):
    return tuple(getattr(value, name, None) for name in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns"))


def _read(path, limit):
    path = _ordinary(path)
    before = path.stat()
    if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= limit:
        raise ValueError("native export evidence type/size refusal")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    after = path.stat()
    if len(data) != before.st_size or len(data) > limit or _identity(before) != _identity(after):
        raise ValueError("native export evidence changed during read")
    _ordinary(path)
    return data, _identity(after)


def _json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate native export evidence JSON key")
            result[key] = value
        return result
    value = json.loads(data.decode("utf-8"), object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise ValueError("native export evidence must be an object")
    return value


def declared_target(export_receipt, arch):
    """Read a selected build target declaration; transport must bind it to args."""
    raw, _ = _read(export_receipt, MAX_RECEIPT_BYTES)
    receipt = _json(raw)
    if receipt.get("expected_arch") != arch:
        raise ValueError("collector architecture mismatch")
    return {key: receipt.get(key) for key in ("host", "user", "host_key_sha256")}


def audit_pe(exe, arch, export_receipt, *, host, user, host_key_sha256):
    """Require a successful collector receipt bound to the exact target inputs."""
    if export_receipt is None:
        raise ValueError("native probe requires a collector export receipt")
    vm = _module("xp_vm")
    if arch not in vm.FLOORS:
        raise ValueError("unsupported native probe architecture")
    receipt_path = _ordinary(export_receipt)
    raw, receipt_identity = _read(receipt_path, MAX_RECEIPT_BYTES)
    receipt = _json(raw)
    if (receipt.get("schema") != "jce.xp.guest-export-collection.v1"
            or receipt.get("state") != "GUEST_EXPORTS_COLLECTED" or receipt.get("collected") is not True):
        raise ValueError("native probe requires a successful collected export receipt")
    if (not isinstance(host, str) or not host or not isinstance(user, str) or not user
            or receipt.get("host") != host or receipt.get("user") != user or receipt.get("expected_arch") != arch):
        raise ValueError("collector target host/user/architecture mismatch")
    if not isinstance(host_key_sha256, str) or not isinstance(receipt.get("host_key_sha256"), str):
        raise ValueError("collector pinned host key is missing")
    pin = vm.fingerprint(host_key_sha256)
    if vm.fingerprint(receipt.get("host_key_sha256", "")) != pin:
        raise ValueError("collector pinned host key mismatch")
    timestamp = receipt.get("timestamp_utc")
    if not isinstance(timestamp, str) or not timestamp or datetime.fromisoformat(timestamp.replace("Z", "+00:00")).tzinfo is None:
        raise ValueError("collector timestamp is missing or unqualified")
    guest = receipt.get("guest")
    if not isinstance(guest, dict) or guest.get("arch") != arch or guest.get("os_version") != list(vm.FLOORS[arch]):
        raise ValueError("collector guest OS/architecture mismatch")
    metadata = receipt.get("inventory")
    if (not isinstance(metadata, dict) or metadata.get("local_relative") != "inventory.json"
            or not isinstance(metadata.get("sha256"), str)
            or not re.fullmatch(r"[0-9a-f]{64}", metadata["sha256"])):
        raise ValueError("collector inventory identity/path mismatch")
    inventory_path = _ordinary(receipt_path.parent / "inventory.json")
    inventory_raw, inventory_identity = _read(inventory_path, MAX_INVENTORY_BYTES)
    inventory_hash = hashlib.sha256(inventory_raw).hexdigest()
    if inventory_hash != metadata["sha256"]:
        raise ValueError("collector inventory hash mismatch")
    inventory = _json(inventory_raw)
    if inventory.get("guest") != receipt.get("guest"):
        raise ValueError("collector/inventory guest identity mismatch")
    exe = _ordinary(exe)
    before = exe.stat()
    if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= vm.MAX_ARTIFACT_BYTES:
        raise ValueError("native artifact type/size refusal")
    vm.pe_identity(exe, arch)
    audit = _module("xp_editor").audit_pe(exe, arch, inventory_path, require_gui=False)
    if audit.get("state") != "STATIC_IMPORT_AUDIT_PASSED" or audit.get("inventory_sha256") != inventory_hash:
        raise ValueError("native probe complete guest import audit did not pass")
    if _identity(before) != _identity(exe.stat()):
        raise ValueError("native artifact changed during audit")
    final_receipt, final_receipt_identity = _read(receipt_path, MAX_RECEIPT_BYTES)
    final_inventory, final_inventory_identity = _read(inventory_path, MAX_INVENTORY_BYTES)
    if (final_receipt != raw or final_inventory != inventory_raw or final_receipt_identity != receipt_identity
            or final_inventory_identity != inventory_identity):
        raise ValueError("native export evidence changed during audit")
    return {"state": "STATIC_IMPORT_AUDIT_PASSED", "pe": audit,
        "collector": {"path": str(receipt_path), "sha256": hashlib.sha256(raw).hexdigest(),
            "timestamp_utc": timestamp, "host": host, "user": user, "expected_arch": arch, "host_key_sha256": pin},
        "inventory": {"path": str(inventory_path), "sha256": inventory_hash, "bytes": len(inventory_raw)},
        "graphics_context_verified": False, "physical_driver_verified": False, "interactive_graphics_verified": False,
        "observation_scope": "Bounded local before/after reads, not an atomic filesystem snapshot or signature"}

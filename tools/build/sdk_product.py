"""Installed SDK file identity, not compiler ABI, execution or a builder payload.

The SDK producer must hold its existing SDK cache/install lease. Publication is
atomic for this one manifest, not a transaction or power-loss guarantee for the
whole SDK. Verification performs only reads and rejects observed source changes.
"""
from __future__ import annotations

import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile

PRODUCT_FILE = "jce-sdk-product.json"
MAX_FILES = 16384
MAX_DIRECTORIES = 4096
_SOURCE_VERSION_CAP = 256 * 1024
_INSTALL_LIST_CAP = 8 * 1024 * 1024
MAX_MANIFEST_BYTES = 8 * 1024 * 1024
_METADATA_CAP = 16384
_CHUNK_BYTES = 1024 * 1024
_SCOPE = "installed-sdk-file-identity"
_FIELDS = frozenset(("schema", "scope", "jce_version", "api_version", "sdk_target",
    "delivery_profile", "required_paths", "files", "build_payload_available",
    "compiled_abi_verified", "execution_verified"))
_LANGUAGES = frozenset(("lua", "c", "cpp", "js", "python", "java", "csharp"))


class ProductError(ValueError):
    """Missing, changed or invalid SDK product identity."""


def _target_owner():
    path = Path(__file__).with_name("sdk_target.py")
    spec = importlib.util.spec_from_file_location("jce_sdk_product_target", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _address(value):
    if type(value) is not str or not value or len(value.encode("utf-8")) > 1023:
        raise ProductError("SDK address must be nonempty UTF-8 text up to 1023 bytes")
    if (value.startswith("/") or "\\" in value or ":" in value or
            any(ord(c) < 32 or ord(c) == 127 for c in value) or
            any(part in ("", ".", "..") for part in value.split("/")) or
            PurePosixPath(value).as_posix() != value):
        raise ProductError("noncanonical SDK relative address: " + value)
    return value


def _snapshot(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns, getattr(info, "st_file_attributes", 0))


def _opened_matches(expected, info):
    # Windows path stat derives executable permission bits from extensions,
    # while fstat does not; their ctime can mean birth/change respectively.
    # Bind only common identity fields here. Full path and full descriptor
    # snapshots are each independently compared before/after the same read.
    observed = _snapshot(info)
    return (expected[0], expected[1], stat.S_IFMT(expected[2]), expected[3], expected[4]) == (
        observed[0], observed[1], stat.S_IFMT(observed[2]), observed[3], observed[4])


def _safe_stat(path, *, directory=False):
    try:
        info = path.lstat()
    except OSError as error:
        raise ProductError("SDK path unavailable: " + str(path)) from error
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise ProductError("SDK symlink/reparse point forbidden: " + str(path))
    if not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)):
        raise ProductError("SDK path has wrong file kind: " + str(path))
    return info


def _root(sdk):
    requested = Path(sdk)
    _safe_stat(requested, directory=True)
    return requested.resolve(strict=True)


def _unique_json(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProductError("duplicate SDK product JSON key: " + key)
        result[key] = value
    return result


def _json(data):
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_unique_json,
            parse_constant=lambda value: (_ for _ in ()).throw(ProductError("nonfinite JSON: " + value)))
    except (UnicodeError, ValueError) as error:
        raise ProductError("invalid SDK product JSON: " + str(error)) from error


def _read_small(path, cap):
    before = _safe_stat(path)
    if before.st_size > cap:
        raise ProductError("SDK metadata exceeds byte limit: " + str(path))
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if not _opened_matches(_snapshot(before), opened):
            raise ProductError("SDK metadata changed before read: " + str(path))
        data = stream.read(cap + 1)
        after = os.fstat(stream.fileno())
    if len(data) > cap or len(data) != before.st_size or _snapshot(after) != _snapshot(opened) or _snapshot(_safe_stat(path)) != _snapshot(before):
        raise ProductError("SDK metadata changed during read: " + str(path))
    return data, _snapshot(before)


def _inventory(root):
    files, directories = {}, {}
    entries_seen = 0
    def visit(folder):
        nonlocal entries_seen
        if len(directories) >= MAX_DIRECTORIES:
            raise ProductError("SDK directory inventory exceeds limit")
        directories[folder] = _snapshot(_safe_stat(folder, directory=True))
        with os.scandir(folder) as entries:
            names = []
            for entry in entries:
                entries_seen += 1
                if entries_seen > MAX_FILES + MAX_DIRECTORIES:
                    raise ProductError("SDK directory entries exceed limit")
                names.append(entry.name)
            names.sort()
        for name in names:
            path = folder / name
            relative = _address(path.relative_to(root).as_posix())
            try: info = path.lstat()
            except OSError as error: raise ProductError("SDK entry changed: " + relative) from error
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ProductError("SDK symlink/reparse point forbidden: " + relative)
            if stat.S_ISDIR(info.st_mode):
                visit(path)
            elif stat.S_ISREG(info.st_mode):
                if relative == PRODUCT_FILE:
                    continue
                key = os.path.normcase(relative)
                if key in files:
                    raise ProductError("duplicate SDK relative address: " + relative)
                files[key] = (relative, _snapshot(info))
                if len(files) > MAX_FILES:
                    raise ProductError("SDK file inventory exceeds limit")
            else:
                raise ProductError("nonregular SDK input: " + relative)
    visit(root)
    return sorted(files.values()), directories


def _role(address):
    parts = address.split("/")
    if parts[0] == "include": return "header"
    if parts[:2] == ["lib", "cmake"]: return "cmake"
    if parts[0] == "lib" and Path(address).suffix.lower() in (".lib", ".a", ".so", ".dll", ".dylib"):
        return "library"
    if parts[0] == "bin": return "host-tool-or-helper"
    if parts[0] == "share": return "resource"
    return "metadata-or-notice"


def _hash_file(path, expected, capture_limit=None):
    if _snapshot(_safe_stat(path)) != expected:
        raise ProductError("SDK source changed before hashing: " + str(path))
    if capture_limit is not None and expected[3] > capture_limit:
        raise ProductError("SDK metadata exceeds byte limit: " + str(path))
    digest, total, captured = hashlib.sha256(), 0, bytearray()
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if not _opened_matches(expected, opened):
            raise ProductError("SDK source changed on open: " + str(path))
        while True:
            block = stream.read(_CHUNK_BYTES)
            if not block: break
            total += len(block)
            if total > expected[3]:
                raise ProductError("SDK source grew during hashing: " + str(path))
            digest.update(block)
            if capture_limit is not None: captured.extend(block)
        after = _snapshot(os.fstat(stream.fileno()))
    if total != expected[3] or after != _snapshot(opened) or _snapshot(_safe_stat(path)) != expected:
        raise ProductError("SDK source changed during hashing: " + str(path))
    return digest.hexdigest(), bytes(captured)


def _version(data, expected_version):
    if type(expected_version) is not str or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", expected_version):
        raise ProductError("explicit canonical expected JCE version required")
    try: text = data.decode("utf-8")
    except UnicodeError as error: raise ProductError("generated version header must be UTF-8") from error
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S).replace("\\\n", "")
    values = {}
    for name in ("MAJOR", "MINOR", "PATCH", "STR", "API_VERSION"):
        macro = "JCE_" + name if name == "API_VERSION" else "JCE_VERSION_" + name
        matches = re.findall(r"(?m)^\s*#\s*define\s+" + macro + r"\s+([^\n]+)", text)
        if len(matches) != 1: raise ProductError("one generated " + macro + " required")
        values[name] = matches[0].strip()
    numbers = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        if not re.fullmatch(r"[0-9]+", values[name]): raise ProductError("invalid version integer")
        value = int(values[name])
        if value > 255: raise ProductError("version exceeds packed API byte")
        numbers.append(value)
    version = ".".join(str(n) for n in numbers)
    if values["STR"] != '"' + version + '"' or version != expected_version:
        raise ProductError("SDK generated version mismatch")
    expression = values["API_VERSION"]
    for name, value in zip(("MAJOR", "MINOR", "PATCH"), numbers):
        expression = re.sub(r"\bJCE_VERSION_" + name + r"\b", str(value), expression)
    try: tree = ast.parse(expression, mode="eval")
    except SyntaxError as error: raise ProductError("invalid generated API expression") from error
    def evaluate(node):
        if isinstance(node, ast.Constant) and type(node.value) is int and 0 <= node.value <= 0xffffffff:
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.BitOr, ast.LShift)):
            left, right = evaluate(node.left), evaluate(node.right)
            if isinstance(node.op, ast.LShift) and right > 31:
                raise ProductError("API shift out of bounds")
            return left | right if isinstance(node.op, ast.BitOr) else left << right
        raise ProductError("unsupported generated API expression")
    api = evaluate(tree.body)
    if api != (numbers[0] << 24 | numbers[1] << 16 | numbers[2] << 8):
        raise ProductError("SDK packed API/version mismatch")
    return version, api


def _profile(data, delivery):
    value = _json(data)
    if type(value) is not dict or set(value) != {"schema", "profile", "languages", "external_vm_allowed"}:
        raise ProductError("invalid SDK delivery profile fields")
    languages = value["languages"]
    if (type(value["schema"]) is not int or value["schema"] != 1 or value["profile"] != delivery or
            type(value["external_vm_allowed"]) is not bool or
            (delivery == "embedded" and value["external_vm_allowed"]) or
            type(languages) is not list or not languages or
            any(type(x) is not str or x not in _LANGUAGES for x in languages) or
            len(set(languages)) != len(languages)):
        raise ProductError("invalid SDK delivery profile identity")
    return value


def _required(values):
    if isinstance(values, (str, bytes)): raise ProductError("required paths must be a sequence")
    result = [_address(x) for x in values]
    if PRODUCT_FILE in result or len(result) != len({os.path.normcase(x) for x in result}):
        raise ProductError("duplicate/self required SDK address")
    return sorted(result)


def _describe(root, request, expected_version, required_paths):
    try: target = _target_owner().require_sdk_target(root, **request)
    except ValueError as error: raise ProductError("SDK target rejected: " + str(error)) from error
    target_path = "lib/" + request["config"] + "/jce-sdk-target.json"
    version_path, profile_path = "include/jce/jce_version.h", "delivery-profile.json"
    required = set(required_paths) | {target_path, version_path, profile_path,
        "include/jce/api.h", "lib/cmake/JCE/JCEConfig.cmake", "lib/cmake/JCE/JCESDKTarget.cmake",
        "lib/cmake/JCE/JCESDKHelpers.cmake", "lib/" + request["config"] + "/" + target["archive"]}
    files, directories = _inventory(root)
    names = {address for address, _ in files}
    if required - names: raise ProductError("SDK required files missing: " + ", ".join(sorted(required - names)))
    captures, rows = {}, []
    for address, snapshot in files:
        digest, data = _hash_file(root / address, snapshot,
            _METADATA_CAP if address in (target_path, version_path, profile_path) else None)
        if address in (target_path, version_path, profile_path): captures[address] = data
        rows.append(dict(path=address, size=snapshot[3], sha256=digest, role=_role(address)))
    if _json(captures[target_path]) != target:
        raise ProductError("SDK target changed between admission and hashing")
    version, api = _version(captures[version_path], expected_version)
    profile = _profile(captures[profile_path], request["delivery"])
    for address, snapshot in files:
        if _snapshot(_safe_stat(root / address)) != snapshot:
            raise ProductError("SDK source changed before publication: " + address)
    again, _ = _inventory(root)
    if again != files: raise ProductError("SDK file closure changed during observation")
    for directory, snapshot in directories.items():
        observed = _snapshot(_safe_stat(directory, directory=True))
        if observed != snapshot:
            fields = ("st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns",
                      "st_ctime_ns", "st_file_attributes")
            changed = {name: {"before": str(previous), "after": str(current)}
                       for name, previous, current in zip(fields, snapshot, observed)
                       if previous != current}
            witness = {"directory": directory.relative_to(root).as_posix(),
                       "changed": changed}
            raise ProductError("SDK directory changed during observation: " +
                json.dumps(witness, ensure_ascii=True, separators=(",", ":")))
    return dict(schema=1, scope=_SCOPE, jce_version=version, api_version=api,
        sdk_target=target, delivery_profile=profile, required_paths=required_paths, files=rows,
        build_payload_available=False, compiled_abi_verified=False, execution_verified=False)


def _request(system, arch, config, variant, delivery):
    return dict(system=system, arch=arch, config=config, variant=variant, delivery=delivery)


def write_product(sdk, *, system, arch, config, variant, delivery,
                  expected_version, required_paths=()):
    """Hash the complete installed tree and atomically publish its identity.

    required_paths may bind producer-owned library/helper/tool requirements.
    The directory inventory itself records presence, not successful execution,
    planned install completeness or the availability of a compiler environment.
    """
    root = _root(sdk)
    product = _describe(root, _request(system, arch, config, variant, delivery),
                        expected_version, _required(required_paths))
    data = (json.dumps(product, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(data) > MAX_MANIFEST_BYTES: raise ProductError("SDK product manifest exceeds limit")
    previous = root / PRODUCT_FILE
    if previous.exists() and _safe_stat(previous).st_size == len(data):
        old, _ = _read_small(previous, MAX_MANIFEST_BYTES)
        if old == data: return product  # Same freshly observed identity; no mtime churn.
    temporary = None
    try:
        fd, temporary = tempfile.mkstemp(prefix=".jce-sdk-product-", suffix=".tmp", dir=root)
        with os.fdopen(fd, "wb") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, root / PRODUCT_FILE)
        temporary = None
    finally:
        if temporary is not None: Path(temporary).unlink(missing_ok=True)
    return product


def verify_product(sdk, *, system, arch, config, variant, delivery,
                   expected_version, required_paths=()):
    """Read-only exact identity validation against caller-selected tuple/version."""
    root = _root(sdk)
    raw, manifest_snapshot = _read_small(root / PRODUCT_FILE, MAX_MANIFEST_BYTES)
    declared = _json(raw)
    if type(declared) is not dict or set(declared) != _FIELDS:
        raise ProductError("invalid SDK product fields")
    if type(declared["schema"]) is not int or declared["schema"] != 1 or declared["scope"] != _SCOPE:
        raise ProductError("unsupported SDK product schema/scope")
    for name in ("build_payload_available", "compiled_abi_verified", "execution_verified"):
        if declared[name] is not False: raise ProductError("SDK identity cannot claim " + name)
    paths = _required(declared["required_paths"])
    if paths != declared["required_paths"] or not set(_required(required_paths)) <= set(paths):
        raise ProductError("SDK product required closure mismatch")
    rows = declared["files"]
    if type(rows) is not list or len(rows) > MAX_FILES: raise ProductError("invalid SDK product file list")
    seen = set()
    for row in rows:
        if type(row) is not dict or set(row) != {"path", "size", "sha256", "role"}:
            raise ProductError("invalid SDK product file row")
        address = _address(row["path"]); key = os.path.normcase(address)
        if address == PRODUCT_FILE or key in seen: raise ProductError("duplicate/self SDK product path")
        seen.add(key)
        if (type(row["size"]) is not int or row["size"] < 0 or
                type(row["sha256"]) is not str or not re.fullmatch("[0-9a-f]{64}", row["sha256"]) or
                row["role"] != _role(address)):
            raise ProductError("invalid SDK product size/hash/role")
    observed = _describe(root, _request(system, arch, config, variant, delivery), expected_version, paths)
    # Canonical JSON comparison preserves bool/int/float distinctions that
    # Python dict equality would silently equate in nested target/profile data.
    if json.dumps(observed, sort_keys=True) != json.dumps(declared, sort_keys=True):
        raise ProductError("SDK product identity mismatch")
    if _snapshot(_safe_stat(root / PRODUCT_FILE)) != manifest_snapshot:
        raise ProductError("SDK product manifest changed during verification")
    return declared


def source_version(source_root):
    """Bind installed generated version fields to the actual producer source."""
    data, _ = _read_small(Path(source_root) / "CMakeLists.txt", _SOURCE_VERSION_CAP)
    try: text = data.decode("utf-8")
    except UnicodeError as error: raise ProductError("invalid producer CMake text") from error
    text = re.sub(r"(?m)#.*$", "", text)
    versions = re.findall(r"\bproject\s*\(\s*JCE\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s+LANGUAGES\s+C\s*\)", text)
    if len(versions) != 1: raise ProductError("one producer JCE version required")
    return versions[0]


def install_required_paths(build, sdk):
    """Bind CMake's actual install file roster, never a second install recipe.

    The producer owns the build/SDK lease; this bounded read proves observed
    list identity only. Paths outside the selected SDK, directories and aliases
    are rejected. Post-install producer metadata and host tools are added by
    the caller. No compiler or executable is run here.
    """
    root = _root(sdk)
    data, _ = _read_small(Path(build) / "install_manifest.txt", _INSTALL_LIST_CAP)
    try: lines = data.decode("utf-8").splitlines()
    except UnicodeError as error: raise ProductError("invalid CMake install file list") from error
    addresses = set()
    for raw in lines:
        if not raw: continue
        path = Path(raw)
        if not path.is_absolute(): raise ProductError("install path must be absolute")
        try: address = path.relative_to(root).as_posix()
        except ValueError as error: raise ProductError("install path escaped selected SDK") from error
        address = _address(address)
        if address == PRODUCT_FILE: raise ProductError("product identity is not a CMake install input")
        _safe_stat(path)
        if path.resolve(strict=True) != path:
            raise ProductError("install path traversed an alias/reparse point")
        addresses.add(address)
        if len(addresses) > MAX_FILES: raise ProductError("CMake install file list exceeds limit")
    if not addresses: raise ProductError("CMake install file list is empty")
    return _required(addresses)


def publish_install(driver, sdk: Path, target: dict, config: str, variant: str,
                         delivery: str, build: Path, require_tools: bool = True):
    """Called only after install completion inside the existing SDK lease."""
    if driver.DRY_RUN: return None
    try:
        required = install_required_paths(build, sdk)
        required.extend(["VERSION.txt", "delivery-profile.json"])
        if require_tools:
            folder = "bin/host" if target.get("cross") or target.get("emscripten") else "bin"
            required.extend(folder + "/" + driver._host_exe(name) for name in
                ("jce_pak", "jce_cook", "jce_bin2obj", "jce_bundle_pack"))
        required = sorted(set(required))
        system = "Emscripten" if target.get("emscripten") else {
            "windows": "Windows", "linux": "Linux", "darwin": "Darwin"}[target["host"]]
        manifest = write_product(sdk, system=system, arch=target["arch"],
            config=config, variant="debug" if config == "Debug" else variant,
            delivery=delivery, expected_version=source_version(driver.ROOT), required_paths=required)
    except (ValueError, OSError, KeyError) as error:
        driver.die("SDK file identity publication rejected: " + str(error))
    driver.log(f"SDK installed-file identity -> {Path(sdk) / PRODUCT_FILE}; compiler ABI/execution/builder not verified")
    return manifest

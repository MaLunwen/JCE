"""Offline Lua/JS EXE sealing using one prebuilt public-SDK stock runtime.

This module never compiles, extracts or invents an archive codec. The public
SDK packer authenticates/encrypts an outer PAK containing the exact typed Bundle.
FNV envelope checks are corruption checks, not signatures or authentication.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from types import SimpleNamespace

from tools.build import delivery, sdk_product, standalone

ROOT = Path(__file__).resolve().parents[2]
MAGIC = b"JCE_STOCK_APP_V2"
LEGACY_MAGIC = b"JCE_STOCK_APP_V1"
FOOTER = struct.Struct("<16sIIQQQQQ")
HOST_LIMIT = 128 * 1024 * 1024
BUNDLE_LIMIT = 512 * 1024 * 1024
ARCHIVE_LIMIT = BUNDLE_LIMIT + 1024 * 1024
IMAGE_LIMIT = HOST_LIMIT + ARCHIVE_LIMIT + FOOTER.size
BUNDLE_ENTRY = "stock/project.jbundle"
_PROTECTION = {"envelope_version": 2, "payload": "jce.secure-project-pak",
               "entry": BUNDLE_ENTRY, "security": "keyed-index-chacha20-hmac-sha256"}
MANIFEST_NAME = "jce-stock-runtime.json"
_FIELDS = {"schema", "scope", "target", "languages", "sdk", "files", "notices", "producer", "protection"}
_TARGET = {"system": "Windows", "arch": "x86_64", "config": "Release",
           "variant": "dist", "delivery": "embedded"}


class StockError(ValueError):
    """An unqualified runtime, Bundle or envelope cannot be shipped."""


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _read(path, cap):
    path = Path(path).absolute()
    if path.resolve(strict=True) != path:
        raise StockError("stock input traverses an alias/reparse point: " + str(path))
    data, _ = sdk_product._read_small(path, cap)
    return data


def _json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise StockError("duplicate stock JSON field: " + key)
            result[key] = value
        return result
    def invalid(value):
        raise StockError("nonfinite stock JSON number: " + value)
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=unique,
                          parse_constant=invalid)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise StockError("invalid stock JSON") from error


def _dump(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=True, indent=2, sort_keys=True)
        stream.write("\n")


def fnv64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def _pe(data):
    if len(data) < 64 or data[:2] != b"MZ":
        raise StockError("stock host is not a PE executable")
    offset, = struct.unpack_from("<I", data, 60)
    if offset > len(data) or len(data) - offset < 94:
        raise StockError("truncated stock PE header")
    machine, = struct.unpack_from("<H", data, offset + 4)
    optional, = struct.unpack_from("<H", data, offset + 20)
    magic, = struct.unpack_from("<H", data, offset + 24)
    subsystem, = struct.unpack_from("<H", data, offset + 92)
    if (data[offset:offset + 4] != b"PE\0\0" or machine != 0x8664 or
            optional < 70 or optional > len(data) - offset - 24 or
            magic != 0x20b or subsystem != 2):
        raise StockError("stock host must be a complete AMD64 PE32+ GUI executable")


def _host(data):
    if not 0 < len(data) <= HOST_LIMIT:
        raise StockError("stock host exceeds its 128 MiB budget")
    _pe(data)
    if len(data) >= FOOTER.size and data[-FOOTER.size:-FOOTER.size + 16] in (MAGIC, LEGACY_MAGIC):
        raise StockError("a sealed application cannot be used as a stock runtime")


def footer_for(host, bundle):
    """Exact wire writer; runtime/stock_payload.c is the production reader."""
    _host(host)
    if not 0 < len(bundle) <= ARCHIVE_LIMIT:
        raise StockError("secure archive exceeds its nonempty envelope budget")
    prefix = struct.pack("<16sIIQQQQ", MAGIC, 2, FOOTER.size, len(host),
                         len(bundle), fnv64(host), fnv64(bundle))
    return prefix + struct.pack("<Q", fnv64(prefix))


def inspect_envelope(data):
    """Read-only packaging check; does not certify native reader execution."""
    if not FOOTER.size <= len(data) <= IMAGE_LIMIT:
        raise StockError("invalid stock image size")
    magic, version, width, host_size, bundle_size, host_hash, bundle_hash, check = FOOTER.unpack_from(data, len(data) - FOOTER.size)
    if (magic != MAGIC or version != 2 or width != FOOTER.size or
            fnv64(data[-FOOTER.size:-8]) != check or
            not 0 < host_size <= HOST_LIMIT or not 0 < bundle_size <= ARCHIVE_LIMIT or
            host_size + bundle_size + FOOTER.size != len(data)):
        raise StockError("invalid stock envelope metadata")
    host = data[:host_size]
    bundle = data[host_size:-FOOTER.size]
    _host(host)
    if fnv64(host) != host_hash or fnv64(bundle) != bundle_hash:
        raise StockError("stock envelope checksum mismatch")
    return host, bundle


def _row(name, data):
    return {"path": name, "size": len(data), "sha256": _sha(data)}


def _file(root, row, cap):
    if type(row) is not dict or set(row) != {"path", "size", "sha256"}:
        raise StockError("invalid stock file identity")
    address = sdk_product._address(row["path"])
    if type(row["size"]) is not int or row["size"] < 1 or row["size"] > cap:
        raise StockError("invalid stock file size")
    data = _read(root / address, cap)
    if _row(address, data) != row:
        raise StockError("stock file identity changed: " + address)
    return root / address, data


def _key_bytes(raw):
    try:
        text = raw.decode("ascii").strip()
    except UnicodeError as error:
        raise StockError("invalid stock sealing key format") from error
    if len(text) != 64 or any(c not in "0123456789abcdefABCDEF" for c in text):
        raise StockError("invalid stock sealing key format")
    return bytes.fromhex(text)


def load_runtime(manifest_path):
    """A relocatable runtime package needs no installed SDK or compiler."""
    path = Path(manifest_path).absolute()
    raw = _read(path, 64 * 1024)
    manifest = _json(raw)
    if (type(manifest) is not dict or set(manifest) != _FIELDS or
            type(manifest["schema"]) is not int or manifest["schema"] != 2 or
            manifest["scope"] != "compilerless-lua-js-stock-runtime" or
            json.dumps(manifest["target"], sort_keys=True) != json.dumps(_TARGET, sort_keys=True) or
            manifest["languages"] != ["lua", "js"]):
        raise StockError("unsupported stock runtime contract/target/languages")
    sdk = manifest["sdk"]
    if (type(sdk) is not dict or set(sdk) != {"product_sha256", "jce_version"} or
            not isinstance(sdk["jce_version"], str) or not sdk["jce_version"] or
            not isinstance(sdk["product_sha256"], str) or len(sdk["product_sha256"]) != 64 or
            any(c not in "0123456789abcdef" for c in sdk["product_sha256"])):
        raise StockError("invalid producing SDK identity")
    origin = manifest["producer"]
    if (type(origin) is not dict or set(origin) != {"receipt_sha256", "compile_input_sha256", "output_sha256", "sealing_key_sha256", "sealing_key_path"} or
            any(type(value) is not str or len(value) != 64 or any(c not in "0123456789abcdef" for c in value)
                for key, value in origin.items() if key != "sealing_key_path") or
            type(origin["sealing_key_path"]) is not str or
            not Path(origin["sealing_key_path"]).is_absolute()):
        raise StockError("missing verified stock producer identity")
    if manifest["protection"] != dict(_PROTECTION, key_sha256=origin["sealing_key_sha256"]):
        raise StockError("stock sealing key/protection identity mismatch")
    files = manifest["files"]
    if type(files) is not dict or set(files) != {"runtime", "inspector"}:
        raise StockError("missing stock runtime/inspector identities")
    runtime, host = _file(path.parent, files["runtime"], HOST_LIMIT)
    inspector, _ = _file(path.parent, files["inspector"], HOST_LIMIT)
    _host(host)
    if origin["output_sha256"] != _sha(host):
        raise StockError("stock producer output differs from runtime bytes")
    if runtime == inspector:
        raise StockError("stock runtime and inspector must be distinct")
    notices = manifest["notices"]
    if type(notices) is not dict or set(notices) != {"LICENSE", "THIRD_PARTY_LICENSES.md"}:
        raise StockError("missing stock notice identities")
    for name, row in notices.items():
        if type(row) is not dict or row.get("path") != name:
            raise StockError("noncanonical stock notice address")
        _file(path.parent, row, 4 * 1024 * 1024)
    # Reuse ordinary/delayed import policy and actual embedded notice verifier.
    imports = standalone.verify(runtime)["imports"]
    standalone.verify(inspector)
    observed_notices = standalone.verify_notices(runtime, path.parent)
    if observed_notices != {name: row["sha256"] for name, row in notices.items()}:
        raise StockError("stock embedded notices disagree with its package")
    if _read(path, 64 * 1024) != raw:
        raise StockError("stock runtime manifest changed during verification")
    return SimpleNamespace(path=path, raw=raw, manifest=manifest, runtime=runtime,
                           host=host, inspector=inspector, imports=imports)


def _producer_receipt(path, expected_sha, exe, host, sdk, product_sha):
    from tools.build import stock_runtime
    raw = _read(path, 1024 * 1024)
    receipt = _json(raw)
    if _sha(raw) != expected_sha:
        raise StockError("stock producer receipt SHA mismatch")
    if (type(receipt) is not dict or type(receipt.get("schema")) is not int or receipt["schema"] != 1 or
            receipt.get("scope") != "stock-runtime-component-producer" or
            receipt.get("status") != "STOCK_COMPONENT_BUILT_CPU_CONTROLS_PASSED_NOT_AN_APPLICATION" or
            type(receipt.get("returncode")) is not int or receipt["returncode"] != 0 or
            receipt.get("runnable_application") is not False or receipt.get("runtime_execution_verified") is not False or
            json.dumps(receipt.get("target"), sort_keys=True) != json.dumps(_TARGET, sort_keys=True)):
        raise StockError("stock publication requires a successful stock component producer receipt")
    output = receipt.get("output", {})
    selected_sdk = receipt.get("sdk", {})
    if (type(output) is not dict or type(selected_sdk) is not dict or
            type(receipt.get("assets")) is not dict):
        raise StockError("invalid stock producer output/SDK records")
    if (output.get("path") != str(exe) or output.get("sha256") != _sha(host) or
            type(output.get("size")) is not int or output["size"] != len(host) or
            selected_sdk.get("path") != str(sdk) or selected_sdk.get("product_sha256") != product_sha or
            receipt.get("assets", {}).get("has_runtime_boot") is not False):
        raise StockError("stock producer EXE/SDK/boot identity mismatch")
    inputs = stock_runtime._snapshot(ROOT)
    compile_identity = _sha(json.dumps({"sdk": product_sha, "sources": inputs},
        sort_keys=True, separators=(",", ":")).encode())
    if (receipt.get("inputs_before") != inputs or receipt.get("inputs_after") != inputs or
            receipt.get("compile_input_sha256") != compile_identity):
        raise StockError("stock producer source or compile identity changed")
    log = receipt.get("native_cpu_log", {})
    if type(log) is not dict or _sha(_read(log.get("path", ""), 8 * 1024 * 1024)) != log.get("sha256"):
        raise StockError("stock producer native CPU receipt unavailable or changed")
    key_record = receipt.get("sealing_key", {})
    if (type(key_record) is not dict or set(key_record) != {"path", "sha256"} or
            type(key_record["path"]) is not str or type(receipt.get("cache")) is not str):
        raise StockError("stock producer lacks the actual sealing key identity")
    key_path = Path(key_record["path"]).absolute()
    if key_path != Path(receipt.get("cache", "")) / ".jce/pak_key.hex":
        raise StockError("stock producer sealing key escaped its owned cache")
    if _sha(_key_bytes(_read(key_path, 128))) != key_record["sha256"]:
        raise StockError("stock producer sealing key changed")
    return {"receipt_sha256": expected_sha, "compile_input_sha256": compile_identity,
            "output_sha256": _sha(host), "sealing_key_sha256": key_record["sha256"],
            "sealing_key_path": str(key_path)}


def publish_runtime(*, exe, sdk, license_file, out, build_receipt, build_receipt_sha256):
    """Producer-only publication after the shared driver built a stock host.

    The selected SDK's actual installed-file identity is fully verified here;
    consumers subsequently need only this small runtime/tool package.
    """
    sdk = Path(sdk).absolute()
    version = sdk_product.source_version(ROOT)
    product = sdk_product.verify_product(sdk, system="Windows", arch="x86_64",
        config="Release", variant="dist", delivery="embedded", expected_version=version,
        required_paths=("bin/jce_pak.exe", "share/jce/THIRD_PARTY_LICENSES.md"))
    product_raw = _read(sdk / sdk_product.PRODUCT_FILE, sdk_product.MAX_MANIFEST_BYTES)
    if _json(product_raw) != product:
        raise StockError("SDK product changed after verification")
    exe, out = Path(exe).absolute(), Path(out).absolute()
    host = _read(exe, HOST_LIMIT)
    _host(host)
    origin = _producer_receipt(build_receipt, build_receipt_sha256, exe, host, sdk, _sha(product_raw))
    inspector = _read(sdk / "bin/jce_pak.exe", HOST_LIMIT)
    notices = {"LICENSE": _read(license_file, 4 * 1024 * 1024),
               "THIRD_PARTY_LICENSES.md": _read(sdk / "share/jce/THIRD_PARTY_LICENSES.md", 4 * 1024 * 1024)}
    # A fresh destination is the ownership boundary. Failed partial packages
    # have no valid manifest and are never silently reused as successful ones.
    out.mkdir(parents=True, exist_ok=False)
    rows = {}
    for role, name, data in (("runtime", "jce_stock_runtime.exe", host),
                             ("inspector", "jce_pak.exe", inspector)):
        with (out / name).open("xb") as stream:
            stream.write(data)
        rows[role] = _row(name, data)
    notice_rows = {}
    for name, data in notices.items():
        with (out / name).open("xb") as stream:
            stream.write(data)
        notice_rows[name] = _row(name, data)
    standalone.verify(out / rows["runtime"]["path"])
    standalone.verify(out / rows["inspector"]["path"])
    observed = standalone.verify_notices(out / rows["runtime"]["path"], out)
    if observed != {name: row["sha256"] for name, row in notice_rows.items()}:
        raise StockError("stock executable lacks matching embedded notices")
    if (_read(exe, HOST_LIMIT) != host or
            _read(sdk / sdk_product.PRODUCT_FILE, sdk_product.MAX_MANIFEST_BYTES) != product_raw or
            _read(sdk / "bin/jce_pak.exe", HOST_LIMIT) != inspector or
            _read(license_file, 4 * 1024 * 1024) != notices["LICENSE"] or
            _read(sdk / "share/jce/THIRD_PARTY_LICENSES.md", 4 * 1024 * 1024) != notices["THIRD_PARTY_LICENSES.md"]):
        raise StockError("stock publication inputs changed")
    manifest = {"schema": 2, "scope": "compilerless-lua-js-stock-runtime",
        "target": dict(_TARGET), "languages": ["lua", "js"],
        "sdk": {"product_sha256": _sha(product_raw), "jce_version": version},
        "files": rows, "notices": notice_rows, "producer": origin,
        "protection": dict(_PROTECTION, key_sha256=origin["sealing_key_sha256"])}
    _dump(out / MANIFEST_NAME, manifest)
    return manifest


class _InspectionDriver:
    DRY_RUN = False
    HOST = "windows"
    ROOT = ROOT

    @staticmethod
    def _sha256(path):
        return _sha(_read(path, BUNDLE_LIMIT))

    @staticmethod
    def run(command, cwd):
        subprocess.run(command, cwd=cwd, check=True, timeout=180)


def _secure_report(path, archive, inner_size):
    report = _json(_read(path, 16 * 1024 * 1024))
    if type(report) is not dict:
        raise StockError("invalid project archive inspection")
    header, totals = report.get("header"), report.get("totals")
    entries = report.get("entries")
    if type(header) is not dict or type(totals) is not dict:
        raise StockError("invalid project archive verification records")
    if (report.get("schema") != "jce.pakbom.v1" or report.get("file") != str(archive) or
            type(report.get("file_size")) is not int or report["file_size"] != archive.stat().st_size or
            any(header.get(key) is not True for key in
                ("header_verified", "encrypted", "secure_index", "authenticated", "auth_verified")) or
            any(type(header.get(key)) is not int or header[key] != value for key, value in
                (("entry_count", 1), ("dict_count", 0), ("index_original_size", 32))) or
            totals.get("verify_ran") is not True or
            any(type(totals.get(key)) is not int or totals[key] != value for key, value in
                (("entries", 1), ("verified_count", 1), ("encrypted_count", 1),
                 ("authenticated_count", 1), ("corrupt_count", 0), ("skipped_count", 0))) or
            type(entries) is not list or len(entries) != 1 or type(entries[0]) is not dict):
        raise StockError("project archive lacks unique authenticated deep verification")
    entry = entries[0]
    if (type(entry.get("original_size")) is not int or entry["original_size"] != inner_size or
            entry.get("encrypted") is not True or entry.get("authenticated") is not True or
            entry.get("verified") is not True):
        raise StockError("project archive entry does not match the exact Bundle budget")
    return report


def _secure_project(driver, runtime, work, payload):
    # One exact Bundle is input to the public archive owner; no private codec.
    stage = work / "secure-stage"
    inner = stage / BUNDLE_ENTRY
    inner.parent.mkdir(parents=True)
    with inner.open("xb") as stream:
        stream.write(payload)
    archive = work / "project.pak"
    bom, deep = work / "project-pack.json", work / "project-inspect.json"
    command = [str(runtime.inspector), "--resource-dir", str(stage),
        "--pak-file", str(archive), "--header-file", str(work / "project.h"),
        "--manifest-file", str(work / "project.cmake"), "--json", str(bom),
        "--obj-format", "none", "--encrypt-key-file", str(runtime.key_file),
        "--platform", "desktop", "--bundle-target", "windows"]
    driver.run(command, cwd=driver.ROOT)
    produced = _read(archive, ARCHIVE_LIMIT)
    packed = _secure_report(bom, archive, len(payload))
    sources = packed.get("sources")
    if (packed.get("sources_verified") is not True or type(sources) is not list or len(sources) != 1 or
            type(sources[0]) is not dict or sources[0].get("path") != BUNDLE_ENTRY or
            sources[0].get("selected") is not False or sources[0].get("size") != len(payload) or
            Path(sources[0].get("source", "")).absolute() != inner or _read(inner, BUNDLE_LIMIT) != payload):
        raise StockError("public secure writer did not verify the exact staged Bundle bytes")
    # Pack mode decrypts and memcmps to its source. Fresh public inspection
    # then verifies authentication of the exact outer bytes we append.
    driver.run([str(runtime.inspector), "--inspect", str(archive), "--verify",
        "--key-file", str(runtime.key_file), "--json", str(deep),
        "--platform", "desktop", "--bundle-target", "windows"], cwd=driver.ROOT)
    checked = _secure_report(deep, archive, len(payload))
    def stable_entry(row):
        # Secure inspect-only cannot recover names; pack mode has the source ledger.
        return {key: value for key, value in row.items() if key != "path"}
    if (_read(archive, ARCHIVE_LIMIT) != produced or
            stable_entry(checked["entries"][0]) != stable_entry(packed["entries"][0])):
        raise StockError("project secure archive changed during verification")
    return produced, {**runtime.manifest["protection"], "archive_sha256": _sha(produced),
        "inner_sha256": _sha(payload), "inner_size": len(payload),
        "pack_bom": {"path": str(bom), "sha256": _sha(_read(bom, 16 * 1024 * 1024))},
        "inspection": {"path": str(deep), "sha256": _sha(_read(deep, 16 * 1024 * 1024))},
        "authenticated": True, "encrypted": True, "exact_source_verified": True}


def seal(*, runtime_manifest, bundle, out, work_dir, receipt, pak_key_file):
    """Protect one exact typed Bundle using an explicit external producer key."""
    runtime = load_runtime(runtime_manifest)
    runtime.key_file = Path(pak_key_file).absolute()
    # The producing path is provenance, not the key's identity. A caller may
    # relocate its explicit key; the strict decoded bytes must still match the
    # protection/producer identity admitted by load_runtime, including after IO.
    if _sha(_key_bytes(_read(runtime.key_file, 128))) != runtime.manifest["protection"]["key_sha256"]:
        raise StockError("selected sealing key does not match the stock producer")
    bundle, out = Path(bundle).absolute(), Path(out).absolute()
    if bundle.suffix.lower() != ".jbundle" or out.suffix.lower() != ".exe":
        raise StockError("stock input/output must be .jbundle/.exe")
    work = Path(work_dir).absolute()
    receipt_path = Path(receipt).absolute()
    final_parent = out.parent.resolve()
    if work.resolve() == receipt_path.resolve():
        raise StockError("stock work directory and receipt file must be distinct")
    if any(path.resolve() == final_parent or path.resolve().is_relative_to(final_parent)
           for path in (work, receipt_path)):
        raise StockError("stock work/receipt must be outside the final EXE directory")
    if (out.exists() or receipt_path.exists() or work.exists() or
            (out.parent.exists() and any(out.parent.iterdir()))):
        raise StockError("stock output directory must be empty and evidence owners fresh")
    payload = _read(bundle, BUNDLE_LIMIT)
    if not payload:
        raise StockError("empty Bundle")
    out.parent.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=False)
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    driver = _InspectionDriver()
    # This is the existing public reader's fresh --verify receipt and typed
    # dependency graph authority. No private archive decoder or second packer.
    plan = delivery.inspect_project_bundles(driver, work,
        {"bundles": [str(bundle)]}, runtime.path.parent,
        {"host": "windows"}, sdk_tools={"jce_pak": runtime.inspector})
    if (not plan or len(plan["bundles"]) != 1 or plan["use_loose_scene"] or
            plan["bundles"][0]["depends_on"] or not plan["startup_scene"]):
        raise StockError("stock application needs one self-contained typed scene Bundle")
    inspection = _json(_read(plan["bundles"][0]["inspection_path"], 16 * 1024 * 1024))
    if inspection["bundle"]["manifest"]["contract"]["minor"] != 1:
        raise StockError("stock reader supports the exact Bundle contract 1.1")
    if len(plan["assets"]) > 16384 or sum(asset["size"] for asset in plan["assets"]) > BUNDLE_LIMIT - 4 * 1024 * 1024:
        raise StockError("Bundle exceeds stock reader decompression budget")
    languages = delivery.admit({}, [asset["path"] for asset in plan["assets"]],
        "embedded", available=("lua", "js"), typed_assets=plan["assets"])
    # The shared language owner admits ScriptAssets. The stock reader also
    # refuses script extensions/representations falsely labelled as data.
    script_representations = {"lua.source", "js.source", "python.source", "java.source",
                              "java.class", "c.class-ref", "cpp.class-ref", "csharp.class-ref"}
    for asset in plan["assets"]:
        extension = Path(asset["path"]).suffix.lower()
        if (extension in (".c", ".cpp", ".csharp") or
                (asset["type"] != "script" and
                 (extension in (".lua", ".jcejs") or asset["representation"] in script_representations))):
            raise StockError("stock runtime cannot provide mislabelled/native script content")
    if (_sha(payload) != plan["bundles"][0]["sha256"] or
            _read(bundle, BUNDLE_LIMIT) != payload):
        raise StockError("Bundle changed during public inspection")
    archive, protection = _secure_project(driver, runtime, work, payload)
    footer = footer_for(runtime.host, archive)
    delivery.verify_bundle_inputs(driver, plan)
    # Revalidate the relocatable runtime package after all external tools.
    current = load_runtime(runtime.path)
    if (current.raw != runtime.raw or current.host != runtime.host or
            _sha(_key_bytes(_read(runtime.key_file, 128))) != runtime.manifest["protection"]["key_sha256"]):
        raise StockError("stock runtime changed during inspection")
    with out.open("xb") as stream:
        stream.write(runtime.host)
        stream.write(archive)
        stream.write(footer)
        stream.flush()
        os.fsync(stream.fileno())
    produced = _read(out, IMAGE_LIMIT)
    prefix, embedded = inspect_envelope(produced)
    if prefix != runtime.host or embedded != archive:
        raise StockError("written stock application differs from selected inputs")
    receipt = {"schema": 1, "scope": "compilerless-stock-app-seal",
        "status": "SEALED_NOT_EXECUTED", "target": dict(_TARGET),
        "runtime_manifest": {"path": str(runtime.path), "sha256": _sha(runtime.raw)},
        "sdk": runtime.manifest["sdk"], "bundle": plan["bundles"][0],
        "startup_scene": plan["startup_scene"], "languages": languages,
        "output": {"path": str(out), "size": len(produced), "sha256": _sha(produced)},
        "imports": runtime.imports, "self_extracting": False,
        "project_protection": protection,
        "envelope": {"version": 2, "size": FOOTER.size,
                     "checksum": "fnv1a64-noncryptographic"},
        "runtime_execution_verified": False, "relocation_verified": False}
    _dump(receipt_path, receipt)
    return receipt


def add_arguments(parser):
    actions = parser.add_subparsers(dest="stock_action", required=True)
    build = actions.add_parser("build-runtime", help="build the stock component and native CPU controls (producer only)")
    build.add_argument("--sdk", required=True)
    build.add_argument("--jobs", type=int)
    publish = actions.add_parser("publish-runtime", help="publish an already-built stock runtime")
    publish.add_argument("--exe", required=True)
    publish.add_argument("--sdk", required=True)
    publish.add_argument("--license-file", required=True)
    publish.add_argument("--build-receipt", required=True)
    publish.add_argument("--build-receipt-sha256", required=True)
    publish.add_argument("--out", required=True)
    pack = actions.add_parser("seal", help="seal a verified Bundle without a compiler")
    pack.add_argument("--runtime-manifest", required=True)
    pack.add_argument("--bundle", required=True)
    pack.add_argument("--pak-key-file", required=True, help="explicit external key matching the stock producer identity")
    pack.add_argument("--out", required=True)
    pack.add_argument("--work-dir", required=True)
    pack.add_argument("--receipt", required=True)


def execute(args, driver=None):
    if args.stock_action == "build-runtime":
        from tools.build import stock_runtime
        return stock_runtime.build(driver, args)
    if args.stock_action == "publish-runtime":
        return publish_runtime(exe=args.exe, sdk=args.sdk, license_file=args.license_file, out=args.out,
            build_receipt=args.build_receipt, build_receipt_sha256=args.build_receipt_sha256)
    if args.stock_action == "seal":
        return seal(runtime_manifest=args.runtime_manifest, bundle=args.bundle, out=args.out,
            work_dir=args.work_dir, receipt=args.receipt, pak_key_file=args.pak_key_file)
    raise StockError("unknown stock application action")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    add_arguments(parser)
    args = parser.parse_args(argv)
    try:
        result = execute(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, "stock-app: " + str(error) + "\n")
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

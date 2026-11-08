"""Bounded SDK smoke inputs and proof checks; the public builder owns cooking.

No compiler, archive parser or key generator lives here. A pointer binds the
current fresh fixture; graph and authenticated-PAK evidence remain independent.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STARTUP = "scenes/smoke.scene.json"
POINTER = "sdk-smoke-dist-fixture.json"
POINTER_SCHEMA = "jce.sdk-smoke-dist-fixture.v2"
MAX_STAGE_BYTES = 8192
MAX_POINTER_BYTES = 16384
MAX_PROOF_JSON_BYTES = 4 * 1024 * 1024
SELECTED = {
    "smoke.png": (568, "00c1e22406b6bea68733186ba35e748e6a0183af87a3d0edbe577b43bcc28600", "texture", "jcea.texture.v1"),
    "smoke.wav": (1644, "b0ea94c3b6ce0101a8b06f8bdeb36f9f0cb5a80f45d215aae15f9b71614a3fee", "sound", "jcea.audio-pcm.v1"),
}
SCRIPT_SELECTED = {
    "scripts/smoke_py.py": (1107, "0c2fa95f6645160ec47c19f264fd64217e7bfe9e0a9d894e86ff8e50bc2cd418", "script", "python.source"),
}
FIXTURES = {"c99": ("JceSdkSmoke", SELECTED),
            "scripting": ("JceSdkSmokeScripting", SCRIPT_SELECTED)}


def _reject(reason: str) -> None:
    raise ValueError("DIST smoke fixture/proof: " + reason)


def _fixture(fixture: str) -> tuple[str, dict]:
    if fixture not in FIXTURES:
        _reject("unknown fixture kind: " + str(fixture))
    return FIXTURES[fixture]


def _checked_build_dir(build_dir: Path, source: Path | None = None) -> Path:
    lexical = Path(build_dir).absolute()
    directory = lexical.resolve()
    if lexical != directory or directory == Path(directory.anchor) or directory in ROOT.parents:
        _reject("aliased or root build directory: " + str(build_dir))
    if directory.is_relative_to(ROOT) and not directory.is_relative_to(ROOT / "build"):
        _reject("build directory is inside tracked source: " + str(directory))
    if source and (directory.is_relative_to(source) or source.is_relative_to(directory)):
        _reject("build directory overlaps source assets: " + str(directory))
    return directory


def _file(path: Path, directory: Path) -> Path:
    if path.absolute() != path.resolve() or path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(directory):
        _reject("missing, linked or escaped file: " + str(path))
    return path


def _read_selected(source: Path, name: str, selected: dict = SELECTED) -> bytes:
    path = _file(source / name, source)
    size, expected, _, _ = selected[name]
    with path.open("rb") as stream:
        data = stream.read(size + 1)
    actual = hashlib.sha256(data).hexdigest()
    if len(data) != size or actual != expected:
        _reject("selected source identity mismatch: " + str(path))
    return data


def _json(path: Path, directory: Path, limit: int = MAX_PROOF_JSON_BYTES) -> dict:
    _file(path, directory)
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        _reject("JSON exceeds byte budget: " + str(path))
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeError, ValueError) as error:
        _reject("malformed JSON: " + str(path) + ": " + str(error))
    if not isinstance(value, dict):
        _reject("JSON object required: " + str(path))
    return value


def _digest(path: Path, directory: Path) -> str:
    _file(path, directory)
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def _current_fixture(directory: Path, *, verify_payloads: bool, fixture: str = "c99",
                     allow_legacy: bool = False) -> Path:
    _, selected = _fixture(fixture)
    pointer = _json(directory / POINTER, directory, MAX_POINTER_BYTES)
    legacy = (allow_legacy and fixture == "c99" and
              pointer.get("schema") == "jce.sdk-smoke-dist-fixture.v1" and
              set(pointer) == {"schema", "project", "inputs_sha256"})
    if not legacy and (set(pointer) != {"schema", "fixture", "project", "inputs_sha256"} or
                       pointer.get("schema") != POINTER_SCHEMA or pointer.get("fixture") != fixture):
        _reject("unowned or malformed fixture pointer")
    if not isinstance(pointer.get("project"), str) or not pointer["project"]:
        _reject("fixture pointer project path is invalid")
    project = Path(pointer["project"]).absolute()
    if (project != project.resolve() or not project.is_dir() or
            project.parent != directory or not project.name.startswith("auth-fixture-") or
            pointer.get("inputs_sha256") != {name: value[1] for name, value in selected.items()}):
        _reject("fixture pointer escaped, linked or identity mismatch")
    if verify_payloads:
        for name in selected:
            _read_selected(project / "assets", name, selected)
    return project


def stage_dist_fixture(source_assets: Path, build_dir: Path, *, fixture: str = "c99") -> tuple[Path, dict]:
    target, selected = _fixture(fixture)
    source = Path(source_assets).absolute()
    if source != source.resolve() or not source.is_dir():
        _reject("missing or linked source assets: " + str(source))
    directory = _checked_build_dir(build_dir, source)
    payloads = {name: _read_selected(source, name, selected) for name in selected}
    if (directory / POINTER).exists() or (directory / POINTER).is_symlink():
        _current_fixture(directory, verify_payloads=False, fixture=fixture, allow_legacy=True)
    directory.mkdir(parents=True, exist_ok=True)
    project = Path(tempfile.mkdtemp(prefix="auth-fixture-", dir=directory))
    assets = project / "assets"
    (assets / "scenes").mkdir(parents=True)
    manifest = {"name": target, "target": target, "source_assets": "assets",
                "cooked_assets": "cooked", "startup_scene": STARTUP}
    documents = {
        project / "jce_project.json": manifest,
        assets / STARTUP: {"entities": []},
        assets / "bundle_roots.json": {"contract": {"name": "jce.bundle.dependencies", "major": 1, "minor": 0},
                                       "assets": list(selected)},
    }
    encoded = {path: (json.dumps(value, indent=2) + "\n").encode("utf-8") for path, value in documents.items()}
    if sum(map(len, payloads.values())) + sum(map(len, encoded.values())) > MAX_STAGE_BYTES:
        _reject("stage exceeds byte budget")
    for name, data in payloads.items():
        (assets / name).parent.mkdir(parents=True, exist_ok=True)
        (assets / name).write_bytes(data)
    for path, data in encoded.items():
        path.write_bytes(data)
    for name, data in payloads.items():
        if _read_selected(source, name, selected) != data or _read_selected(assets, name, selected) != data:
            _reject("selected source changed during staging: " + name)
    pointer = {"schema": POINTER_SCHEMA, "fixture": fixture, "project": str(project),
               "inputs_sha256": {name: value[1] for name, value in selected.items()}}
    temporary = directory / (project.name + ".pointer.tmp")
    temporary.write_text(json.dumps(pointer, indent=2) + "\n", encoding="utf-8", newline="\n")
    os.replace(temporary, directory / POINTER)
    return project, manifest


def validate_dist_proof(build_dir: Path, *, fixture: str = "c99") -> dict:
    _, payloads = _fixture(fixture)
    directory = _checked_build_dir(build_dir)
    project = _current_fixture(directory, verify_payloads=True, fixture=fixture)
    generated = directory / "jce_generated"
    graph_dir = generated / "content_graph"
    paths = [graph_dir / name for name in ("admission.jbundle", "admission.jbundle.json", "bundle_graph.json")]
    for path in paths:
        _file(path, directory)
    metadata = _json(paths[1], directory)
    _json(paths[2], directory)  # Bound reads before consuming the existing graph authority.
    spec = importlib.util.spec_from_file_location("jce_sdk_smoke_delivery", ROOT / "tools/build/delivery.py")
    delivery = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(delivery)
    addresses, assets = delivery._single_admission_assets(graph_dir, STARTUP)
    if set(addresses) != {*payloads, STARTUP}:
        _reject("selected closure differs from the declared fixture addresses")
    by_path = {asset["path"]: asset for asset in assets}
    for name, (source_size, _, kind, representation) in payloads.items():
        if by_path[name]["type"] != kind or by_path[name]["representation"] != representation:
            _reject("selected payload is not the required cooked type: " + name)
        if kind == "script" and by_path[name]["size"] != source_size:
            _reject("script source byte count changed: " + name)
    bom_path = generated / "project_assets.bom.json"
    bom = _json(bom_path, directory)
    receipt = _json(generated / "project_assets.receipt.json", directory)
    pak = _file(generated / "project_assets.pak", directory)
    header, totals, selected = bom.get("header", {}), bom.get("totals", {}), bom.get("selected_source", {})
    if not all(isinstance(value, dict) for value in (header, totals, selected)):
        _reject("malformed BOM proof objects")
    if not isinstance(metadata.get("content_hash"), str) or not re.fullmatch("[0-9a-fA-F]{16}", metadata["content_hash"]):
        _reject("missing selected archive content identity")
    count = totals.get("entries")
    if (bom.get("schema") != "jce.pakbom.v1" or bom.get("file") != str(pak) or
            bom.get("file_size") != pak.stat().st_size or bom.get("sources_verified") is not True or
            any(header.get(key) is not True for key in ("header_verified", "encrypted", "secure_index", "authenticated", "auth_verified")) or
            type(count) is not int or count <= 0 or totals.get("verify_ran") is not True or
            any(totals.get(key) != count for key in ("verified_count", "encrypted_count", "authenticated_count")) or
            totals.get("corrupt_count") != 0 or totals.get("skipped_count") != 0 or
            selected.get("path") != str(paths[0]) or selected.get("size") != paths[0].stat().st_size or
            selected.get("content_id") != metadata.get("content_hash") or selected.get("imported_count") != len(assets)):
        _reject("authenticated BOM, selected identity or deep verification incomplete")
    rows = bom.get("sources")
    if not isinstance(rows, list) or len(rows) != count:
        _reject("missing BOM source rows")
    row_paths = [delivery._admission_address(row.get("path")) for row in rows if isinstance(row, dict)]
    if len(row_paths) != count or len(set(row_paths)) != count or "__bundle__/manifest.json" in row_paths:
        _reject("duplicate, invalid or reserved BOM address")
    selected_rows = {row["path"]: row for row in rows if row.get("selected") is True}
    if set(selected_rows) != set(by_path):
        _reject("BOM selected closure mismatch")
    for name, asset in by_path.items():
        row = selected_rows[name]
        if row.get("source") != str(paths[0]) or row.get("size") != asset["size"] or row.get("content_id") != asset["content_id"]:
            _reject("BOM selected byte identity mismatch: " + name)
    embed = generated / "project_assets.obj"
    if not embed.exists():
        embed = pak
    inputs = {str(path): _digest(path, directory) for path in paths}
    hashes = {"pak_sha256": _digest(pak, directory), "bom_sha256": _digest(bom_path, directory),
              "embed_sha256": _digest(embed, directory)}
    _file(generated / "jce_pak_key.c", directory)
    if (receipt.get("schema") != "jce.dist.asset-receipt.v1" or receipt.get("deep_verification") != "PASS" or
            receipt.get("selected_count") != len(assets) or receipt.get("packed_count") != count or
            receipt.get("inputs_sha256") != inputs or any(receipt.get(key) != value for key, value in hashes.items())):
        _reject("actual builder receipt identity mismatch")
    return {"schema": "jce.sdk-smoke-dist-proof.v1", "fixture": fixture, "fixture_project": str(project),
            "stage_inputs_sha256": {name: value[1] for name, value in payloads.items()},
            "payloads": {name: {key: by_path[name][key] for key in ("size", "asset_id", "content_id", "type", "representation")}
                         for name in payloads}, "selected_count": len(assets), "packed_count": count,
            **hashes, "authenticated_deep_verification": "PASS", "native_run": "NOT_RUN_BY_HELPER", "gpu": "NOT_RUN"}

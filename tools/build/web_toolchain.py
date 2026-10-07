"""Read Emscripten identity without changing SDKs or Conan cache settings.

Some dependency recipes reject Conan's emcc model, so compiler settings use the
actual underlying Clang major. Complete emcc version and selected ABI headers
remain in the additional digest included in every normal compiled host package
IDs by the wasm profile's tools.info.package_id:confs declaration. This is a
bounded identity witness, not a source-purity audit of the entire sysroot.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess

_VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9._-]+)?")
_HEADERS = (
    "system/include/emscripten/html5.h",
    "system/include/emscripten/html5_webgl.h",
    "system/include/emscripten/emscripten.h",
)


def parse_emcc_version(banner: str) -> str:
    """Reject arbitrary Clang text instead of manufacturing an emcc identity."""
    first = banner.splitlines()[0] if banner else ""
    match = re.match(r"^emcc \(.*\) (" + _VERSION.pattern + r")(?:\s|$)", first)
    if not match:
        raise ValueError("emcc --version did not report an Emscripten full version")
    return match.group(1)


def parse_clang_major(banner: str) -> str:
    """Read actual LLVM identity; unsupported Conan versions must fail, not lie."""
    first = banner.splitlines()[0] if banner else ""
    match = re.match(r"^clang version ([1-9][0-9]*)\.[0-9]+\.[0-9]+"
                     r"[A-Za-z0-9.+-]*(?:\s|$)", first)
    if not match:
        raise ValueError("underlying compiler did not report a complete Clang version")
    return match.group(1)


def _version_file(path: Path) -> str:
    text = path.read_text(encoding="utf-8").strip()
    if text.startswith('"'):
        text = json.loads(text)
    if not isinstance(text, str) or not _VERSION.fullmatch(text):
        raise ValueError("invalid emscripten-version.txt")
    return text


def _version_output(executable: Path, env: dict | None) -> str:
    result = subprocess.run([str(executable), "--version"], env=env,
                            capture_output=True, text=True, timeout=30,
                            check=False)
    if result.returncode != 0:
        raise ValueError(f"toolchain version query failed: {executable}: "
                         f"{result.stderr.strip()}")
    return result.stdout.strip()


def probe_toolchain(em_paths: dict, env: dict | None = None) -> dict:
    """Read the actual wrappers/headers; no compilation, activation or writes."""
    cc = Path(em_paths["cc"])
    directory = cc.parent
    toolchain = Path(em_paths["toolchain"])
    if not cc.is_file() or not Path(em_paths["cxx"]).is_file():
        raise ValueError("Emscripten compiler wrappers are missing")
    banner = _version_output(cc, env)
    version = parse_emcc_version(banner)
    if _version_file(directory / "emscripten-version.txt") != version:
        raise ValueError("emcc and emscripten-version.txt versions disagree")
    clang = directory.parent / "bin" / ("clang.exe" if cc.suffix == ".bat" else "clang")
    clang_banner = _version_output(clang, env)
    clang_major = parse_clang_major(clang_banner)
    headers = {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
               for name in _HEADERS}
    # Keep paths out of binary identity: moving identical SDK bytes is not an ABI change.
    identity = {"schema": 2, "emcc_version": version, "emcc_banner": banner,
                "clang_major": clang_major, "clang_banner": clang_banner, "headers_sha256": headers,
                "toolchain_sha256": hashlib.sha256(toolchain.read_bytes()).hexdigest()}
    payload = dict(identity)
    payload["clang_banner"] = "\n".join(
        line for line in clang_banner.splitlines()
        if not line.startswith("InstalledDir:"))
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    identity["abi_sha256"] = hashlib.sha256(encoded).hexdigest()
    return identity


def conan_host_args(identity: dict, profile: Path | str) -> list[str]:
    """Select standard host settings and the globally inherited ABI conf."""
    version = identity.get("emcc_version")
    digest = identity.get("abi_sha256")
    clang_major = identity.get("clang_major")
    if clang_major != parse_clang_major(identity.get("clang_banner", "")):
        raise ValueError("Clang major does not match the actual compiler banner")
    if not isinstance(version, str) or not _VERSION.fullmatch(version):
        raise ValueError("missing actual Emscripten full version")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("missing actual Web toolchain ABI digest")
    contents = Path(profile).read_text(encoding="utf-8")
    if not re.search(r"^compiler=clang\s*$", contents, re.MULTILINE):
        raise ValueError("Web profile must use the actual underlying Clang compiler model")
    if not re.search(r"^tools\.info\.package_id:confs=\['user\.jce:web_toolchain_abi'\]\s*$",
                     contents, re.MULTILINE):
        raise ValueError("Web ABI conf must explicitly participate in host package IDs")
    return ["-pr:h", str(profile), "-s:h", "compiler=clang", "-s:h",
            "compiler.version=" + clang_major, "-c:h",
            "user.jce:web_toolchain_abi=" + digest]

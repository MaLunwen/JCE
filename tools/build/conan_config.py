"""First-party Conan configuration overlay sharing the existing package store.

Only small configuration is copied into a caller-owned build directory. Original
settings and package/source bytes are never edited; authentication databases are
not copied. Public remotes are the default for this explicit toolchain overlay.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re

_SENSITIVE = re.compile(r"(?:password|passwd|token|secret|credential|authorization|authentication|api[_-]?key|access[_-]?key)", re.I)
_CREDENTIAL_URL = re.compile(r"://[^/\s]*@")
_LIMIT = 1024 * 1024


def _small_read(path: Path) -> bytes:
    if path.stat().st_size > _LIMIT:
        raise ValueError("Conan configuration exceeds the small-file budget: " + path.name)
    return path.read_bytes()


def _atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".jce-config.tmp")
    temporary.write_bytes(content)
    os.replace(temporary, path)


def prepare_toolchain_home(base_home: Path | str, overlay_home: Path | str,
                           actual_clang_version: str) -> dict:
    """Prepare isolated settings, with the original store/database still shared.

    Requires Conan's existing Python dependencies. Version additions use its
    documented settings_user list merge, rather than replacing global settings.
    Caller must pass CONAN_HOME=returned home only to the intended Conan process.
    """
    import yaml
    from conan.internal.model.conf import ConfDefinition
    from conan.internal.model.settings import load_settings_yml

    if not isinstance(actual_clang_version, str) or not re.fullmatch(r"[1-9][0-9]*", actual_clang_version):
        raise ValueError("actual Clang major is required")
    base = Path(base_home).resolve()
    overlay = Path(overlay_home).resolve()
    if base == overlay or base in overlay.parents or overlay in base.parents:
        raise ValueError("Conan overlay must be separate from the original home")
    settings = _small_read(base / "settings.yml")
    user_path = base / "settings_user.yml"
    user_bytes = _small_read(user_path) if user_path.is_file() else b"{}"
    user = yaml.safe_load(user_bytes) or {}
    if not isinstance(user, dict):
        raise ValueError("settings_user must be a mapping")
    compiler = user.setdefault("compiler", {})
    if not isinstance(compiler, dict):
        raise ValueError("settings_user compiler must be a mapping")
    clang = compiler.setdefault("clang", {})
    if not isinstance(clang, dict):
        raise ValueError("settings_user Clang must be a mapping")
    versions = clang.setdefault("version", [])
    if not isinstance(versions, list):
        raise ValueError("settings_user Clang versions must be a list")
    if actual_clang_version not in [str(value) for value in versions]:
        versions.append(actual_clang_version)

    global_path = base / "global.conf"
    global_bytes = _small_read(global_path) if global_path.is_file() else b""
    configuration = ConfDefinition()
    configuration.loads(global_bytes.decode("utf-8"))
    configured_store = configuration.get("core.cache:storage_path", default=None)
    store = Path(configured_store) if configured_store else base / "p"
    store = store.resolve()
    if store == overlay or overlay in store.parents:
        raise ValueError("Conan package store cannot live inside the configuration overlay")
    safe_lines = []
    for line in configuration.dumps().splitlines():
        key, _, value = line.partition("=")
        if key == "core.cache:storage_path":
            continue
        if not (key.startswith(("tools.", "user.")) or key == "core:non_interactive"):
            continue
        if _SENSITIVE.search(key) or _SENSITIVE.search(value) or _CREDENTIAL_URL.search(value):
            continue
        safe_lines.append(line)
    safe_lines.extend(["core.cache:storage_path=" + store.as_posix(),
                       "core:non_interactive=True"])
    profiles = {}
    profiles_path = base / "profiles"
    if profiles_path.is_dir():
        for path in sorted(profiles_path.rglob("*")):
            if not path.is_file():
                continue
            if profiles_path.resolve() not in path.resolve().parents:
                raise ValueError("profile symlink leaves original profile directory")
            data = _small_read(path).decode("utf-8")
            lines = [line for line in data.splitlines() if not line.lstrip().startswith("#")]
            clean = "\n".join(lines) + "\n"
            if _SENSITIVE.search(clean) or _CREDENTIAL_URL.search(clean):
                raise ValueError("profile contains authentication configuration: " + path.name)
            profiles[path.relative_to(profiles_path)] = clean.encode("utf-8")
    if sum(len(data) for data in profiles.values()) > 4 * _LIMIT:
        raise ValueError("Conan profiles exceed the overlay configuration budget")

    marker = overlay / ".jce-toolchain-overlay.json"
    if overlay.exists() and any(overlay.iterdir()) and not marker.is_file():
        raise ValueError("refusing to overwrite an unowned Conan configuration directory")
    targets = [overlay / name for name in (
        "settings.yml", "settings_user.yml", "global.conf", "remotes.json",
        ".jce-toolchain-overlay.json")]
    targets.extend(overlay / "profiles" / relative for relative in profiles)
    for target in targets:
        if overlay not in target.resolve().parents:
            raise ValueError("overlay configuration target escapes its owned directory")
    _atomic_write(overlay / "settings.yml", settings)
    # JSON is YAML-compatible and avoids duplicate YAML compiler keys.
    _atomic_write(overlay / "settings_user.yml", (json.dumps(user, indent=2) + "\n").encode())
    _atomic_write(overlay / "global.conf", ("\n".join(safe_lines) + "\n").encode())
    remotes = {"remotes": [{"name": "conancenter", "url": "https://center2.conan.io", "verify_ssl": True}]}
    _atomic_write(overlay / "remotes.json", (json.dumps(remotes, indent=2) + "\n").encode())
    for relative, data in profiles.items():
        _atomic_write(overlay / "profiles" / relative, data)
    merged = load_settings_yml(str(overlay))
    merged.compiler = "clang"
    merged.compiler.version = actual_clang_version
    merged.compiler.version.validate()
    result = {"home": str(overlay), "storage": str(store), "clang_major": actual_clang_version,
              "settings_sha256": hashlib.sha256(settings).hexdigest(),
              "source_user_settings_sha256": hashlib.sha256(user_bytes).hexdigest(),
              "source_global_conf_sha256": hashlib.sha256(global_bytes).hexdigest(),
              "profiles": [str(path) for path in profiles],
              "scope": "configuration overlay; shared original package store, public remotes, no copied authentication"}
    _atomic_write(marker, (json.dumps(result, indent=2) + "\n").encode())
    return result

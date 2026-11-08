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
import shutil
import stat

_SENSITIVE = re.compile(r"(?:password|passwd|token|secret|credential|authorization|authentication|api[_-]?key|access[_-]?key)", re.I)
_CREDENTIAL_URL = re.compile(r"://[^/\s]*@")
_LIMIT = 1024 * 1024
_PROFILE_LIMIT = 4 * _LIMIT
_PROFILE_ENTRIES_LIMIT = 4096


def _unaliased(path: Path | str) -> Path:
    original = Path(path).absolute()
    resolved = original.resolve()
    if original != resolved:
        raise ValueError("Conan configuration path is redirected: " + str(original))
    return resolved


def _small_read(path: Path) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(_LIMIT + 1)
    if len(data) > _LIMIT:
        raise ValueError("Conan configuration exceeds the small-file budget: " + path.name)
    return data


def configuration_digest(home: Path | str) -> str:
    """Hash only bounded settings/conf/profiles, never packages or credentials.

    Missing selected files/directories have explicit null entries. Permission,
    size, type and containment failures propagate; none can bless reuse.
    """
    base = Path(home).resolve()
    def status(path):
        try:
            return path.stat()
        except FileNotFoundError:
            return None
    base_status = status(base)
    if base_status and not stat.S_ISDIR(base_status.st_mode):
        raise ValueError("Conan configuration home is not a directory")
    def checked(path):
        if base not in path.resolve().parents or path.resolve() != path:
            raise ValueError("Conan configuration path escapes selected home")
    files = {}
    for name in ("settings.yml", "settings_user.yml", "global.conf"):
        path = base / name
        checked(path)
        entry = status(path)
        if entry is None:
            files[name] = None
        elif not stat.S_ISREG(entry.st_mode):
            raise ValueError("Conan configuration input is not a file: " + name)
        else:
            files[name] = hashlib.sha256(_small_read(path)).hexdigest()
    profiles_path = base / "profiles"
    checked(profiles_path)
    profiles = None
    profile_status = status(profiles_path)
    if profile_status is not None:
        if not stat.S_ISDIR(profile_status.st_mode):
            raise ValueError("Conan profiles input is not a directory")
        profiles = {}
        total = 0
        count = 0
        pending = [profiles_path]
        while pending:
            # scandir propagates directory access failures; Path.rglob may
            # suppress those errors and incorrectly hash an incomplete set.
            with os.scandir(pending.pop()) as entries:
                for item in entries:
                    count += 1
                    if count > _PROFILE_ENTRIES_LIMIT:
                        raise ValueError("Conan profiles exceed the configuration entry budget")
                    path = Path(item.path)
                    checked(path)
                    entry = path.stat()
                    if stat.S_ISDIR(entry.st_mode):
                        pending.append(path)
                        continue
                    if not stat.S_ISREG(entry.st_mode):
                        raise ValueError("Conan profile input is not a regular file")
                    data = _small_read(path)
                    total += len(data)
                    if total > _PROFILE_LIMIT:
                        raise ValueError("Conan profiles exceed the overlay configuration budget")
                    profiles[path.relative_to(profiles_path).as_posix()] = hashlib.sha256(data).hexdigest()
    payload = {"schema": 1, "files": files, "profiles": profiles}
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()


def _atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".jce-config.tmp")
    temporary.write_bytes(content)
    os.replace(temporary, path)


def _hook_state(home: Path, hooks: dict, retired: dict) -> tuple:
    configuration_digest(home)
    destination = home / "extensions/hooks"
    for directory in (home / "extensions", destination):
        _unaliased(directory)
        if directory.exists() and not directory.is_dir():
            raise ValueError("Conan hook directory is not a directory: " + str(directory))
    retiring, conflicts = [], []
    for name in sorted(set(hooks) | set(retired)):
        installed = _unaliased(destination / name)
        if not installed.exists():
            continue
        if not installed.is_file():
            raise ValueError("installed Conan hook is not a file: " + str(installed))
        data = _small_read(installed)
        if name in hooks and data == hooks[name]:
            continue
        if name in retired and hashlib.sha256(data).hexdigest() == retired[name]:
            retiring.append((installed, data))
        else:
            conflicts.append(installed)
    return destination, retiring, conflicts


def _hook_inputs(repository: Path) -> tuple:
    source = repository / "conan/hooks"
    hooks = {}
    for path in sorted(source.glob("hook_*.py")):
        _unaliased(path)
        if not path.is_file():
            raise ValueError("owned Conan hook is not a file: " + str(path))
        hooks[path.name] = _small_read(path)
    retired = json.loads((repository / "contracts/conan-source-policy.json").read_text(encoding="utf-8"))["retired_hooks"]
    if not isinstance(retired, dict) or any(
            not re.fullmatch(r"hook_[A-Za-z0-9_]+\.py", name) or
            not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)
            for name, digest in retired.items()):
        raise ValueError("invalid retired Conan hook policy")
    return hooks, retired


def _requested_hook_home(env: dict) -> tuple:
    explicit = "CONAN_HOME" in env
    if explicit and not env["CONAN_HOME"]:
        raise ValueError("CONAN_HOME must not be empty")
    home = _unaliased(env["CONAN_HOME"] if explicit else Path.home() / ".conan2")
    return home, explicit


def select_hook_home(repository: Path | str, env: dict) -> dict:
    """Read-only native hook-home plan, shared by builds and installed-hook lint.

    Inspect the selected home without creating it or repairing installed bytes.
    Current owned inputs determine its identity; stale selected bytes cannot
    select another tuple. This plan is not evidence that a build executed.
    """
    repository = Path(repository).resolve()
    hooks, retired = _hook_inputs(repository)
    base, explicit = _requested_hook_home(env)
    home = base
    destination, retiring, conflicts = _hook_state(home, hooks, retired)
    if conflicts and not explicit:
        identity = {"base": str(base), "hooks": {
            name: hashlib.sha256(data).hexdigest() for name, data in hooks.items()}, "retired": retired}
        key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:16]
        home = _unaliased(repository / "build/conan-config" / ("native-" + key))
        destination, retiring, conflicts = _hook_state(home, hooks, retired)
    return {"base": base, "home": home, "explicit": explicit, "hooks": hooks,
            "retired": retired, "destination": destination,
            "retiring": retiring, "conflicts": conflicts}


def sync_hooks(repository: Path | str, env: dict, *, dry_run=False, log=print) -> None:
    """Synchronize only the admitted home; never overwrite unknown hook bytes.

    Only the passed process environment is changed. Explicit homes retain their
    refusal contract; default selection shares the read-only lint policy.
    """
    repository = Path(repository).resolve()
    source = repository / "conan/hooks"
    if dry_run:
        hooks, _ = _hook_inputs(repository)
        home, _ = _requested_hook_home(env)
        log(f"syncing {len(hooks)} configuration-only Conan hooks -> {home / 'extensions/hooks'}")
        return
    selection = select_hook_home(repository, env)
    home, hooks = selection["home"], selection["hooks"]
    destination, retiring, conflicts = (
        selection["destination"], selection["retiring"], selection["conflicts"])
    if conflicts:
        advice = ("use a separate CONAN_HOME" if selection["explicit"] else
                  "preserve the owned configuration home")
        raise ValueError(f"unknown installed hook {conflicts[0]}; {advice}")
    if home != selection["base"]:
        config = prepare_configuration_home(selection["base"], home)
        env["CONAN_HOME"] = config["home"]
        log(f"Native Conan configuration overlay: {config['home']}; shared store: {config['storage']}")
    archives = []
    for installed, data in retiring:
        archive = _unaliased(repository / "docs/delivery/retired-installed-conan-hooks" / installed.name)
        if archive.exists() and (not archive.is_file() or archive.read_bytes() != data):
            raise ValueError("hook archive differs: " + str(archive))
        archives.append((installed, archive, data))
    log(f"syncing {len(hooks)} configuration-only Conan hooks -> {destination}")
    destination.mkdir(parents=True, exist_ok=True)
    for installed, archive, data in archives:
        archive.parent.mkdir(parents=True, exist_ok=True)
        if not archive.exists():
            shutil.copy2(installed, archive)
        if archive.read_bytes() != data:
            raise ValueError("hook archive verification failed: " + str(archive))
        installed.unlink()
    for name, data in hooks.items():
        installed = destination / name
        if installed.is_file() and installed.read_bytes() == data:
            continue
        shutil.copy2(source / name, installed)
        if installed.read_bytes() != data:
            raise ValueError("owned Conan hook changed during synchronization: " + name)


def prepare_configuration_home(base_home: Path | str, overlay_home: Path | str) -> dict:
    """Isolate native configuration without changing compiler settings.

    Original settings bytes remain intact in the overlay. Only the returned
    home receives first-party hooks; the original package store stays shared.
    """
    return _prepare_home(base_home, overlay_home, None)


def prepare_compiler_home(base_home: Path | str, overlay_home: Path | str,
                          actual_clang_version: str, *, package_store: Path | str | None = None) -> dict:
    """Isolate configuration for a separately probed native or Web Clang.

    The caller supplies the actual compiler major, never a compatibility alias.
    An explicit independent store permits a fresh dependency provenance scope
    without copying or changing the existing package/source cache.
    """
    if not isinstance(actual_clang_version, str) or not re.fullmatch(r"[1-9][0-9]*", actual_clang_version):
        raise ValueError("actual Clang major is required")
    return _prepare_home(base_home, overlay_home, actual_clang_version, package_store=package_store)


def prepare_toolchain_home(base_home: Path | str, overlay_home: Path | str,
                           actual_clang_version: str) -> dict:
    """Prepare Web settings with the actual Clang major and a shared store.

    Requires Conan's existing Python dependencies. Version additions use its
    documented settings_user list merge, rather than replacing global settings.
    Caller must pass CONAN_HOME=returned home only to the intended Conan process.
    """
    return prepare_compiler_home(base_home, overlay_home, actual_clang_version)


def _prepare_home(base_home: Path | str, overlay_home: Path | str,
                  actual_clang_version: str | None, *, package_store: Path | str | None = None) -> dict:
    """One bounded configuration/store owner for native and Web callers."""
    import yaml
    from conan.internal.model.conf import ConfDefinition
    from conan.internal.model.settings import load_settings_yml

    base = _unaliased(base_home)
    overlay = _unaliased(overlay_home)
    if base == overlay or base in overlay.parents or overlay in base.parents:
        raise ValueError("Conan overlay must be separate from the original home")
    configuration_digest(base)
    load_settings_yml(str(base))  # Validate the original schema before output.
    if overlay.exists() and not overlay.is_dir():
        raise ValueError("Conan overlay is not a directory")
    settings = _small_read(base / "settings.yml")
    user_path = base / "settings_user.yml"
    user_bytes = _small_read(user_path) if user_path.is_file() else b"{}"
    user = yaml.safe_load(user_bytes) or {}
    if not isinstance(user, dict):
        raise ValueError("settings_user must be a mapping")
    user_output = user_bytes
    if actual_clang_version is not None:
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
        # JSON is YAML-compatible and avoids duplicate YAML compiler keys.
        user_output = (json.dumps(user, indent=2) + "\n").encode()

    global_path = base / "global.conf"
    global_bytes = _small_read(global_path) if global_path.is_file() else b""
    configuration = ConfDefinition()
    configuration.loads(global_bytes.decode("utf-8"))
    configured_store = configuration.get("core.cache:storage_path", default=None)
    store = Path(configured_store) if configured_store else base / "p"
    store = store.resolve()
    original_store = store
    if package_store is not None:
        store = Path(package_store).resolve()
        if (store == original_store or store == base or store in base.parents
                or base in store.parents):
            raise ValueError("isolated Conan store must be independent of the original home/store")
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
        for candidate in (target, target.with_name(target.name + ".jce-config.tmp")):
            if overlay not in _unaliased(candidate).parents:
                raise ValueError("overlay configuration target escapes its owned directory")
            if candidate.exists() and not candidate.is_file():
                raise ValueError("overlay configuration target is not a file")
    _atomic_write(overlay / "settings.yml", settings)
    _atomic_write(overlay / "settings_user.yml", user_output)
    _atomic_write(overlay / "global.conf", ("\n".join(safe_lines) + "\n").encode())
    remotes = {"remotes": [{"name": "conancenter", "url": "https://center2.conan.io", "verify_ssl": True}]}
    _atomic_write(overlay / "remotes.json", (json.dumps(remotes, indent=2) + "\n").encode())
    for relative, data in profiles.items():
        _atomic_write(overlay / "profiles" / relative, data)
    merged = load_settings_yml(str(overlay))
    if actual_clang_version is not None:
        merged.compiler = "clang"
        merged.compiler.version = actual_clang_version
        merged.compiler.version.validate()
    result = {"home": str(overlay), "storage": str(store), "clang_major": actual_clang_version,
              "settings_sha256": hashlib.sha256(settings).hexdigest(),
              "source_user_settings_sha256": hashlib.sha256(user_bytes).hexdigest(),
              "source_global_conf_sha256": hashlib.sha256(global_bytes).hexdigest(),
              "profiles": [str(path) for path in profiles],
              "scope": ("configuration overlay; shared original package store, public remotes, no copied authentication"
                        if package_store is None else
                        "configuration overlay; independent selected package store, public remotes, no copied authentication")}
    _atomic_write(marker, (json.dumps(result, indent=2) + "\n").encode())
    return result

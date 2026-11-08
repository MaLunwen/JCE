"""Isolated native embedded SDK output policy, not a second build recipe.

Request stamps bind cooperating producer retries. Only compiler-generated SDK
target metadata and actual archives admit installed target compatibility.
"""
from __future__ import annotations

import json
import os
import stat
from pathlib import Path
import tempfile
from types import SimpleNamespace

REQUEST_FILE = ".jce-sdk-output-request.json"
OPTIONS_FILE = "sdk-build-options.json"
_STATES = frozenset(("requested", "configure_failed", "configured",
                     "build_failed", "built", "install_failed", "installed"))


def add_arguments(parser):
    parser.add_argument("--staging-dir", type=Path,
                        help="native embedded SDK prefix under build/sdk/; reuse the canonical matching cache")
    parser.add_argument("--build-dir", type=Path,
                        help="isolated native embedded SDK cache under build/; requires --install-dir")
    parser.add_argument("--install-dir", type=Path,
                        help="isolated SDK prefix under dist/sdk/; requires --build-dir")
    parser.add_argument("--embed-engine-shaders", choices=("auto", "on", "off"), default="auto",
                        help="native embedded stock shaders: auto/on=shipping ON; off requires isolated outputs")


def selection(args, target, delivery):
    build, install = getattr(args, "build_dir", None), getattr(args, "install_dir", None)
    staging = getattr(args, "staging_dir", None)
    shaders = getattr(args, "embed_engine_shaders", "auto")
    if staging and (build or install):
        raise ValueError("sdk: --staging-dir excludes --build-dir and --install-dir")
    if shaders not in ("auto", "on", "off"):
        raise ValueError("sdk: --embed-engine-shaders requires auto, on or off")
    if bool(build) != bool(install):
        raise ValueError("sdk: --build-dir and --install-dir must be supplied together")
    if (build or staging or shaders != "auto") and (delivery != "embedded" or
            target.get("emscripten") or target.get("cross")):
        raise ValueError("sdk: isolated outputs/shader selection require native embedded delivery")
    if shaders == "off" and not build:
        raise ValueError("sdk: shader OFF requires isolated --build-dir and --install-dir")
    if staging and getattr(args, "clean", False):
        raise ValueError("sdk: --clean is unsupported with --staging-dir")
    if build and getattr(args, "clean", False):
        raise ValueError("sdk: --clean is unsupported for isolated outputs; select a fresh pair")
    if (build or staging) and args.variant not in ("release", "dist"):
        raise ValueError("sdk: isolated outputs require one release/dist variant")


def admit(refuse, args, target, delivery):
    try:
        selection(args, target, delivery)
    except ValueError as error:
        refuse(str(error))


def _overlap(left, right):
    return left == right or left.is_relative_to(right) or right.is_relative_to(left)


def _bounded_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate SDK output stamp key: " + key)
            result[key] = value
        return result
    with path.open("rb") as stream:
        data = stream.read(16385)
    if len(data) > 16384:
        raise ValueError("SDK output stamp exceeds 16384 bytes: " + str(path))
    def reject_constant(value):
        raise ValueError("nonfinite SDK output stamp scalar: " + value)
    result = json.loads(data.decode("utf-8"), object_pairs_hook=unique,
                        parse_constant=reject_constant)
    if type(result) is not dict:
        raise ValueError("SDK output stamp must be an object: " + str(path))
    return result


def _request_stamp(path, request):
    data = _bounded_json(path)
    if set(data) != {"schema", "request", "state"} or type(data["schema"]) is not int or data["schema"] != 1:
        raise ValueError("invalid SDK output request stamp: " + str(path))
    if data["request"] != request or type(data["state"]) is not str or data["state"] not in _STATES:
        raise ValueError("SDK output request identity mismatch: " + str(path))
    return data


def _cache_identity(plan, *, shared=False):
    cache = plan["build"] / "CMakeCache.txt"
    if not cache.is_file():
        return
    with cache.open("rb") as stream:
        encoded = stream.read(1048577)
    if len(encoded) > 1048576:
        raise ValueError("isolated SDK CMake cache exceeds 1 MiB")
    actual = {}
    for line in encoded.decode("utf-8").splitlines():
        if line.startswith(("//", "#")) or ":" not in line or "=" not in line:
            continue
        typed, value = line.split("=", 1)
        key = typed.partition(":")[0]
        if key in actual:
            raise ValueError("duplicate isolated SDK CMake cache key: " + key)
        actual[key] = value
    paths = {"CMAKE_HOME_DIRECTORY": plan["source"],
             "CMAKE_CACHEFILE_DIR": plan["build"],
             "CMAKE_INSTALL_PREFIX": plan["install"],
             "CMAKE_TOOLCHAIN_FILE": plan["toolchain"]}
    values = {"CMAKE_BUILD_TYPE": "Release", "JCE_BUILD_VARIANT": plan["variant"],
              "JCE_DELIVERY_PROFILE": "embedded", "JCE_EDITOR_STANDALONE": "ON",
              "JCE_ENABLE_SDK_INSTALL": "ON", "JCE_EMBED_ENGINE_SHADERS": plan["shaders"]}
    if shared:
        # Configure replaces a prior Editor/default SDK prefix and enable bit;
        # source, toolchain, variant, profile and shipping shaders must match.
        paths.pop("CMAKE_INSTALL_PREFIX")
        values.pop("JCE_ENABLE_SDK_INSTALL")
        if actual.get("JCE_ENABLE_SDK_INSTALL") not in ("ON", "OFF"):
            raise ValueError("shared SDK cache identity mismatch: JCE_ENABLE_SDK_INSTALL")
    for key, expected in paths.items():
        if not actual.get(key) or Path(actual[key]).resolve() != expected:
            raise ValueError("isolated SDK cache identity mismatch: " + key)
    for key, expected in values.items():
        if actual.get(key) != expected:
            raise ValueError("isolated SDK cache identity mismatch: " + key)


def _staging_prefix(root, value):
    requested = Path(value).absolute()
    parent = root / "build/sdk"
    if ".." in requested.parts or requested == parent or not requested.is_relative_to(parent):
        raise ValueError("sdk: staging prefix must be a strict child of build/sdk")
    current = root
    for part in requested.relative_to(root).parts:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            continue
        if (stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400 or
                not stat.S_ISDIR(info.st_mode)):
            raise ValueError("sdk: staging path is not an ordinary directory: " + str(current))
    if requested.resolve() != requested:
        raise ValueError("sdk: staging path traverses an alias")
    return requested


def _output_request(plan, target):
    request = {key: str(plan[key]) for key in
               ("source", "build", "install", "toolchain", "system", "arch", "variant", "shaders")}
    request.update(delivery="embedded", config="Release", target=target["key"])
    if plan.get("staging"):
        request["mode"] = "shared-staging"
    _json_bytes({"schema": 1, "request": request, "state": "configure_failed"})
    return request


def _installed_output_identity(driver, plan):
    install = plan["install"]
    if install.exists() and any(install.iterdir()):
        driver._sdk_target_module().require_sdk_target(install, plan["system"],
            plan["arch"], "Release", plan["variant"], "embedded")
        if _bounded_json(install / OPTIONS_FILE) != {"schema": 1, "request": plan["request"]}:
            raise ValueError("sdk: installed isolated/staged shader/output identity mismatch")


def resolve(driver, args, target):
    """Read-only preflight; callers repeat under the existing kernel lease."""
    selection(args, target, "embedded")
    staging = getattr(args, "staging_dir", None)
    if (getattr(args, "build_dir", None) or staging) and target["host"] != driver.HOST:
        raise ValueError("sdk: isolated outputs require the matching native host")
    root = driver.ROOT.resolve()
    custom = bool(getattr(args, "build_dir", None))
    owner = driver._standalone_module()
    native = (owner.native_cache_layout(driver, args, target) if custom else
              owner.native_cache_plan(driver, args, target))
    default_build, conan = native["build"], native["conan"]
    args._native_cache_plan = native
    default_sdk = driver.sdk_install_dir(target, args.variant)
    default_sdk = default_sdk.with_name(default_sdk.name + "-single").resolve()
    build = Path(args.build_dir).resolve() if custom else default_build
    install = (_staging_prefix(root, staging) if staging else
               Path(args.install_dir).resolve() if custom else default_sdk)
    plan = {"source": root, "build": build, "install": install, "conan": conan,
            "toolchain": conan / "build/Release/generators/conan_toolchain.cmake",
            "variant": args.variant, "custom": custom,
            "shaders": "OFF" if getattr(args, "embed_engine_shaders", "auto") == "off" else "ON",
            "system": {"windows": "Windows", "linux": "Linux", "darwin": "Darwin"}[target["host"]],
            "arch": target["arch"]}
    if staging:
        plan["staging"] = True
        plan["request"] = _output_request(plan, target)
        _cache_identity(plan, shared=True)
        _installed_output_identity(driver, plan)
        return plan
    if not custom:
        return plan
    for path, parent in ((build, root / "build"), (install, root / "dist/sdk")):
        if path == parent or not path.is_relative_to(parent):
            raise ValueError("sdk: isolated output must be a strict child of " + str(parent))
        if path.exists() and not path.is_dir():
            raise ValueError("sdk: isolated output is not a directory: " + str(path))
    # All normal platform caches/prefixes are protected, not just this target.
    for folder in ("desktop", "web", "host", "host-conan", ".locks"):
        if _overlap(build, root / "build" / folder):
            raise ValueError("sdk: isolated cache overlaps shared build ownership")
    for candidate in driver.TARGETS.values():
        for variant in ("release", "dist"):
            normal = driver.sdk_install_dir(candidate, variant).resolve()
            for protected in (normal, normal.with_name(normal.name + "-debug"),
                              normal.with_name(normal.name + "-single")):
                if _overlap(install, protected):
                    raise ValueError("sdk: isolated prefix overlaps a default SDK")
    if _overlap(build, install):
        raise ValueError("sdk: build and install outputs overlap")
    request = _output_request(plan, target)
    plan["request"] = request
    stamp = build / REQUEST_FILE
    if stamp.exists():
        _request_stamp(stamp, request)
        _cache_identity(plan)
    elif build.exists() and any(build.iterdir()):
        raise ValueError("sdk: isolated cache is nonempty without its owner request stamp")
    _installed_output_identity(driver, plan)
    return plan


def smoke_directory(plan, scripting=False):
    return plan["build"] / "sdk-smoke" / ("scripting" if scripting else "c99") if plan["custom"] else None


def cache_paths(symbols, args, target):
    driver = SimpleNamespace(**symbols)
    plan = resolve(driver, args, target)
    result = [plan["build"], plan["conan"], plan["install"]]
    if getattr(args, "smoke", False):
        for scripting in (False, True):
            result += driver._smoke_cache_paths({"t": target, "sdk": plan["install"],
                "variant": args.variant, "build_dir": smoke_directory(plan, scripting)}, scripting)
    return result


def _json_bytes(data):
    encoded = (json.dumps(data, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8")
    if len(encoded) > 16384:
        raise ValueError("SDK output request exceeds 16384 bytes")
    return encoded


def _atomic_json(path, data):
    encoded = _json_bytes(data)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb",
                prefix=path.name + ".", suffix=".tmp", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary and temporary.exists():
            temporary.unlink()


def state(plan, selected):
    if selected not in _STATES:
        raise ValueError("invalid SDK producer state: " + selected)
    if plan["custom"]:
        _atomic_json(plan["build"] / REQUEST_FILE,
                     {"schema": 1, "request": plan["request"], "state": selected})


def verify_install(driver, plan):
    driver._sdk_target_module().require_sdk_target(plan["install"], plan["system"],
        plan["arch"], "Release", plan["variant"], "embedded")
    if plan["custom"] or plan.get("staging"):
        _atomic_json(plan["install"] / OPTIONS_FILE, {"schema": 1, "request": plan["request"]})
    state(plan, "installed")

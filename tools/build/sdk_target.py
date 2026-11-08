"""Read installed SDK compiler metadata before any consumer mutation.

This validates the producer's target declaration and selected archive presence.
It does not inspect archive ABI, execute a compiler or certify target linkage.
"""
from __future__ import annotations

import json
from pathlib import Path

_SYSTEMS = frozenset(("Windows", "Linux", "Darwin", "Emscripten", "Android", "iOS"))
_ARCH_BITS = {"x86_64": 64, "i686": 32, "aarch64": 64, "armv7": 32,
              "wasm32": 32, "wasm64": 64}
_CONFIGS = frozenset(("Debug", "Release"))
_MAX_METADATA_BYTES = 16384
_VARIANTS = frozenset(("debug", "release", "dist"))
_DELIVERIES = frozenset(("embedded", "authoring"))
_FIELDS = frozenset(("schema", "system", "arch", "pointer_bits", "config",
                     "variant", "delivery", "crt", "archive"))


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_json_constant(value):
    raise ValueError(f"non-finite JSON value: {value}")


def _choice(field, value, allowed):
    if type(value) is not str or value not in allowed:
        raise ValueError(f"invalid {field}: {value!r}; expected {sorted(allowed)}")


def _selection(system, arch, config, variant, delivery):
    for field, value, choices in (("system", system, _SYSTEMS),
                                   ("arch", arch, _ARCH_BITS),
                                   ("config", config, _CONFIGS),
                                   ("variant", variant, _VARIANTS),
                                   ("delivery", delivery, _DELIVERIES)):
        _choice(field, value, choices)
    required_config = "Debug" if variant == "debug" else "Release"
    if config != required_config:
        raise ValueError(f"variant {variant!r} requires config {required_config!r}, "
                         f"found {config!r}")


def require_sdk_target(sdk: str | Path, system: str, arch: str, config: str,
                       variant: str, delivery: str) -> dict:
    """Return valid schema-1 metadata or raise ValueError without changing files.

    Inputs are canonical compiler/CLI target values, not host aliases. Only the
    exact lib/<config> metadata and archive are read; no VERSION stamp, sibling
    configuration or directory-name inference can supply a missing target.
    """
    _selection(system, arch, config, variant, delivery)
    try:
        path = Path(sdk) / "lib" / config / "jce-sdk-target.json"
    except (TypeError, ValueError) as exc:
        raise ValueError(f"invalid SDK root: {sdk!r}") from exc
    try:
        # One bounded read also rejects a file that grows after a size check.
        with path.open("rb") as stream:
            encoded = stream.read(_MAX_METADATA_BYTES + 1)
        if len(encoded) > _MAX_METADATA_BYTES:
            raise ValueError(f"SDK target metadata exceeds {_MAX_METADATA_BYTES} bytes")
        # This is generator-owned flat metadata, whose allowed strings and
        # keys are ASCII scalars. Decode aliases/embedded NUL are forbidden.
        if b"\\" in encoded:
            raise ValueError("SDK target metadata requires unescaped ASCII scalars")
        data = json.loads(encoded.decode("utf-8"),
                          object_pairs_hook=_unique_object,
                          parse_constant=_reject_json_constant)
    except (OSError, UnicodeError, ValueError) as exc:
        raise ValueError(f"SDK target metadata unreadable at {path}: {exc}") from exc
    if type(data) is not dict:
        raise ValueError(f"SDK target metadata at {path} must be a JSON object")
    missing = _FIELDS - data.keys()
    if missing:
        raise ValueError(f"SDK target metadata at {path} lacks {sorted(missing)}")
    unknown = data.keys() - _FIELDS
    if unknown:
        raise ValueError(f"SDK target metadata at {path} has unknown fields {sorted(unknown)}")
    if type(data["schema"]) is not int or data["schema"] != 1:
        raise ValueError(f"unsupported SDK target schema at {path}: {data['schema']!r}")
    _selection(data["system"], data["arch"], data["config"], data["variant"],
               data["delivery"])
    if (type(data["pointer_bits"]) is not int or
            data["pointer_bits"] != _ARCH_BITS[data["arch"]]):
        raise ValueError(f"SDK pointer_bits does not match arch {data['arch']!r}")
    for field, expected in (("system", system), ("arch", arch), ("config", config),
                            ("variant", variant), ("delivery", delivery)):
        if data[field] != expected:
            raise ValueError(f"SDK target {field} mismatch at {path}: "
                             f"expected {expected!r}, found {data[field]!r}")
    if system == "Windows":
        allowed_crt = {"MultiThreadedDebug", "MultiThreadedDebugDLL"} if config == "Debug" else {
            "MultiThreaded", "MultiThreadedDLL"}
        _choice("crt", data["crt"], allowed_crt)
        if delivery == "embedded" and data["crt"].endswith("DLL"):
            raise ValueError("embedded Windows SDK requires static CRT")
        archive_name = "jce_engine_core.lib"
    else:
        _choice("crt", data["crt"], {"none"})
        archive_name = "jce_engine_core.a"
    if type(data["archive"]) is not str or data["archive"] != archive_name:
        raise ValueError(f"SDK archive must be the basename {archive_name!r}")
    archive = path.parent / archive_name
    if not archive.is_file():
        raise ValueError(f"SDK archive missing or not a file: {archive}")
    return data

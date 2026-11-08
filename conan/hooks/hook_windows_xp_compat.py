"""Pure mimalloc 3.3.2 XP build configuration; never inspect dependency files."""
import shlex
import json
import hashlib

_XP = "user.jce:windows_xp"
_HEADER = "user.jce:mimalloc_xp_header"
_HEADERS_SHA = "user.jce:mimalloc_xp_headers_sha256"
_CONFIG = "user.jce:mimalloc_xp_config"
_INPUTS = "user.jce:mimalloc_xp_inputs"
_REVISION = "exclusive-srw-rtlgenrandom-crt-tls-v1"
_VARIABLES = "tools.cmake.cmaketoolchain:extra_variables"
_GUARDS = ("JCE_WINDOWS_XP_SYNC_PORT", "JCE_WINDOWS_XP_MIMALLOC_PORT")
_ON = _GUARDS + ("MI_USE_RTLGENRANDOM", "MI_WIN_INIT_USE_CRT_TLS")
_FORBIDDEN = ("MI_WIN_INIT_USE_FLS", "MI_WIN_INIT_USE_RAW_DLLMAIN", "MI_WIN_INIT_USE_TLS_DLLMAIN")
_OFF_OPTIONS = ("MI_WIN_USE_FLS", "MI_WIN_INIT_USE_RAW_DLLMAIN", "MI_WIN_INIT_USE_TLS_DLLMAIN", "MI_WIN_DIRECT_TLS")
_RESERVED = _ON + _FORBIDDEN + ("MI_WIN_USE_FLS", "MI_WIN_DIRECT_TLS")


def _error(message):
    raise ValueError("mimalloc XP configuration: " + message)


def _header_path(value):
    if not isinstance(value, str) or not value or len(value) > 4096:
        _error("header must be an absolute compiler path")
    if any(c in value for c in "\x00\r\n\"';$<>{}[]()`"):
        _error("header contains quoting or CMake metacharacters")
    if value.startswith("\\") and not value.startswith("\\\\"):
        _error("header cannot be a Windows drive-relative path")
    path = value.replace("\\", "/")
    drive = len(path) >= 3 and path[0].isalpha() and path[1:3] == ":/"
    if not drive and not path.startswith("/"):
        _error("header must be absolute")
    parts = path.split("/")
    if any(part in (".", "..") for part in parts) or not parts[-1]:
        _error("header path must not contain aliases or end in a directory")
    if path.startswith("//") and (len(parts) < 5 or not parts[2] or not parts[3]):
        _error("header UNC path requires a server, share and file")
    return path


def _cmake_value(value):
    if isinstance(value, dict):
        if "value" not in value or any(key not in ("value", "type", "cache", "force") for key in value):
            _error("invalid typed CMake value")
        for key in ("cache", "force"):
            if key in value and type(value[key]) is not bool:
                _error("typed CMake cache/force must be boolean")
        if "type" in value and (not isinstance(value["type"], str) or len(value["type"]) > 32):
            _error("typed CMake type must be a bounded string")
        return value["value"]
    return value


def _false(value):
    return (type(value) is bool and not value or type(value) is int and value == 0
            or isinstance(value, str) and value.upper() in ("0", "OFF", "FALSE", "NO", "N", "IGNORE", "NOTFOUND", ""))


def _macro(name, action, value):
    if name not in _RESERVED:
        return
    if action != "D" or name in _FORBIDDEN or name == "MI_WIN_USE_FLS":
        _error("conflicting define/undefine " + name)
    expected = "0" if name == "MI_WIN_DIRECT_TLS" else "1"
    if value != expected:
        _error("conflicting numeric definition " + name)


def _tokens(flags):
    if not isinstance(flags, list) or len(flags) > 256:
        _error("compiler flags require a bounded list")
    if any(not isinstance(flag, str) or len(flag) > 8192 for flag in flags):
        _error("compiler flags must be bounded strings")
    text = " ".join(flags)
    if len(text) > 32768 or any(c in text for c in "\x00\r\n;") or "$<" in text:
        _error("compiler flags contain unresolved list or generator syntax")
    return shlex.split(" ".join(flags), posix=False)


def _include_spellings(header):
    # Conan embeds flags and extra-variable values verbatim inside CMake
    # quoted strings. Keep one CMake escape level until that file is read.
    plain = '-include "' + header + '"'
    escaped = r'-include \"' + header + r'\"'
    return plain, escaped


def _flag_tokens(flags, header, digest):
    _tokens(flags)  # Check the raw list/type/budget before decoding our operand.
    plain, escaped = _include_spellings(header)
    text = " ".join(flags)
    occurrences = text.count(plain) + text.count(escaped)
    logical = [flag.replace(escaped, plain) for flag in flags]
    tokens = _tokens(logical)
    canonical = []
    guards = set()
    included = 0
    index = 0
    while index < len(tokens):
        token = tokens[index].strip("\"'")
        if token.startswith("@") or token in ("-Xclang", "-Xpreprocessor") or token.startswith("-Wp,"):
            _error("opaque preprocessor flags cannot establish the XP policy")
        if token in ("-include", "/FI") or token.startswith(("-include", "/FI")):
            if token in ("-include", "/FI"):
                index += 1
                if index >= len(tokens):
                    _error("forced include has no operand")
                operand = tokens[index].strip("\"'")
            else:
                operand = token[len("-include"):] if token.startswith("-include") else token[3:]
                operand = operand.lstrip("=").strip("\"'")
            if _header_path(operand) != header:
                _error("competing forced include is outside the verified header cohort")
            canonical.extend(("-include", "<verified-mimalloc-headers:" + digest + ">"))
            included += 1
        elif token[:2] in ("-D", "/D", "-U", "/U"):
            action = token[1]
            operand = token[2:].strip("\"'")
            if not operand:
                index += 1
                if index >= len(tokens):
                    _error("define/undefine has no operand")
                operand = tokens[index].strip("\"'")
            name, equals, value = operand.partition("=")
            _macro(name, action, value if equals else None)
            if name in _GUARDS:
                guards.add(name)
            canonical.append(token[:2] + operand)
        else:
            canonical.append(token)
        index += 1
    if occurrences != included:
        _error("ambiguous forced include spelling; use a literal quoted verified operand")
    return canonical, guards, included


def _port_flags(flags, header, digest):
    unused, guards, included = _flag_tokens(flags, header, digest)
    plain, escaped = _include_spellings(header)
    result = []
    for index, flag in enumerate(flags):
        normalized = flag.replace(plain, escaped)
        if index and flags[index - 1].strip() == "-include" and flag == '"' + header + '"':
            normalized = r'\"' + header + r'\"'
        result.append(normalized)
    if included and " ".join(result).count(escaped) != included:
        _error("ambiguous forced include cannot be escaped without changing caller flags")
    for name in _GUARDS:
        if name not in guards:
            result.append("-D" + name + "=1")
    if not included:
        result.append(escaped)
    return result


def _extra_definitions(value):
    if not isinstance(value, str) or len(value) > 16384 or any(c in value for c in "\x00\r\n") or "$<" in value:
        _error("MI_EXTRA_CPPDEFS requires a bounded literal semicolon string")
    found = False
    for item in value.split(";"):
        item = item.strip().strip("\"'")
        if not item:
            continue
        if item.startswith(("-U", "/U")):
            _macro(item[2:].strip(), "U", None)
        else:
            if item.startswith(("-D", "/D")):
                item = item[2:].strip().strip("\"'")
            name, equals, actual = item.partition("=")
            _macro(name.strip(), "D", actual.strip() if equals else None)
            found = found or name.strip() == "MI_USE_RTLGENRANDOM"
    if found:
        return value
    return value + ("" if not value or value.endswith(";") else ";") + "MI_USE_RTLGENRANDOM=1"


def _configuration(conanfile, settings):
    if conanfile.name != "mimalloc":
        return None
    xp = conanfile.conf.get(_XP, default=False)
    if type(xp) is not bool:
        _error("user.jce:windows_xp must be boolean")
    if not xp:
        return None
    if str(conanfile.version) != "3.3.2":
        _error("only the reviewed mimalloc/3.3.2 version is admitted")
    if (str(settings.get_safe("os")) != "Windows" or str(settings.get_safe("compiler")) != "clang"
            or str(settings.get_safe("arch")) not in ("x86", "x86_64")
            or settings.get_safe("compiler.libcxx") not in (None, "libc++")
            or settings.get_safe("compiler.frontend") not in (None, "GNU", "gnu")
            or any(settings.get_safe(key) is not None for key in
                   ("compiler.runtime", "compiler.runtime_type", "compiler.runtime_version"))):
        _error("requires Windows GNU Clang/libc++ x86 or x86_64 without MSVC runtime settings")
    header = _header_path(conanfile.conf.get(_HEADER))
    digest = conanfile.conf.get(_HEADERS_SHA)
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        _error("verified combined header SHA256 must be lowercase hexadecimal")
    if conanfile.conf.get(_CONFIG, default=_REVISION) != _REVISION:
        _error("port policy revision conflicts")
    variables = conanfile.conf.get(_VARIABLES, default={}, check_type=dict)
    if not isinstance(variables, dict) or len(variables) > 256 or any(not isinstance(k, str) for k in variables):
        _error("CMake variables require a bounded string-keyed dictionary")
    variables = dict(variables)
    system = _cmake_value(variables.get("CMAKE_SYSTEM_NAME",
        conanfile.conf.get("tools.cmake.cmaketoolchain:system_name", default="Windows")))
    if system != "Windows":
        _error("requires the desktop Windows CMake system")
    canonical_variables = {}
    for name, entry in variables.items():
        value = _cmake_value(entry)
        if name.startswith(("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS")):
            if not isinstance(value, str):
                _error("CMake compiler flags must be strings")
            _flag_tokens([value], header, digest)
            # These base variables replace Conan's FLAGS_INIT, so retain the
            # verified forced include even when the caller presets them.
            if name in ("CMAKE_C_FLAGS", "CMAKE_C_FLAGS_INIT", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_INIT"):
                value = " ".join(_port_flags([value], header, digest))
                if isinstance(entry, dict):
                    entry = dict(entry)
                    entry["value"] = value
                else:
                    entry = value
                variables[name] = entry
            canonical_variables[name] = {"argv": _flag_tokens([value], header, digest)[0],
                "metadata": {k: v for k, v in entry.items() if k != "value"} if isinstance(entry, dict) else {}}
        elif name in _OFF_OPTIONS:
            if not _false(value):
                _error("conflicting CMake option " + name)
        elif name in _ON:
            if type(value) not in (str, int) or str(value) != "1":
                _error("conflicting CMake macro " + name)
        elif name in _FORBIDDEN:
            _error("unsupported custom initialization mode " + name)
        if name.startswith("MI_") and name != "MI_EXTRA_CPPDEFS":
            if type(value) not in (str, bool, int) or isinstance(value, str) and len(value) > 16384:
                _error("MI CMake variables require bounded scalar values")
            canonical_variables[name] = entry
    extra = variables.get("MI_EXTRA_CPPDEFS", "")
    if isinstance(extra, dict) and extra.get("type", "STRING") != "STRING":
        _error("MI_EXTRA_CPPDEFS typed value must use STRING")
    merged = _extra_definitions(_cmake_value(extra))
    if isinstance(extra, dict):
        extra = dict(extra)
        extra["value"] = merged
    else:
        extra = merged
    variables["MI_EXTRA_CPPDEFS"] = extra
    canonical_variables["MI_EXTRA_CPPDEFS"] = extra
    for name in _OFF_OPTIONS:
        if name not in variables:
            variables[name] = False
        canonical_variables[name] = variables[name]
    channels = {}
    canonical_flags = {}
    for key in ("tools.build:cflags", "tools.build:cxxflags"):
        flags = _port_flags(conanfile.conf.get(key, default=[], check_type=list), header, digest)
        channels[key] = flags
        canonical_flags[key] = _flag_tokens(flags, header, digest)[0]
    defines = conanfile.conf.get("tools.build:defines", default=[], check_type=list)
    if not isinstance(defines, list) or len(defines) > 256:
        _error("directory definitions require a bounded list")
    defines = list(defines)
    for definition in defines:
        if (not isinstance(definition, str) or len(definition) > 8192
                or any(c in definition for c in "\x00\r\n;") or "$<" in definition):
            _error("directory definitions require bounded literal strings")
        operand = definition.strip("\"'")
        action = "D"
        if operand.startswith(("-D", "/D", "-U", "/U")):
            action = operand[1]
            operand = operand[2:].strip().strip("\"'")
        name, equals, value = operand.partition("=")
        _macro(name.strip(), action, value if equals else None)
    for name in _GUARDS:
        if name + "=1" not in defines:
            defines.append(name + "=1")
    channels["tools.build:defines"] = defines
    identity = {"policy": _REVISION, "headers_sha256": digest,
        "settings": {key: settings.get_safe(key) for key in ("os", "arch", "compiler", "compiler.version",
            "compiler.cppstd", "compiler.libcxx", "compiler.frontend", "compiler.runtime", "compiler.runtime_type", "compiler.runtime_version")},
        "flags": canonical_flags, "directory_defines": defines, "cmake": canonical_variables}
    canonical = json.dumps(identity, sort_keys=True, ensure_ascii=True, allow_nan=False, separators=(",", ":"))
    if len(canonical) > 65536:
        _error("canonical policy exceeds its bounded budget")
    fingerprint = hashlib.sha256(canonical.encode("ascii")).hexdigest()
    if conanfile.conf.get(_INPUTS, default=fingerprint) != fingerprint:
        _error("canonical configuration digest conflicts")
    return channels, variables, digest, fingerprint


def post_package_id(conanfile):
    selected = _configuration(conanfile, conanfile.info.settings)
    if selected is None:
        return
    values = {_CONFIG: _REVISION, _HEADERS_SHA: selected[2], _INPUTS: selected[3]}
    for key, value in values.items():
        if conanfile.info.conf.get(key, default=value) != value:
            _error("package identity conflicts with " + key)
    for key, value in values.items():
        conanfile.info.conf.define(key, value)


def pre_generate(conanfile):
    selected = _configuration(conanfile, conanfile.settings)
    if selected is None:
        return
    channels, variables, digest, fingerprint = selected
    for key, value in channels.items():
        conanfile.conf.define(key, value)
    conanfile.conf.define(_VARIABLES, variables)

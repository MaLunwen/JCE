"""Pure configuration for reviewed XP codec, stdio and bimg consumers."""
import shlex
import json
import hashlib

_XP = "user.jce:windows_xp"
_CONFIG = "user.jce:xp_consumers_config"
_INPUTS = "user.jce:xp_consumers_inputs"
_REVISION = "codec-stdio-consumers-v1"
_VARIABLES = "tools.cmake.cmaketoolchain:extra_variables"
_BGFX_ISA_INCLUDE = "CMAKE_PROJECT_bgfx_INCLUDE"
_BGFX_ISA = "sse2-bx-reference-v1"
_TOOLCHAIN = "user.jce:xp_consumer_toolchain"
_USER_TOOLCHAIN = "tools.cmake.cmaketoolchain:user_toolchain"
_REVIEWED = {"dav1d": ("1.5.3", "codec", "meson"),
             "libaom-av1": ("3.13.3", "codec", "cmake"),
             "flecs": ("4.1.5", "stdio", "cmake"),
             "miniz": ("3.0.2", "stdio", "cmake"),
             "kuba-zip": ("0.3.0", "stdio", "cmake"),
             "bgfx": ("1.146.9306-550", "bimg", "cmake")}
_GUARDS = ("JCE_WINDOWS_XP_SYNC_PORT", "JCE_WINDOWS_XP_CODEC_PORT", "JCE_WINDOWS_XP_STDIO_PORT",
           "JCE_WINDOWS_XP_BX_MEMORY_PORT")
_SETTINGS = ("os", "arch", "compiler", "compiler.version", "compiler.cppstd", "compiler.libcxx",
             "compiler.frontend", "compiler.runtime", "compiler.runtime_type", "compiler.runtime_version")


def _error(message):
    raise ValueError("XP consumers configuration: " + message)


def _path(value):
    if not isinstance(value, str) or not value or len(value) > 4096:
        _error("requires a bounded absolute compiler path")
    if any(ord(c) < 32 or c in "\"';$<>{}[]()`*?|" for c in value):
        _error("path contains quoting or build-language metacharacters")
    if value.startswith("\\") and not value.startswith("\\\\"):
        _error("path cannot be Windows drive-relative")
    path = value.replace("\\", "/")
    drive = len(path) >= 3 and path[0] in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz" and path[1:3] == ":/"
    if not drive and not path.startswith("/"):
        _error("compiler path must be absolute")
    parts = path.split("/")
    if any(part in (".", "..") for part in parts) or not parts[-1]:
        _error("path contains an alias or ends in a directory")
    if path.startswith("//") and (len(parts) < 5 or not parts[2] or not parts[3]):
        _error("UNC path requires server, share and file")
    names = parts[2:] if path.startswith("//") else parts[1:]
    if any(not part or ":" in part or part.endswith((" ", ".")) for part in names):
        _error("path contains ambiguous Windows aliases")
    return path


def _cmake_value(entry):
    value = entry
    if isinstance(entry, dict):
        if "value" not in entry or any(key not in ("value", "type", "cache", "force") for key in entry):
            _error("invalid typed CMake variable")
        if any(key in entry and type(entry[key]) is not bool for key in ("cache", "force")):
            _error("typed CMake cache/force must be boolean")
        if "type" in entry and entry["type"] not in ("BOOL", "FILEPATH", "PATH", "STRING", "INTERNAL"):
            _error("typed CMake type must be a supported cache type")
        value = entry["value"]
    if type(value) not in (str, bool, int) or isinstance(value, str) and (
            len(value) > 16384 or any(ord(c) < 32 or c in '"$' for c in value)):
        _error("CMake values require bounded literal scalars")
    return value


def _macro(name, action, value, guards):
    reserved = name.split("(", 1)[0]
    if reserved not in _GUARDS and reserved != "MINIZ_NO_STDIO":
        return
    if name != reserved or name not in guards or action != "D" or value != "1":
        _error("conflicting reserved define/undefine " + name)


def _spellings(header):
    return '-include "' + header + '"', r'-include \"' + header + r'\"'


def _scan(flags, header, mode, guards, *, sse2_only=False):
    if not isinstance(flags, list) or len(flags) > 256 or any(
            not isinstance(flag, str) or len(flag) > 8192 for flag in flags):
        _error("compiler flags require a bounded string list")
    text = " ".join(flags)
    if len(text) > 32768 or any(c in text for c in "\x00\r\n;$"):
        _error("compiler flags contain unresolved build-language syntax")
    logical = list(flags)
    expected_includes = 0
    if header:
        plain, escaped = _spellings(header)
        if mode == "cmake":
            expected_includes = text.count(plain) + text.count(escaped)
            logical = [flag.replace(escaped, plain) for flag in flags]
        else:
            for i in range(1, len(logical)):
                if logical[i - 1] == "-include" and _path(logical[i]) == header:
                    expected_includes += 1
                    logical[i] = '"' + header + '"'
    tokens = shlex.split(" ".join(logical), posix=False)
    found = set()
    included = 0
    i = 0
    while i < len(tokens):
        token = tokens[i].strip("\"'")
        if token.startswith(("-stdlib", "--stdlib")) and token != "-stdlib=libc++":
            _error("requires the selected GNU libc++ standard library")
        if sse2_only and token != "-msse2" and token.startswith((
                "-march", "-mcpu", "-msse", "-mssse", "-mavx", "-mpopcnt", "-mfma",
                "-mbmi", "-mlzcnt", "-maes", "-mpclmul", "-mf16c")):
            _error("the XP BGFX provider requires the SSE2 target")
        if (token.startswith(("@", "-Wp,", "/clang:", "--config", "--target", "--sysroot", "-imacros"))
                or token in ("-Xclang", "-Xpreprocessor", "-imacros", "-target", "-isysroot")
                or token.startswith(("-fms-runtime-lib=", "-rtlib="))):
            _error("opaque preprocessor or target overrides are unsupported")
        if token.startswith(("-include", "/FI")):
            if token in ("-include", "/FI"):
                i += 1
                if i >= len(tokens):
                    _error("forced include has no operand")
                operand = tokens[i].strip("\"'")
            else:
                operand = token[len("-include"):] if token.startswith("-include") else token[3:]
                operand = operand.lstrip("=").strip("\"'")
            if not header or _path(operand) != header:
                _error("forced include is outside the verified header cohort")
            included += 1
        elif token[:2] in ("-D", "/D", "-U", "/U"):
            action = token[1]
            operand = token[2:].strip("\"'")
            if not operand:
                i += 1
                if i >= len(tokens):
                    _error("define/undefine has no operand")
                operand = tokens[i].strip("\"'")
            name, equals, value = operand.partition("=")
            _macro(name, action, value if equals else None, guards)
            if name in guards:
                found.add(name)
        i += 1
    if included != expected_includes:
        _error("ambiguous forced include spelling for the selected generator")
    return found, included


def _flags(flags, header, digest, mode, guards, *, sse2_only=False):
    unused, included = _scan(flags, header, mode, guards, sse2_only=sse2_only)
    result = list(flags)
    if header and mode == "cmake":
        plain, escaped = _spellings(header)
        result = [flag.replace(plain, escaped) for flag in result]
    if header and mode == "meson":
        for i in range(1, len(result)):
            if result[i - 1] == "-include":
                result[i] = header
    for name in guards:
        if "-D" + name + "=1" not in result:
            result.append("-D" + name + "=1")
    if header and not included:
        if mode == "meson":
            # Meson serializes each list item as one compiler argv element.
            result.extend(("-include", header))
        else:
            # Keep one CMake escape level around the quoted compiler operand.
            result.append(_spellings(header)[1])
    canonical = list(result)
    if header:
        marker = "<verified-xp-headers:" + digest + ">"
        if mode == "cmake":
            canonical = [flag.replace(_spellings(header)[1], '-include "' + marker + '"') for flag in canonical]
        else:
            for i in range(1, len(canonical)):
                if canonical[i - 1] == "-include":
                    canonical[i] = marker
    return result, canonical


def _variables(conanfile, guards, bgfx_isa=None):
    variables = conanfile.conf.get(_VARIABLES, default={}, check_type=dict)
    allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_"
    if len(variables) > 256 or any(not isinstance(key, str) or not key or len(key) > 256
            or any(c not in allowed for c in key) for key in variables):
        _error("CMake variables require bounded literal identifier keys")
    for name, entry in variables.items():
        value = _cmake_value(entry)
        if name == _BGFX_ISA_INCLUDE and bgfx_isa is not None and entry == bgfx_isa:
            continue
        if name.startswith(("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS")):
            _error("replacement CMake compiler flags could bypass verified configuration")
        if (name.startswith(("CMAKE_PROJECT_", "CMAKE_USER_MAKE_RULES_OVERRIDE", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER",
                                    "CMAKE_C_COMPILE", "CMAKE_CXX_COMPILE", "CMAKE_C_PREPROCESS", "CMAKE_CXX_PREPROCESS"))
                or name in ("CMAKE_TOOLCHAIN_FILE", "CMAKE_TOP_LEVEL_INCLUDES")):
            _error("opaque CMake compiler or include override")
        if name == "CMAKE_SYSTEM_NAME" and value != "Windows":
            _error("requires the desktop Windows CMake system")
        _macro(name, "D", str(value), guards)
    system = conanfile.conf.get("tools.cmake.cmaketoolchain:system_name", default="Windows")
    if system != "Windows":
        _error("requires the desktop Windows CMake system")
    return variables




def _bx_separate_variables(variables):
    if "BX_AMALGAMATED" in variables:
        entry = variables["BX_AMALGAMATED"]
        value = _cmake_value(entry)
        false = (type(value) is bool and not value or type(value) is int and value == 0
                 or isinstance(value, str) and value.upper() in ("0", "OFF", "FALSE", "NO", "N"))
        if not false:
            _error("reviewed x86 BX requires BX_AMALGAMATED=False")
        if isinstance(entry, dict) and (entry.get("type", "BOOL") != "BOOL"
                or any(key in entry and entry[key] is not True for key in ("cache", "force"))):
            _error("BX_AMALGAMATED requires a forced BOOL cache entry")
    result = dict(variables)
    # The recipe also passes ON on the CMake command line; ordinary set() is
    # insufficient. Use Conan's supported typed forced cache configuration.
    result["BX_AMALGAMATED"] = {"value": False, "type": "BOOL", "cache": True, "force": True}
    return result



def _meson_resource(conanfile, variables):
    root = _path(_cmake_value(variables.get("JCE_XP_TOOLCHAIN_ROOT")))
    compilers = conanfile.conf.get("tools.build:compiler_executables", default={}, check_type=dict)
    if len(compilers) > 32:
        _error("compiler selection exceeds its bounded budget")
    c = _path(compilers.get("c"))
    windres = _path(compilers.get("rc"))
    prefix = root + "/bin/i686-w64-mingw32-"
    if c != prefix + "clang.exe" or windres != prefix + "windres.exe" or any(ch in c + windres for ch in "%!"):
        _error("x86 Meson resource compiler must match the selected target C compiler and toolchain")
    definitions = conanfile.buildenv.dumps()
    if not isinstance(definitions, str) or len(definitions) > 65536:
        _error("build environment exceeds its bounded budget")
    found = False
    for line in definitions.splitlines():
        name, equals, value = line.partition("=")
        if name.rstrip("+").upper() != "WINDRES":
            continue
        if name != "WINDRES" or not equals or found:
            _error("WINDRES requires one literal selected resource compiler")
        found = True
        if value.startswith("(path)"):
            value = value[6:]
        if _path(value) != windres:
            _error("WINDRES conflicts with the selected resource compiler")
    return {"c": c, "windres": windres}



def _cohort(conanfile, kind):
    header = _path(conanfile.conf.get("user.jce:xp_" + kind + "_header"))
    sha_key = "user.jce:xp_" + kind + "_headers_sha256"
    digest = conanfile.conf.get(sha_key)
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        _error("verified combined header SHA256 must be lowercase hexadecimal")
    return header, sha_key, digest


def _configuration(conanfile, settings):
    if conanfile.name not in _REVIEWED:
        return None
    xp = conanfile.conf.get(_XP, default=False)
    if type(xp) is not bool:
        _error("user.jce:windows_xp must be boolean")
    if not xp:
        return None
    version, kind, mode = _REVIEWED[conanfile.name]
    if str(conanfile.version) != version:
        _error("only the reviewed version of " + conanfile.name + " is admitted")
    if (str(settings.get_safe("os")) != "Windows" or str(settings.get_safe("compiler")) != "clang"
            or str(settings.get_safe("compiler.version")) != "22"
            or str(settings.get_safe("arch")) not in ("x86", "x86_64")
            or settings.get_safe("compiler.libcxx") not in (None, "libc++")
            or settings.get_safe("compiler.frontend") not in (None, "GNU", "gnu")
            or any(settings.get_safe(key) is not None for key in
                   ("compiler.runtime", "compiler.runtime_type", "compiler.runtime_version"))):
        _error("requires Windows GNU Clang 22/libc++ x86 or x86_64 without MSVC runtime settings")
    toolchain = conanfile.conf.get(_TOOLCHAIN)
    _path(toolchain)
    if conanfile.conf.get(_USER_TOOLCHAIN, default=[]) != [toolchain]:
        _error("requires exactly the explicitly admitted XP consumer toolchain")
    for key in ("tools.cmake.cmaketoolchain:toolchain_file", "tools.cmake.cmaketoolchain:enabled_blocks",
                "tools.meson.mesontoolchain:extra_machine_files"):
        if conanfile.conf.get(key) is not None:
            _error("opaque toolchain replacement: " + key)
    # Keep bundled codec aliases in C and BX's optional memory query in C++.
    cohort = "codec" if kind == "bimg" else kind
    header, sha_key, digest = _cohort(conanfile, cohort)
    sha_values = {sha_key: digest}
    c_guards = _GUARDS[:2] if cohort == "codec" else (_GUARDS[2],)
    cpp_guards = c_guards
    cpp_header, cpp_digest = header, digest
    guards = c_guards
    if kind == "bimg":
        cpp_header, cpp_sha_key, cpp_digest = _cohort(conanfile, "bx_memory")
        sha_values[cpp_sha_key] = cpp_digest
        c_guards += ("MINIZ_NO_STDIO",)
        cpp_guards = (_GUARDS[3], "MINIZ_NO_STDIO")
        guards = c_guards + (_GUARDS[3],)
    bgfx_isa = None
    if kind == "bimg":
        path = cpp_header.rsplit("/", 1)[0] + "/windows-xp-bgfx-isa.cmake"
        bgfx_isa = {"value": path, "type": "FILEPATH", "cache": True, "force": True}
    variables = _variables(conanfile, guards, bgfx_isa)
    resource = (_meson_resource(conanfile, variables)
                if conanfile.name == "dav1d" and str(settings.get_safe("arch")) == "x86" else None)
    separate_bx = kind == "bimg" and str(settings.get_safe("arch")) == "x86"
    if separate_bx:
        variables = _bx_separate_variables(variables)
    if bgfx_isa is not None:
        variables = {**variables, _BGFX_ISA_INCLUDE: bgfx_isa}
    channels = {}
    canonical_flags = {}
    for key in ("tools.build:cflags", "tools.build:cxxflags"):
        cpp = key == "tools.build:cxxflags"
        channels[key], canonical_flags[key] = _flags(
            conanfile.conf.get(key, default=[], check_type=list), cpp_header if cpp else header,
            cpp_digest if cpp else digest, mode, cpp_guards if cpp else c_guards,
            sse2_only=bgfx_isa is not None)
    definitions = conanfile.conf.get("tools.build:defines", default=[], check_type=list)
    if not isinstance(definitions, list) or len(definitions) > 256:
        _error("directory definitions require a bounded list")
    found = set()
    for definition in definitions:
        if (not isinstance(definition, str) or len(definition) > 8192
                or any(c in definition for c in "\x00\r\n;$")):
            _error("directory definitions require bounded literal strings")
        operand = definition.strip("\"'")
        action = "D"
        if operand.startswith(("-D", "/D", "-U", "/U")):
            action = operand[1]
            operand = operand[2:].strip().strip("\"'")
        name, equals, value = operand.partition("=")
        _macro(name, action, value if equals else None, guards)
        if name in guards:
            found.add(name)
    channels["tools.build:defines"] = list(definitions) + [name + "=1" for name in guards if name not in found]
    canonical_variables = variables
    if bgfx_isa is not None:
        channels[_VARIABLES] = variables
        canonical_variables = {**variables, _BGFX_ISA_INCLUDE: {
            **bgfx_isa, "value": "<verified-xp-bgfx-cohort:" + cpp_digest + ">"}}
    identity = {"policy": _REVISION, "name": conanfile.name, "version": version, "cohort": kind,
                "generator": mode, "headers_sha256": digest, "toolchain_request": [toolchain],
                "settings": {key: settings.get_safe(key) for key in _SETTINGS},
                "flags": canonical_flags, "directory_defines": channels["tools.build:defines"],
                "cmake": canonical_variables}
    if kind == "bimg":
        identity["bx_memory_headers_sha256"] = cpp_digest
        identity["cpu_isa"] = _BGFX_ISA
    if resource is not None:
        identity["meson_resource"] = resource
    canonical = json.dumps(identity, sort_keys=True, ensure_ascii=True, allow_nan=False, separators=(",", ":"))
    if len(canonical) > 65536:
        _error("canonical configuration exceeds its bounded budget")
    fingerprint = hashlib.sha256(canonical.encode("ascii")).hexdigest()
    if conanfile.conf.get(_CONFIG, default=_REVISION) != _REVISION or conanfile.conf.get(_INPUTS, default=fingerprint) != fingerprint:
        _error("canonical policy identity conflicts")
    return channels, sha_values, digest, fingerprint, resource


def post_package_id(conanfile):
    selected = _configuration(conanfile, conanfile.info.settings)
    if selected is None:
        return
    values = {_CONFIG: _REVISION, _INPUTS: selected[3]}
    values.update(selected[1])
    for key, value in values.items():
        if conanfile.info.conf.get(key, default=value) != value:
            _error("package identity conflicts with " + key)
    for key, value in values.items():
        conanfile.info.conf.define(key, value)


def pre_generate(conanfile):
    selected = _configuration(conanfile, conanfile.settings)
    if selected is None:
        return
    for key, value in selected[0].items():
        conanfile.conf.define(key, value)
    if selected[4] is not None:
        # Conan's MesonToolchain consumes WINDRES, whereas CMake consumes rc.
        conanfile.buildenv.define_path("WINDRES", selected[4]["windres"])

"""Select bgfx build configuration without changing any upstream file.

This hook changes Conan configuration and package metadata only. Previous JCE
source patches are retired; existing Conan sources/binaries are not repaired.
A changed binary identity prevents a previously patched bgfx package being
silently reused by a new package resolution.
"""
import shlex
import json
import hashlib

_GRAPHICS_TIER_CONF = "user.jce:bgfx_graphics_tier"
_CPU_PROFILER_CONF = "user.jce:bgfx_cpu_profiler"
_CPU_PROFILER_CONFIG_CONF = "user.jce:bgfx_cpu_profiler_config"
_CPU_PROFILER_CONFIG = "directory-defines-v1"
_ANNOTATION_CONF = "user.jce:bgfx_debug_annotation"
_ANNOTATION_OPTION_CONF = "user.jce:bgfx_debug_annotation_option"
_ANNOTATION_COMPILED_CONF = "user.jce:bgfx_debug_annotation_compiled"
_ANNOTATION_CONFIG_CONF = "user.jce:bgfx_debug_annotation_config"
_ANNOTATION_INPUTS_CONF = "user.jce:bgfx_debug_annotation_inputs"
_ANNOTATION_CONFIG = "upstream-cmake-v1"
_BX_DEBUG_CONF = "user.jce:bgfx_bx_debug"
_BX_DEBUG_CONSUMER_CONF = "user.jce:bgfx_bx_debug_consumer"
_BX_DEBUG_OPTION_CONF = "user.jce:bgfx_bx_debug_option"
_BX_DEBUG_COMPILED_CONF = "user.jce:bgfx_bx_debug_compiled"
_BX_DEBUG_CONFIG_CONF = "user.jce:bgfx_bx_debug_config"
_BX_DEBUG_INPUTS_CONF = "user.jce:bgfx_bx_debug_inputs"
_BX_DEBUG_CONFIG = "upstream-cmake-v1"
_GL_BLIT_CONFIG_CONF = "user.jce:bgfx_gl_blit_config"
_GL_BLIT_CONFIG = "upstream-2d-fbo-v1"
_XP_BACKEND_CONF = "user.jce:bgfx_xp_backend"
_XP_BACKEND = "opengl-d3d11-d3d12-vulkan-v3"
_XP_RENDERERS = ("AGC", "DIRECT3D11", "DIRECT3D12", "GNM", "METAL", "NVN",
                 "OPENGL", "OPENGLES", "VULKAN", "WEBGPU")


def _xp_renderer_configuration(conanfile, settings):
    enabled = conanfile.conf.get("user.jce:windows_xp", default=False)
    if type(enabled) is not bool:
        raise ValueError("user.jce:windows_xp must be a boolean")
    if not enabled:
        return None
    if (str(settings.get_safe("os")) != "Windows"
            or str(settings.get_safe("compiler")) != "clang"
            or str(settings.get_safe("arch")) not in ("x86", "x86_64")
            or any(settings.get_safe(key) not in (None, "") for key in
                   ("compiler.runtime", "compiler.runtime_type", "compiler.runtime_version"))):
        raise ValueError("XP bgfx requires the isolated GNU Windows Clang target")
    libcxx = settings.get_safe("compiler.libcxx")
    if libcxx not in (None, "", "libc++"):
        raise ValueError("XP bgfx requires libc++ when a C++ runtime is declared")
    definitions = {"BGFX_CONFIG_RENDERER_" + name: 0 for name in _XP_RENDERERS}
    definitions["BGFX_CONFIG_RENDERER_OPENGL"] = _GRAPHICS_TIERS[_graphics_tier_name(conanfile)]["opengl"]
    for name in ("DIRECT3D11", "DIRECT3D12", "VULKAN"):
        definitions["BGFX_CONFIG_RENDERER_" + name] = 1
    # Defining any renderer suppresses upstream's entire automatic selection.
    # Validate the complete set, including explicit zeroes, before modifying it.
    _validate_reserved_definitions(conanfile, {**definitions, "BGFX_CONFIG_VIDEO": 0},
                                   "XP renderer", cmake_options=("BGFX_CONFIG_VIDEO",))
    variables = conanfile.conf.get("tools.cmake.cmaketoolchain:extra_variables", default={}, check_type=dict)
    if "BGFX_CONFIG_VIDEO" in variables and _annotation_bool(variables["BGFX_CONFIG_VIDEO"]) != 0:
        raise ValueError("XP renderer requires BGFX_CONFIG_VIDEO=OFF")
    return definitions


def _validate_reserved_definitions(conanfile, definitions, label, *, strict_dynamic=True, cmake_options=()):
    """Check exact reserved operands across the existing compiler channels."""
    variables = conanfile.conf.get("tools.cmake.cmaketoolchain:extra_variables", default={}, check_type=dict)
    flag_sets = [conanfile.conf.get(key, default=[], check_type=list)
                 for key in ("tools.build:cflags", "tools.build:cxxflags")]
    for name, value in variables.items():
        if name.startswith(("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS")):
            literal = _cmake_literal(value)
            if literal is None:
                if strict_dynamic:
                    raise ValueError(label + " cannot qualify dynamic CMake compiler flags")
                continue
            flag_sets.append([literal])
    for flags in flag_sets:
        tokens = []
        for token in shlex.split(" ".join(flags), posix=False):
            # GCC/Clang forward comma-separated operands to the preprocessor.
            # Inspect those operands before configuration-specific flags can
            # override the base/directory policy during a real compile.
            token = token.strip("\"'")
            tokens.extend(token[4:].split(",") if token.startswith("-Wp,") else (token,))
        for index, token in enumerate(tokens):
            token = token.strip("\"'")
            if token[:2] not in ("-D", "/D", "-U", "/U"):
                continue
            operand = token[2:] or (tokens[index + 1] if index + 1 < len(tokens) else "")
            name, equals, value = operand.strip("\"'").partition("=")
            if name in definitions and (definitions[name] is None or token[1] != "D" or not equals or value != str(definitions[name])):
                raise ValueError(label + " compiler definition conflicts with " + name)
    for definition in conanfile.conf.get("tools.build:defines", default=[], check_type=list):
        name, equals, value = definition.partition("=")
        if name in definitions and (definitions[name] is None or not equals or value != str(definitions[name])):
            raise ValueError(label + " directory definition conflicts with " + name)
    for name, value in variables.items():
        if name in definitions and name not in cmake_options and str(_cmake_literal(value)) != str(definitions[name]):
            raise ValueError(label + " CMake definition conflicts with " + name)

# A tier is a SUPPORT-MATRIX choice, not a performance one.  The JCE renderer
# builds the newest core context the driver grants at or above the floor, so a
# "stable" build already runs at 4.6 on hardware that offers it.  Raising the
# tier changes almost nothing on capable GPUs -- see the measurement in
# jce_renderer_caps.h -- and it REFUSES every GPU below the new floor.  Pick a
# higher tier to narrow what you ship to, never to go faster.
#
# stable is OpenGL 3.1 rather than 3.3 because 3.1 is the true minimum of this
# configuration: bgfx writes `#version 140`, which is GLSL 1.40, which is
# GL 3.1.
_GRAPHICS_TIERS = {
    "stable": {"value": 0, "opengl": 31, "opengles": 30},
    "modern": {"value": 1, "opengl": 43, "opengles": 31},
    "current": {"value": 2, "opengl": 46, "opengles": 32},
    "compatibility": {"value": 3, "opengl": 21, "opengles": 20},
}

def _graphics_tier_name(conanfile):
    value = conanfile.conf.get(
        _GRAPHICS_TIER_CONF, default="stable", check_type=str
    )
    tier = value.strip().lower()
    if tier not in _GRAPHICS_TIERS:
        raise ValueError(
            "%s must be one of stable, modern, current, compatibility (got %r)"
            % (_GRAPHICS_TIER_CONF, value)
        )
    return tier


def _gl_blit_emulation_enabled(conanfile):
    # Upstream already owns the 2D color FBO copy fallback. Its native copy
    # path still wins when present; this does not add 3D/array/depth blits.
    enabled = int(_graphics_tier_name(conanfile) == "compatibility")
    _validate_reserved_definitions(conanfile, {"BGFX_GL_CONFIG_BLIT_EMULATION": enabled},
                                   "GL blit policy", strict_dynamic=bool(enabled))
    return enabled


def _cpu_profiler_enabled(conanfile):
    value = conanfile.conf.get(_CPU_PROFILER_CONF, default="off", check_type=str)
    selected = value.strip().lower()
    if selected not in ("off", "on"):
        raise ValueError("%s must be off or on (got %r)" % (_CPU_PROFILER_CONF, value))
    enabled = int(selected == "on")
    flags = conanfile.conf.get("tools.build:cxxflags", default=[], check_type=list)
    # Preserve caller flags verbatim. This reserved definition must agree with
    # the binary identity even when Conan resolves a previously built package.
    # Non-POSIX tokenization preserves Windows path backslashes. Inspect only
    # actual define/undef operands, never similarly named macros or path text.
    tokens = shlex.split(" ".join(flags), posix=False)
    for index, token in enumerate(tokens):
        token = token.strip("\"'")
        if token[:2] not in ("-D", "/D", "-U", "/U"):
            continue
        operand = token[2:] or (tokens[index + 1] if index + 1 < len(tokens) else "")
        name, equals, value = operand.strip("\"'").partition("=")
        if name == "BGFX_CONFIG_PROFILER" and (
                token[1] != "D" or not equals or value != str(enabled)):
            raise ValueError("BGFX_CONFIG_PROFILER compiler flags conflict with %s=%s" %
                             (_CPU_PROFILER_CONF, selected))
    for definition in conanfile.conf.get("tools.build:defines", default=[], check_type=list):
        name, equals, value = definition.partition("=")
        if name.strip() == "BGFX_CONFIG_PROFILER" and (not equals or value != str(enabled)):
            raise ValueError("BGFX_CONFIG_PROFILER directory defines conflict with %s=%s" %
                             (_CPU_PROFILER_CONF, selected))
    variables = conanfile.conf.get(
        "tools.cmake.cmaketoolchain:extra_variables", default={}, check_type=dict)
    if "BGFX_CONFIG_PROFILER" in variables and str(variables["BGFX_CONFIG_PROFILER"]) != str(enabled):
        raise ValueError("BGFX_CONFIG_PROFILER CMake variable conflicts with %s=%s" %
                         (_CPU_PROFILER_CONF, selected))
    return enabled



def _cmake_literal(value):
    if isinstance(value, dict):
        value = value.get("value")
    if not isinstance(value, (str, int, bool)):
        return None
    text = str(value)
    # Expressions/toolchain substitutions cannot prove a compiled capability.
    if any(char in text for char in ("$", ";", "\r", "\n")):
        return None
    return text


def _annotation_bool(value):
    text = _cmake_literal(value)
    if text is None:
        return -1
    text = text.upper()
    return int(text not in ("", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND")
               and not text.endswith("-NOTFOUND"))


def _annotation_cmake_keys(build_type):
    keys = ("CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_INIT")
    if build_type:
        keys += ("CMAKE_CXX_FLAGS_" + str(build_type).upper(),
                 "CMAKE_CXX_FLAGS_" + str(build_type).upper() + "_INIT")
    return keys


def _annotation_cmake_flags(variables, build_type):
    keys = _annotation_cmake_keys(build_type)
    return [_cmake_literal(value) for key, value in variables.items() if key in keys]


def _annotation_bound_inputs(value, budget, depth=0):
    budget[0] -= 1
    if budget[0] < 0 or depth > 8:
        raise ValueError("annotation unknown-input fingerprint exceeds structure budget")
    if isinstance(value, str):
        budget[1] -= len(value)
        if budget[1] < 0:
            raise ValueError("annotation unknown-input fingerprint exceeds text budget")
    elif isinstance(value, dict):
        if len(value) > 64:
            raise ValueError("annotation unknown-input fingerprint has too many fields")
        for key, item in value.items():
            if not isinstance(key, str):
                raise ValueError("annotation unknown-input fingerprint requires string keys")
            _annotation_bound_inputs(key, budget, depth + 1)
            _annotation_bound_inputs(item, budget, depth + 1)
    elif isinstance(value, (list, tuple)):
        if len(value) > 64:
            raise ValueError("annotation unknown-input fingerprint has too many items")
        for item in value:
            _annotation_bound_inputs(item, budget, depth + 1)
    elif isinstance(value, int):
        if value.bit_length() > 64:
            raise ValueError("annotation unknown-input fingerprint integer is too large")
    elif value is not None and not isinstance(value, float):
        raise ValueError("annotation unknown-input fingerprint has unsupported value")


def _annotation_fingerprint(inputs):
    # Existing configuration requests only: never open a toolchain or package.
    _annotation_bound_inputs(inputs, [256, 8192])
    canonical = json.dumps(inputs, sort_keys=True, ensure_ascii=True,
                           allow_nan=False, separators=(",", ":"))
    if len(canonical) > 16384:
        raise ValueError("annotation unknown-input fingerprint exceeds serialized budget")
    return hashlib.sha256(canonical.encode("ascii")).hexdigest()


def _annotation_reserved_flags(conanfile, compiled, variables, build_type):
    flags = list(conanfile.conf.get("tools.build:cxxflags", default=[], check_type=list))
    flags += [value for value in _annotation_cmake_flags(variables, build_type)
              if value is not None]
    tokens = shlex.split(" ".join(flags), posix=False)
    for index, token in enumerate(tokens):
        token = token.strip("\"'")
        if token[:2] not in ("-D", "/D", "-U", "/U"):
            continue
        operand = token[2:] or (tokens[index + 1] if index + 1 < len(tokens) else "")
        name, equals, value = operand.strip("\"'").partition("=")
        if name == "BGFX_CONFIG_DEBUG_ANNOTATION" and (
                compiled < 0 or token[1] != "D" or not equals or value != str(compiled)):
            raise ValueError("BGFX_CONFIG_DEBUG_ANNOTATION compiler flags conflict with effective upstream configuration")
    for definition in conanfile.conf.get("tools.build:defines", default=[], check_type=list):
        name, equals, value = definition.partition("=")
        if name.strip() == "BGFX_CONFIG_DEBUG_ANNOTATION" and (
                compiled < 0 or not equals or value != str(compiled)):
            raise ValueError("BGFX_CONFIG_DEBUG_ANNOTATION directory defines conflict with effective upstream configuration")


def _annotation_configuration(conanfile, settings):
    selected = conanfile.conf.get(_ANNOTATION_CONF, default="auto", check_type=str).strip().lower()
    if selected not in ("auto", "off", "on"):
        raise ValueError("%s must be auto, off or on (got %r)" % (_ANNOTATION_CONF, selected))
    variables = conanfile.conf.get("tools.cmake.cmaketoolchain:extra_variables", default={}, check_type=dict)
    raw = variables.get("BGFX_CONFIG_DEBUG_ANNOTATION", False)
    option = _annotation_bool(raw)
    if selected != "auto":
        wanted = int(selected == "on")
        if "BGFX_CONFIG_DEBUG_ANNOTATION" in variables and option != wanted:
            raise ValueError("BGFX_CONFIG_DEBUG_ANNOTATION CMake option conflicts with %s=%s" % (_ANNOTATION_CONF, selected))
        option = wanted
    build_type = settings.get_safe("build_type")
    os_name = settings.get_safe("os")
    system_conf = conanfile.conf.get(
        "tools.cmake.cmaketoolchain:system_name", default=os_name)
    system_input = variables.get("CMAKE_SYSTEM_NAME", system_conf)
    system = _cmake_literal(system_input)
    custom = conanfile.conf.get("tools.cmake.cmaketoolchain:user_toolchain", default=[], check_type=list)
    if (custom or "CMAKE_TOOLCHAIN_FILE" in variables or "CMAKE_CONFIGURATION_TYPES" in variables
            or None in _annotation_cmake_flags(variables, build_type)
            or ("CMAKE_BUILD_TYPE" in variables and _cmake_literal(variables["CMAKE_BUILD_TYPE"]) != build_type)):
        compiled = -1
    elif system == "WindowsStore":
        compiled = 0
    elif system is None or not system or build_type not in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
        compiled = -1
    else:
        # This is the pinned upstream target's expression, not request==on.
        # In particular OFF cannot suppress Debug; WindowsStore always excludes.
        compiled = 1 if build_type == "Debug" or option == 1 else option
    _annotation_reserved_flags(conanfile, compiled, variables, build_type)
    # Historical default belongs to Conan settings, before caller overrides.
    default_compiled = int(os_name != "WindowsStore" and build_type == "Debug")
    unknown_inputs = None
    if compiled < 0:
        relevant = _annotation_cmake_keys(build_type) + (
            "BGFX_CONFIG_DEBUG_ANNOTATION", "CMAKE_SYSTEM_NAME", "CMAKE_BUILD_TYPE",
            "CMAKE_CONFIGURATION_TYPES", "CMAKE_TOOLCHAIN_FILE")
        unknown_inputs = {
            "request": selected, "option": raw, "os": os_name,
            "build_type": None if build_type is None else str(build_type),
            "system_conf": system_conf, "system": system_input, "user_toolchain": custom,
            "extra_variables": {key: value for key, value in variables.items() if key in relevant},
            "cxxflags": conanfile.conf.get("tools.build:cxxflags", default=[], check_type=list),
            "defines": conanfile.conf.get("tools.build:defines", default=[], check_type=list),
        }
    return selected, option, compiled, raw, default_compiled, unknown_inputs


def _bx_debug_configuration(conanfile, settings):
    raw_request = conanfile.conf.get(_BX_DEBUG_CONF, default="auto", check_type=str)
    if not isinstance(raw_request, str) or raw_request.strip().lower() not in ("auto", "off", "on"):
        raise ValueError("%s must be auto, off or on (got %r)" % (_BX_DEBUG_CONF, raw_request))
    selected = raw_request.strip().lower()
    variables = conanfile.conf.get("tools.cmake.cmaketoolchain:extra_variables", default={}, check_type=dict)
    raw = variables.get("BX_CONFIG_DEBUG", False)
    option = _annotation_bool(raw)
    if selected != "auto":
        wanted = int(selected == "on")
        if "BX_CONFIG_DEBUG" in variables and option != wanted:
            raise ValueError("BX_CONFIG_DEBUG CMake option conflicts with %s=%s" % (_BX_DEBUG_CONF, selected))
        option = wanted
    build_type = settings.get_safe("build_type")
    os_name = settings.get_safe("os")
    system_conf = conanfile.conf.get("tools.cmake.cmaketoolchain:system_name", default=os_name)
    system_input = variables.get("CMAKE_SYSTEM_NAME", system_conf)
    system = _cmake_literal(system_input)
    custom = conanfile.conf.get("tools.cmake.cmaketoolchain:user_toolchain", default=[], check_type=list)
    flag_keys = _annotation_cmake_keys(build_type)
    flag_keys += tuple(key.replace("CXX", "C") for key in flag_keys)
    flags = [_cmake_literal(value) for key, value in variables.items() if key in flag_keys]
    for key in ("tools.build:cflags", "tools.build:cxxflags", "tools.build:defines"):
        values = conanfile.conf.get(key, default=[], check_type=list)
        flags.extend(_cmake_literal(value) for value in values)
    if (custom or "CMAKE_TOOLCHAIN_FILE" in variables or "CMAKE_CONFIGURATION_TYPES" in variables
            or None in flags or system is None or not system
            or build_type not in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
            or ("CMAKE_BUILD_TYPE" in variables and _cmake_literal(variables["CMAKE_BUILD_TYPE"]) != build_type)):
        compiled = -1
    else:
        # Both pinned bx/bgfx expressions force Debug on, regardless of OFF.
        # Unlike debug annotations, BX debug has no WindowsStore exclusion.
        compiled = 1 if build_type == "Debug" else option
    _validate_reserved_definitions(conanfile, {"BX_CONFIG_DEBUG": compiled if compiled >= 0 else None},
                                   "BX debug", strict_dynamic=False, cmake_options=("BX_CONFIG_DEBUG",))
    default_compiled = int(build_type == "Debug")
    unknown_inputs = None
    if compiled < 0:
        relevant = flag_keys + ("BX_CONFIG_DEBUG", "CMAKE_SYSTEM_NAME", "CMAKE_BUILD_TYPE",
                                "CMAKE_CONFIGURATION_TYPES", "CMAKE_TOOLCHAIN_FILE")
        unknown_inputs = {
            "request": selected, "option": raw, "os": os_name,
            "build_type": None if build_type is None else str(build_type),
            "system_conf": system_conf, "system": system_input, "user_toolchain": custom,
            "extra_variables": {key: value for key, value in variables.items() if key in relevant},
            "cflags": conanfile.conf.get("tools.build:cflags", default=[], check_type=list),
            "cxxflags": conanfile.conf.get("tools.build:cxxflags", default=[], check_type=list),
            "defines": conanfile.conf.get("tools.build:defines", default=[], check_type=list),
        }
    return selected, option, compiled, raw, default_compiled, unknown_inputs

def _bx_debug_consumer_configuration(conanfile, configuration):
    raw = conanfile.conf.get(_BX_DEBUG_CONSUMER_CONF, default="auto", check_type=str)
    if not isinstance(raw, str) or raw.strip().lower() not in ("auto", "off", "on"):
        raise ValueError("%s must be auto, off or on (got %r)" % (_BX_DEBUG_CONSUMER_CONF, raw))
    selected = raw.strip().lower()
    consumer = -1 if selected == "auto" else int(selected == "on")
    expected = configuration[2]
    if expected < 0:
        # This is the requested upstream expression, not a provider observation.
        # ON makes Debug OR option true for every configuration. Otherwise a
        # single known requested configuration can bound that expression.
        if configuration[1] == 1:
            expected = 1
        else:
            inputs = configuration[5]
            variables = inputs["extra_variables"]
            build_type = inputs["build_type"]
            if (build_type in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
                    and "CMAKE_CONFIGURATION_TYPES" not in variables
                    and ("CMAKE_BUILD_TYPE" not in variables
                         or _cmake_literal(variables["CMAKE_BUILD_TYPE"]) == build_type)):
                expected = 1 if build_type == "Debug" else configuration[1]
    if consumer >= 0 and expected >= 0 and consumer != expected:
        raise ValueError("BX debug consumer assertion conflicts with the known/requested upstream configuration")
    if configuration[2] < 0 and configuration[0] != "auto" and consumer < 0:
        raise ValueError("BX debug unknown explicit provider requires user.jce:bgfx_bx_debug_consumer=off|on")
    return consumer


def _bx_debug_export_configuration(conanfile, configuration):
    consumer = _bx_debug_consumer_configuration(conanfile, configuration)
    target = conanfile.cpp_info.components.get("bx")
    if target is None:
        if consumer >= 0:
            raise ValueError("BX debug consumer assertion requires the bx component")
        return consumer
    reserved = [(index, value) for index, value in enumerate(target.defines)
                if value.partition("=")[0].strip() == "BX_CONFIG_DEBUG"]
    if not reserved:
        if consumer >= 0:
            target.defines.append("BX_CONFIG_DEBUG=%d" % consumer)
        return consumer
    if len(reserved) != 1 or reserved[0][1] not in ("BX_CONFIG_DEBUG=0", "BX_CONFIG_DEBUG=1"):
        raise ValueError("BX debug component definition is duplicate or noncanonical")
    index, current = reserved[0]
    baseline = "BX_CONFIG_DEBUG=%d" % configuration[4]
    compiled = configuration[2]
    if compiled < 0 and consumer < 0:
        if current != baseline:
            raise ValueError("BX debug unknown component definition differs from the recipe baseline")
        # Unknown auto keeps its historical consumer default without claiming
        # the provider is known. Explicit assertions are labeled separately.
    else:
        actual = "BX_CONFIG_DEBUG=%d" % (compiled if compiled >= 0 else consumer)
        if current not in (baseline, actual):
            raise ValueError("BX debug component definition contradicts the effective configuration")
        target.defines[index] = actual
    return consumer


def post_package_id(conanfile):
    """Partition bgfx binaries by their real floor and optional CPU profiler."""
    if conanfile.name != "bgfx":
        return
    blit = _gl_blit_emulation_enabled(conanfile)
    xp = _xp_renderer_configuration(conanfile, conanfile.info.settings)
    bx_debug = _bx_debug_configuration(conanfile, conanfile.info.settings)
    if blit:
        conanfile.info.conf.define(_GL_BLIT_CONFIG_CONF, _GL_BLIT_CONFIG)
    if xp is not None:
        conanfile.info.conf.define(_XP_BACKEND_CONF, _XP_BACKEND)
    tier = _graphics_tier_name(conanfile)
    profiler = _cpu_profiler_enabled(conanfile)
    conanfile.info.conf.define(_GRAPHICS_TIER_CONF, tier)
    conanfile.info.conf.define(_CPU_PROFILER_CONF, "on" if profiler else "off")
    if profiler:
        conanfile.info.conf.define(_CPU_PROFILER_CONFIG_CONF, _CPU_PROFILER_CONFIG)
    annotation = _annotation_configuration(conanfile, conanfile.info.settings)
    # Keep the exact historical package ID when the compiled macro is the
    # upstream default: auto/off, Debug's forced ON and WindowsStore's exclusion
    # need no new bgfx binary. Request metadata alone must not invalidate it.
    unknown_digest = _annotation_fingerprint(annotation[5]) if annotation[2] < 0 else None
    if annotation[2] < 0 or annotation[2] != annotation[4]:
        if unknown_digest is not None:
            conanfile.info.conf.define(_ANNOTATION_INPUTS_CONF, unknown_digest)
        conanfile.info.conf.define(_ANNOTATION_OPTION_CONF,
            ("on" if annotation[1] else "off") if annotation[1] >= 0 else "unknown:" + repr(annotation[3]))
        conanfile.info.conf.define(_ANNOTATION_COMPILED_CONF, str(annotation[2]))
        conanfile.info.conf.define(_ANNOTATION_CONFIG_CONF, _ANNOTATION_CONFIG)
    if bx_debug[2] < 0 or bx_debug[2] != bx_debug[4]:
        if bx_debug[2] < 0:
            conanfile.info.conf.define(_BX_DEBUG_INPUTS_CONF, _annotation_fingerprint(bx_debug[5]))
        conanfile.info.conf.define(_BX_DEBUG_OPTION_CONF,
            ("on" if bx_debug[1] else "off") if bx_debug[1] >= 0 else "unknown:" + repr(bx_debug[3]))
        conanfile.info.conf.define(_BX_DEBUG_COMPILED_CONF, str(bx_debug[2]))
        conanfile.info.conf.define(_BX_DEBUG_CONFIG_CONF, _BX_DEBUG_CONFIG)
    conanfile.info.conf.define("user.jce:source_policy", "pristine-v1")


def pre_generate(conanfile):
    if conanfile.name != "bgfx":
        return
    blit = _gl_blit_emulation_enabled(conanfile)
    xp = _xp_renderer_configuration(conanfile, conanfile.settings)
    tier = _GRAPHICS_TIERS[_graphics_tier_name(conanfile)]
    profiler = _cpu_profiler_enabled(conanfile)
    annotation = _annotation_configuration(conanfile, conanfile.settings)
    bx_debug = _bx_debug_configuration(conanfile, conanfile.settings)
    key = "tools.cmake.cmaketoolchain:extra_variables"
    variables = dict(conanfile.conf.get(key, default={}, check_type=dict))
    if annotation[0] != "auto" and "BGFX_CONFIG_DEBUG_ANNOTATION" not in variables:
        variables["BGFX_CONFIG_DEBUG_ANNOTATION"] = annotation[1]
    if bx_debug[0] != "auto" and "BX_CONFIG_DEBUG" not in variables:
        variables["BX_CONFIG_DEBUG"] = {"value": bool(bx_debug[1]), "cache": True, "type": "BOOL", "force": True}
    if xp is not None:
        # Keep the XP package graphics-only. Media still uses JCE's decoders.
        variables["BGFX_CONFIG_VIDEO"] = {"value": False, "cache": True, "type": "BOOL", "force": True}
    variables.update({
        "BGFX_OPENGL_VERSION": tier["opengl"],
        "BGFX_OPENGLES_VERSION": tier["opengles"],
        "BGFX_CONFIG_MULTITHREADED": 0 if str(conanfile.settings.os) == "Emscripten" else 1,
    })
    conanfile.conf.define(key, variables)
    flag_key = "tools.build:cxxflags"
    flags = list(conanfile.conf.get(flag_key, default=[], check_type=list))
    # bgfx consumes a preprocessor define; a CMake cache entry alone has no
    # effect. Explicit zero keeps normal shipping builds free of these scopes.
    profiler_define = "-DBGFX_CONFIG_PROFILER=%d" % profiler
    if profiler_define not in flags:
        flags.append(profiler_define)
    if str(conanfile.settings.os) != "Emscripten":
        define = "-DBGFX_CONFIG_MAX_MATRIX_CACHE=131072"
        if define not in flags:
            flags.append(define)
    if blit and "-DBGFX_GL_CONFIG_BLIT_EMULATION=1" not in flags:
        flags.append("-DBGFX_GL_CONFIG_BLIT_EMULATION=1")
    conanfile.conf.define(flag_key, flags)
    # Upstream's Apple/Ninja branch replaces CMAKE_CXX_FLAGS after project().
    # Conan emits these as directory add_compile_definitions, which survives
    # that reset and is inherited by the actual bgfx target's subdirectory.
    define_key = "tools.build:defines"
    defines = list(conanfile.conf.get(define_key, default=[], check_type=list))
    profiler_definition = "BGFX_CONFIG_PROFILER=%d" % profiler
    if profiler_definition not in defines:
        defines.append(profiler_definition)
    if blit and "BGFX_GL_CONFIG_BLIT_EMULATION=1" not in defines:
        defines.append("BGFX_GL_CONFIG_BLIT_EMULATION=1")
    conanfile.conf.define(define_key, defines)
    if xp is not None:
        for name, value in xp.items():
            definition = name + "=" + str(value)
            if definition not in defines:
                defines.append(definition)
        conanfile.conf.define(define_key, defines)
        for key in ("tools.build:cflags", "tools.build:cxxflags"):
            flags = list(conanfile.conf.get(key, default=[], check_type=list))
            for name, value in xp.items():
                definition = "-D" + name + "=" + str(value)
                if definition not in flags:
                    flags.append(definition)
            conanfile.conf.define(key, flags)


def post_package_info(conanfile):
    """Expose the package's real floor to JCE's renderer compilation."""
    if conanfile.name != "bgfx":
        return
    blit = _gl_blit_emulation_enabled(conanfile)
    xp = _xp_renderer_configuration(conanfile, conanfile.settings)
    tier = _graphics_tier_name(conanfile)
    cfg = _GRAPHICS_TIERS[tier]
    profiler = _cpu_profiler_enabled(conanfile)
    annotation = _annotation_configuration(conanfile, conanfile.settings)
    bx_debug = _bx_debug_configuration(conanfile, conanfile.settings)
    consumer = _bx_debug_export_configuration(conanfile, bx_debug)
    defines = [
        "JCE_GRAPHICS_API_TIER_VALUE=%d" % cfg["value"],
        "JCE_BGFX_OPENGL_VERSION=%d" % cfg["opengl"],
        "JCE_BGFX_OPENGLES_VERSION=%d" % cfg["opengles"],
        "JCE_BGFX_CPU_PROFILER_COMPILED=%d" % profiler,
        "JCE_BGFX_DEBUG_ANNOTATION_REQUEST=%d" % (-1 if annotation[0] == "auto" else int(annotation[0] == "on")),
        "JCE_BGFX_DEBUG_ANNOTATION_OPTION=%d" % annotation[1],
        "JCE_BGFX_DEBUG_ANNOTATION_COMPILED=%d" % annotation[2],
        "JCE_BGFX_BX_DEBUG_REQUEST=%d" % (-1 if bx_debug[0] == "auto" else int(bx_debug[0] == "on")),
        "JCE_BGFX_BX_DEBUG_OPTION=%d" % bx_debug[1],
        "JCE_BGFX_BX_DEBUG_COMPILED=%d" % bx_debug[2],
    ]
    if consumer >= 0:
        defines.extend(["JCE_BGFX_BX_DEBUG_CONSUMER_ASSERTED=1",
                        "JCE_BGFX_BX_DEBUG_CONSUMER_VALUE=%d" % consumer])
    if blit:
        defines.append("JCE_BGFX_GL_BLIT_EMULATION=1")
    if xp is not None:
        defines.extend(["JCE_BGFX_XP_D3D11=1", "JCE_BGFX_XP_D3D12=1", "JCE_BGFX_XP_VULKAN=1"])
    targets = [conanfile.cpp_info]
    targets.extend(conanfile.cpp_info.components.values())
    for target in targets:
        for define in defines:
            if define not in target.defines:
                target.defines.append(define)

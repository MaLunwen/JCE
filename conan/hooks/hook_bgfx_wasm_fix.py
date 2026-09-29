"""Select bgfx build configuration without changing any upstream file.

This hook changes Conan configuration and package metadata only. Previous JCE
source patches are retired; existing Conan sources/binaries are not repaired.
A changed binary identity prevents a previously patched bgfx package being
silently reused by a new package resolution.
"""
_GRAPHICS_TIER_CONF = "user.jce:bgfx_graphics_tier"
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
}

def _graphics_tier_name(conanfile):
    value = conanfile.conf.get(
        _GRAPHICS_TIER_CONF, default="stable", check_type=str
    )
    tier = value.strip().lower()
    if tier not in _GRAPHICS_TIERS:
        raise ValueError(
            "%s must be one of stable, modern, current (got %r)"
            % (_GRAPHICS_TIER_CONF, value)
        )
    return tier


def post_package_id(conanfile):
    """Make the selected GL/GLES floor part of bgfx's binary identity."""
    if conanfile.name != "bgfx":
        return
    tier = _graphics_tier_name(conanfile)
    conanfile.info.conf.define(_GRAPHICS_TIER_CONF, tier)
    conanfile.info.conf.define("user.jce:source_policy", "pristine-v1")


def pre_generate(conanfile):
    if conanfile.name != "bgfx":
        return
    tier = _GRAPHICS_TIERS[_graphics_tier_name(conanfile)]
    key = "tools.cmake.cmaketoolchain:extra_variables"
    variables = dict(conanfile.conf.get(key, default={}, check_type=dict))
    variables.update({
        "BGFX_OPENGL_VERSION": tier["opengl"],
        "BGFX_OPENGLES_VERSION": tier["opengles"],
        "BGFX_CONFIG_MULTITHREADED": 0 if str(conanfile.settings.os) == "Emscripten" else 1,
    })
    conanfile.conf.define(key, variables)
    if str(conanfile.settings.os) != "Emscripten":
        flag_key = "tools.build:cxxflags"
        flags = list(conanfile.conf.get(flag_key, default=[], check_type=list))
        define = "-DBGFX_CONFIG_MAX_MATRIX_CACHE=131072"
        if define not in flags:
            flags.append(define)
        conanfile.conf.define(flag_key, flags)


def post_package_info(conanfile):
    """Expose the package's real floor to JCE's renderer compilation."""
    if conanfile.name != "bgfx":
        return
    tier = _graphics_tier_name(conanfile)
    cfg = _GRAPHICS_TIERS[tier]
    defines = [
        "JCE_GRAPHICS_API_TIER_VALUE=%d" % cfg["value"],
        "JCE_BGFX_OPENGL_VERSION=%d" % cfg["opengl"],
        "JCE_BGFX_OPENGLES_VERSION=%d" % cfg["opengles"],
    ]
    targets = [conanfile.cpp_info]
    targets.extend(conanfile.cpp_info.components.values())
    for target in targets:
        for define in defines:
            if define not in target.defines:
                target.defines.append(define)

#!/usr/bin/env python3
"""
check_gfx_api_tiers.py — the graphics-API version ladder is written down three
times, in three languages, and nothing made the copies agree.

WHAT THE LADDER IS.  bgfx bakes its minimum GL / GLES version in at COMPILE
time (BGFX_CONFIG_RENDERER_OPENGL_MIN_VERSION).  Left unset it falls back to 1,
i.e. desktop GL pinned to 2.1 / GLSL 1.20 forever.  JCE selects a rung with the
conf `user.jce:bgfx_graphics_tier` (stable | modern | current), and
conan/hooks/hook_bgfx_wasm_fix.py turns that into bgfx cache variables plus
three consumer defines.

WHERE THE THREE COPIES ARE:

  1. conan/hooks/hook_bgfx_wasm_fix.py   _GRAPHICS_TIERS  -- Python dict, the
     value actually compiled into libbgfx.
  2. engine/src/renderer/jce_renderer_caps.c  the `#if JCE_GRAPHICS_API_TIER_VALUE
     == n` block -- preprocessor assertions that the package's floor matches the
     tier it claims.
  3. the same file's jce_renderer_api_tier_minimum() gl[]/gles[] tables -- the
     numbers the ENGINE reports to callers and to the editor's UI.

Copy 2 fails the build when it disagrees with copy 1, which is good.  Copy 3
fails NOTHING: it is a plain lookup table, so a drifted entry means the engine
truthfully compiles against GL 4.3 while telling every caller, log line and
editor panel that its floor is 3.3.  That is the same shape as every
"no symptom" defect in this tree -- a wrong value that behaves exactly like a
right one.

THE SHADER PROFILE IS ALSO CHECKED, for a related reason.  On 2026-08-31 the
OpenGL shader profile in tools/compile_shaders.cmake was raised 120 -> 330 to
"modernise GL".  It was wrong three times over and nothing said so:

  * It does not compile.  This shaderc routes glsl <= 400 through
    glsl-optimizer, which supports 1.10-1.50 only, so 330 and 400 are the exact
    two values it cannot emit (410+ works again, via glslang).  Every OpenGL
    build in the tree broke for a day.
  * It would not have mattered.  bgfx REWRITES each shader's `#version` in
    ShaderGL::create -- 120/130 by the identifiers the source uses, 430 kept for
    compute, 140 when built with BGFX_CONFIG_RENDERER_OPENGL >= 31.
  * It fights bgfx's own scheme: the `#version 140` path ships compat #defines
    (texture2DLod -> textureLod, ...) that adapt 120-style source upward, which
    is why bgfx's own shader.mk uses GLSL_LEVEL=120.

The lever for a newer GL is the tier, never this profile.

Vulkan is deliberately absent from the cross-check: bgfx calls
vkEnumerateInstanceVersion at runtime and takes max(device version, 1.0), so it
negotiates the newest version each device offers.  The vk[] table in copy 3 is
advisory only and has no counterpart in copy 1 to check it against.

Usage:
    python tools/lint/check_gfx_api_tiers.py
    python tools/lint/check_gfx_api_tiers.py --list
Exit 0 clean, 1 on drift or an unusable shader profile.
"""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
HOOK = REPO_ROOT / "conan" / "hooks" / "hook_bgfx_wasm_fix.py"
CAPS = REPO_ROOT / "engine" / "src" / "renderer" / "jce_renderer_caps.c"
SHADERS = REPO_ROOT / "tools" / "compile_shaders.cmake"
# The editor compiles shader-graph output with the same shaderc and had its
# OWN copy of the profile table.  The 120 -> 330 change landed in both files;
# a gate that guarded only the engine one would have called that half-fixed
# tree clean.
GRAPH = REPO_ROOT / "editor" / "src" / "shadergraph" / "jce_shadergraph_shaderc.cpp"
# The per-platform cook lists and the runtime suffix map.  "Only cook a profile
# the loader can pick" is stated three times in JCESDKHelpers.cmake prose and
# enforced by nothing: essl1 was compiled and PAKed on Android and Web for as
# long as it took someone to notice shader_suffix() never asks for it.  The
# prose survived because it was right -- the cook lists dropped essl1 -- but
# prose does not fail a build, and this lands hardest on exactly the mobile and
# web targets that pay for every packed byte.
HELPERS = REPO_ROOT / "cmake" / "JCESDKHelpers.cmake"
LOADER = REPO_ROOT / "engine" / "src" / "renderer" / "jce_shaders.c"
GRAPH_GL = re.compile(r'JCE_BACKEND_OPENGL:\s*return\s*\{[^}]*?"(\w+)"\s*\}')

# What this shaderc can actually emit for the `glsl` suffix.  Measured by
# compiling one shader at each: 120/130/140/150 OK, 330 and 400 rejected by
# glsl-optimizer, 410+ OK via glslang.  Only <=150 is acceptable, because
# bgfx's version rewrite expects 120-style source.
USABLE_GLSL_PROFILES = {"120", "130", "140", "150"}
REQUIRED_COMPUTE_GLSL = "430"
REQUIRED_COMPUTE_ESSL = "310_es"


def fail(msg: str) -> None:
    print("  " + msg, file=sys.stderr)


def hook_tiers(text: str):
    """copy 1 -> {name: {'value': n, 'opengl': n, 'opengles': n}}"""
    m = re.search(r"^_GRAPHICS_TIERS\s*=\s*(\{.*?^\})", text, re.M | re.S)
    if not m:
        return None
    try:
        return ast.literal_eval(m.group(1))
    except (ValueError, SyntaxError):
        return None


def caps_preprocessor(text: str):
    """copy 2 -> {tier_value: (opengl, opengles)}"""
    out = {}
    for tier, gl, gles in re.findall(
            r"JCE_GRAPHICS_API_TIER_VALUE\s*==\s*(\d+)\s*\n"
            r"#\s*if\s+JCE_BGFX_OPENGL_VERSION\s*!=\s*(\d+)\s*\|\|\s*"
            r"JCE_BGFX_OPENGLES_VERSION\s*!=\s*(\d+)", text):
        out[int(tier)] = (int(gl), int(gles))
    return out


def caps_runtime_table(text: str, name: str):
    """copy 3 -> [(maj, min, patch), ...] for gl[] / gles[]"""
    m = re.search(r"\b%s\[JCE_GRAPHICS_API_TIER_COUNT\]\s*=\s*\{(.*?)\}\s*;"
                  % re.escape(name), text, re.S)
    if not m:
        return None
    return [tuple(int(x) for x in t)
            for t in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}",
                                m.group(1))]


def shader_profiles(text: str):
    glsl = None
    row = re.search(r"^\s*set\(_profiles\s+(.+?)\)\s*$", text, re.M)
    head = re.search(r"^\s*set\(_suffixes\s+(.+?)\)\s*$", text, re.M)
    if row and head:
        cols, names = row.group(1).split(), head.group(1).split()
        if "glsl" in names and len(cols) == len(names):
            glsl = cols[names.index("glsl")]
    cg = re.search(r'_suffix\s+STREQUAL\s+"glsl"\)\s*\n\s*set\(_profile\s+"([^"]+)"', text)
    ce = re.search(r'_suffix\s+STREQUAL\s+"essl"\)\s*\n\s*set\(_profile\s+"([^"]+)"', text)
    return glsl, (cg.group(1) if cg else None), (ce.group(1) if ce else None)


def cooked_profiles(text: str):
    """{platform: [suffix, ...]} from jce_shader_profiles()'s set(_profiles ...)"""
    body = re.search(r"function\(jce_shader_profiles.*?^endfunction\(\)",
                     text, re.M | re.S)
    if not body:
        return None
    out, plat = {}, None
    for line in body.group(0).splitlines():
        m = re.search(r'_plat\s+STREQUAL\s+"(\w+)"', line)
        if m:
            plat = m.group(1)
            continue
        if re.match(r"\s*else\(\)", line):
            plat = "fallback"
            continue
        m = re.search(r"^\s*set\(_profiles\s+([\w\s]+?)\)", line)
        if m and plat:
            out[plat] = m.group(1).split()
            plat = None
    return out or None


def loadable_suffixes(text: str):
    """The suffixes shader_suffix() can actually return."""
    body = re.search(r"static const char \*shader_suffix\(.*?\n\}", text, re.S)
    if not body:
        return None
    return set(re.findall(r'return\s+"(\w+)"\s*;', body.group(0)))


def as_version(packed: int):
    """33 -> (3, 3, 0); 30 -> (3, 0, 0)"""
    return (packed // 10, packed % 10, 0)


def main() -> int:
    for p in (HOOK, CAPS, SHADERS, GRAPH):
        if not p.is_file():
            print("check_gfx_api_tiers: FAIL - expected %s to exist; the "
                  "build's shape changed and this gate no longer reads it."
                  % p.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
            return 1

    tiers = hook_tiers(HOOK.read_text(encoding="utf-8"))
    ctext = CAPS.read_text(encoding="utf-8")
    pre = caps_preprocessor(ctext)
    gl_tab = caps_runtime_table(ctext, "gl")
    gles_tab = caps_runtime_table(ctext, "gles")
    glsl, cglsl, cessl = shader_profiles(SHADERS.read_text(encoding="utf-8"))
    gm = GRAPH_GL.search(GRAPH.read_text(encoding="utf-8"))
    graph_glsl = gm.group(1) if gm else None
    cooked = cooked_profiles(HELPERS.read_text(encoding="utf-8"))
    loadable = loadable_suffixes(LOADER.read_text(encoding="utf-8"))

    if tiers is None:
        print("check_gfx_api_tiers: FAIL - could not read _GRAPHICS_TIERS out "
              "of %s." % HOOK.relative_to(REPO_ROOT).as_posix(),
              file=sys.stderr)
        return 1

    by_value = {cfg["value"]: (name, cfg) for name, cfg in tiers.items()}

    if "--list" in sys.argv:
        print("%-9s %-6s %-9s %-9s %-11s %s"
              % ("tier", "value", "hook GL", "hook GLES", "caps #if", "caps table"))
        for value in sorted(by_value):
            name, cfg = by_value[value]
            pre_s = ("%d/%d" % pre[value]) if value in pre else "MISSING"
            tab_s = "?"
            if gl_tab and gles_tab and value < len(gl_tab):
                tab_s = "%d.%d / %d.%d" % (gl_tab[value][0], gl_tab[value][1],
                                           gles_tab[value][0], gles_tab[value][1])
            print("%-9s %-6d %-9d %-9d %-11s %s"
                  % (name, value, cfg["opengl"], cfg["opengles"], pre_s, tab_s))
        print("shader profiles: glsl %s, compute %s / %s" % (glsl, cglsl, cessl))
        print("Vulkan: negotiated at runtime by bgfx; vk[] is advisory, "
              "unchecked.")
        print("loadable suffixes: %s" % ", ".join(sorted(loadable or [])))
        for platform in sorted(cooked or {}):
            print("  cook %-9s %s" % (platform, " ".join(cooked[platform])))

    ok = True

    if gl_tab is None or gles_tab is None:
        ok = False
        fail("could not read the gl[]/gles[] tables out of "
             "jce_renderer_api_tier_minimum().")
    elif len(gl_tab) != len(tiers) or len(gles_tab) != len(tiers):
        ok = False
        fail("the hook declares %d tier(s) but the runtime tables hold %d gl / "
             "%d gles entries.  A tier with no row is returned as whatever "
             "sits at that index." % (len(tiers), len(gl_tab), len(gles_tab)))

    for value in sorted(by_value):
        name, cfg = by_value[value]

        if value not in pre:
            ok = False
            fail("tier '%s' (value %d) has no `#if JCE_GRAPHICS_API_TIER_VALUE "
                 "== %d` guard in %s.  Nothing then checks that the package "
                 "built for this tier actually carries this tier's floor."
                 % (name, value, value, CAPS.relative_to(REPO_ROOT).as_posix()))
        elif pre[value] != (cfg["opengl"], cfg["opengles"]):
            ok = False
            fail("tier '%s': the hook compiles bgfx for GL %d / GLES %d, but "
                 "%s asserts GL %d / GLES %d.  One of the two is a typo and "
                 "the build will refuse to compile -- fix whichever is wrong."
                 % (name, cfg["opengl"], cfg["opengles"],
                    CAPS.relative_to(REPO_ROOT).as_posix(),
                    pre[value][0], pre[value][1]))

        if gl_tab and gles_tab and value < len(gl_tab):
            want_gl, want_gles = as_version(cfg["opengl"]), as_version(cfg["opengles"])
            if gl_tab[value] != want_gl:
                ok = False
                fail("tier '%s': bgfx is compiled with an OpenGL floor of %d "
                     "(= %d.%d), but jce_renderer_api_tier_minimum() reports "
                     "%d.%d.%d.  Nothing fails when these differ -- the engine "
                     "just tells every caller, log line and editor panel a "
                     "floor it is not built for."
                     % (name, cfg["opengl"], want_gl[0], want_gl[1],
                        gl_tab[value][0], gl_tab[value][1], gl_tab[value][2]))
            if gles_tab[value] != want_gles:
                ok = False
                fail("tier '%s': bgfx is compiled with a GLES floor of %d "
                     "(= %d.%d), but jce_renderer_api_tier_minimum() reports "
                     "%d.%d.%d."
                     % (name, cfg["opengles"], want_gles[0], want_gles[1],
                        gles_tab[value][0], gles_tab[value][1], gles_tab[value][2]))

    if glsl is None:
        ok = False
        fail("could not read the `glsl` column out of set(_profiles ...) in %s."
             % SHADERS.relative_to(REPO_ROOT).as_posix())
    elif glsl not in USABLE_GLSL_PROFILES:
        ok = False
        fail("shader profile for `glsl` is %s.  This shaderc emits %s only: "
             "<=400 goes through glsl-optimizer, which supports 1.10-1.50, so "
             "330 and 400 fail outright.  And bgfx rewrites the `#version` "
             "line at load time anyway, so a higher number here buys nothing. "
             "The lever for a newer GL is user.jce:bgfx_graphics_tier."
             % (glsl, "/".join(sorted(USABLE_GLSL_PROFILES))))

    if graph_glsl is None:
        ok = False
        fail("could not read the OpenGL row of profile_for_backend() in %s."
             % GRAPH.relative_to(REPO_ROOT).as_posix())
    elif graph_glsl not in USABLE_GLSL_PROFILES:
        ok = False
        fail("the editor's shader-graph compiler targets glsl %s in %s.  Same "
             "shaderc, same limit: it cannot emit that.  This table is a "
             "second copy of the one in %s and both have to move together."
             % (graph_glsl, GRAPH.relative_to(REPO_ROOT).as_posix(),
                SHADERS.relative_to(REPO_ROOT).as_posix()))
    elif glsl is not None and graph_glsl != glsl:
        ok = False
        fail("the engine cooks OpenGL shaders at glsl %s but the editor's "
             "shader graph compiles them at %s.  A graph material would then "
             "be built against a different dialect than everything it renders "
             "beside." % (glsl, graph_glsl))

    if cooked is None or loadable is None:
        ok = False
        fail("could not read jce_shader_profiles() from %s or shader_suffix() "
             "from %s."
             % (HELPERS.relative_to(REPO_ROOT).as_posix(),
                LOADER.relative_to(REPO_ROOT).as_posix()))
    else:
        for platform in sorted(cooked):
            for suffix in cooked[platform]:
                if suffix in loadable:
                    continue
                ok = False
                fail("the %s cook list builds `%s` shaders, but shader_suffix()"
                     " in %s never returns that suffix for any renderer -- so "
                     "those binaries are compiled, packed into the .pak, "
                     "shipped, and can never be loaded.  Either add the pick to "
                     "the loader or drop the profile from the list."
                     % (platform, suffix,
                        LOADER.relative_to(REPO_ROOT).as_posix()))

    if cglsl != REQUIRED_COMPUTE_GLSL:
        ok = False
        fail("compute shaders must be built at glsl %s (bgfx keeps that "
             "`#version` verbatim; anything else is emitted, fails "
             "glCompileShader at runtime, and every dispatch silently becomes "
             "a no-op).  Found: %s" % (REQUIRED_COMPUTE_GLSL, cglsl))
    if cessl != REQUIRED_COMPUTE_ESSL:
        ok = False
        fail("compute shaders must be built at essl %s.  Found: %s"
             % (REQUIRED_COMPUTE_ESSL, cessl))

    if not ok:
        print("check_gfx_api_tiers: FAILED", file=sys.stderr)
        return 1

    print("check_gfx_api_tiers: OK (%d tier(s) -- %s -- agree across the conan "
          "hook, the caps preprocessor guards and the runtime table; shader "
          "profiles glsl %s (shader graph %s), compute %s / %s)"
          % (len(tiers), ", ".join(n for _, (n, _c) in sorted(by_value.items())),
             glsl, graph_glsl, cglsl, cessl))
    return 0


if __name__ == "__main__":
    sys.exit(main())

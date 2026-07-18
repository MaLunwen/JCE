"""
Conan 2 hook: normalise bgfx/1.129.8930-495's conan_cmake_project_include.cmake.

Two issues patched:

1. UNIX AND NOT APPLE guard is TRUE for Emscripten (CMake sets UNIX=TRUE for
   wasm32 targets), causing find_package(wayland REQUIRED CONFIG) to fail.
   Fix: replace with an explicit CMAKE_SYSTEM_NAME check.

2. bgfx's multi-threaded render loop uses bx::Thread, which is unavailable on
   Emscripten without full pthreads — but it is REQUIRED everywhere else (it is
   what gives bgfx >1 encoder, i.e. multi-threaded draw submission). bgfx's own
   config.h already defaults BGFX_CONFIG_MULTITHREADED to 1 off Emscripten, so
   the ONLY reason to touch it is to force it OFF for wasm.

   CRITICAL: post_source patches the recipe's SHARED source folder, so a setting
   written here for a wasm build is also read by every later non-wasm build that
   reuses the same cached source. A previous version wrote an UNCONDITIONAL
   `set(BGFX_CONFIG_MULTITHREADED 0 ... FORCE)`, which leaked into the Windows /
   Linux / macOS builds and silently forced them single-threaded (maxEncoders=1,
   so JCE_PARALLEL_SUBMIT had no encoders and the editor ran the whole renderer
   on one thread). We now write a PLATFORM-CONDITIONAL block instead: 0 on
   Emscripten, 1 everywhere else — correct regardless of which platform built
   the shared source first. Run on every platform so the file is always
   normalised (and any stale unconditional line is replaced).
"""

import os
import re

_MT_BLOCK = (
    'if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")\n'
    '    set(BGFX_CONFIG_MULTITHREADED 0 CACHE STRING "Emscripten: no render thread (no pthreads)" FORCE)\n'
    'else()\n'
    '    set(BGFX_CONFIG_MULTITHREADED 1 CACHE STRING "Enable bgfx render thread (multi-encoder submit)" FORCE)\n'
    'endif()\n'
)

# 3. Matrix-cache headroom. bgfx's per-frame transform cache saturates SILENTLY
#    at BGFX_CONFIG_MAX_MATRIX_CACHE (default MAX_DRAW_CALLS+1 = 65536): once a
#    frame submits more set_transform matrices, later draws get clamped/garbage
#    transforms and disappear. Measured: a per-char skinned crowd demands
#    ~104k matrices/frame at 1000 chars (bone palettes ~24-128 each x passes x
#    editor viewports) and stops color-rendering around ~650. Doubling to
#    131072 costs +64B x 65536 x 2 frames = +8MB and moves the per-char wall to
#    ~1300 chars; the structural fix at scale stays the crowd-instancing paths
#    (JCE_CROWD_BINDPOSE/_INSTANCE, ~1.5k matrices at 1000 chars). Emscripten
#    keeps the smaller default (wasm memory budget). The constant is private to
#    bgfx (src/config.h -> bgfx_p.h MatrixCache) so no consumer ABI impact —
#    but keep the engine-side pressure warning (jce_renderer.c end_frame,
#    JCE_BGFX_MATRIX_CACHE_CAP) in sync with this value.
#    Not in bgfx.cmake's BGFX_CONFIG_OPTIONS forward list, so a cache var would
#    be ignored — inject a compile definition instead (config.h only reads the
#    macro under #ifndef, and defining it on bx/bimg too is harmless).
_MC_BLOCK = (
    'if(NOT (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten"))\n'
    '    add_compile_definitions(BGFX_CONFIG_MAX_MATRIX_CACHE=131072)\n'
    'endif()\n'
)


def post_source(conanfile):
    if conanfile.name != "bgfx":
        return

    cmake_file = os.path.join(conanfile.source_folder, "conan_cmake_project_include.cmake")
    if not os.path.exists(cmake_file):
        conanfile.output.warning("[bgfx_fix hook] conan_cmake_project_include.cmake not found")
        return

    with open(cmake_file, "r") as f:
        content = f.read()

    # Fix 1: wayland guard — UNIX is TRUE for Emscripten but wayland is Linux-only.
    patched = content.replace(
        "if(UNIX AND NOT APPLE)",
        'if(CMAKE_SYSTEM_NAME STREQUAL "Linux")',
    )

    # Fix 2+3: strip any prior MULTITHREADED / MATRIX_CACHE setting (stale
    # unconditional lines or earlier conditional blocks — including the
    # negated matrix-cache guard), then append the canonical blocks.
    patched = re.sub(
        r'\n?if\((?:NOT \()?EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten"\)?\).*?endif\(\)\n?',
        '\n', patched, flags=re.DOTALL,
    )
    patched = re.sub(
        r'\n?set\(BGFX_CONFIG_MULTITHREADED[^\n]*\)\n?',
        '\n', patched,
    )
    patched = re.sub(
        r'\n?add_compile_definitions\(BGFX_CONFIG_MAX_MATRIX_CACHE[^\n]*\)\n?',
        '\n', patched,
    )
    if not patched.endswith("\n"):
        patched += "\n"
    patched += _MT_BLOCK
    patched += _MC_BLOCK

    if content != patched:
        with open(cmake_file, "w") as f:
            f.write(patched)
        conanfile.output.info(
            "[bgfx_fix hook] normalised conan_cmake_project_include.cmake "
            "(wayland guard + platform-conditional BGFX_CONFIG_MULTITHREADED)"
        )

    _patch_d3d12_pso_guards(conanfile)
    _patch_gl_compute_barrier(conanfile)


# ── D3D12 pipeline-state-creation robustness (P0 crash fix, 2026-07-02) ──
# bgfx's D3D12 backend derefs the pipeline-state object returned by
# getPipelineState WITHOUT surviving a creation failure gracefully:
#   * compute getPipelineState: CreateComputePipelineState via DX_CHECK
#     (compiled out in release) -> caches + derefs a NULL PSO -> AV.
#   * graphics getPipelineState: has BGFX_FATAL(NULL!=pso) which, once our
#     fatal callback returns, aborts the process.
# Both fire when a scene switch brings up many GPU-particle systems on D3D12
# and a PSO creation fails (minidump: cs_particle_update compute PSO; also
# the particle-render graphics PSO).  The engine-side fixes (particle pool
# warmup + staggered creation) remove the usual trigger, but this makes the
# backend DEGRADE (skip the offending dispatch/draw) instead of crashing —
# a robust catch-all for any residual D3D12 PSO-create failure.  Idempotent;
# each replacement is anchored and warns on bgfx version drift.
_PSO_PATCHES = [
    # (name, old_anchor, already_applied_marker, new, expected_count)
    ("compute-create",
     "\t\t\tif (NULL == pso)\n\t\t\t{\n"
     "\t\t\t\tDX_CHECK(m_device->CreateComputePipelineState(&desc\n"
     "\t\t\t\t\t, IID_ID3D12PipelineState\n"
     "\t\t\t\t\t, (void**)&pso\n"
     "\t\t\t\t\t) );\n\t\t\t}\n",
     "CreateComputePipelineState failed",
     "\t\t\tif (NULL == pso)\n\t\t\t{\n"
     "\t\t\t\t/* JCE patch: survive compute-PSO creation failure. */\n"
     "\t\t\t\tHRESULT jceHr = m_device->CreateComputePipelineState(&desc\n"
     "\t\t\t\t\t, IID_ID3D12PipelineState, (void**)&pso);\n"
     "\t\t\t\tif (FAILED(jceHr) || NULL == pso) {\n"
     "\t\t\t\t\tBX_TRACE(\"CreateComputePipelineState failed (0x%08x).\", jceHr);\n"
     "\t\t\t\t\treturn NULL;\n\t\t\t\t}\n\t\t\t}\n", 1),
    # graphics: replace the fatal with a soft NULL return (the cache add +
    # blob read that follow are skipped by the early return).
    ("graphics-fatal",
     "\t\t\tBGFX_FATAL(NULL != pso, Fatal::InvalidShader, \"Failed to create PSO!\");\n",
     "JCE: graphics PSO create failed",
     "\t\t\tif (NULL == pso) { BX_TRACE(\"JCE: graphics PSO create failed - skipping draw.\"); return NULL; }\n", 1),
    # compute dispatch site: skip on NULL pso.
    ("compute-use",
     "\t\t\t\t\tID3D12PipelineState* pso = getPipelineState(key.m_program);\n"
     "\t\t\t\t\tif (pso != currentPso)\n",
     "compute-PSO creation failed",
     "\t\t\t\t\tID3D12PipelineState* pso = getPipelineState(key.m_program);\n"
     "\t\t\t\t\tif (NULL == pso) { continue; } /* JCE: skip on PSO fail */\n"
     "\t\t\t\t\tif (pso != currentPso)\n", 1),
    # graphics draw site: skip on NULL pso.
    ("graphics-use",
     "\t\t\t\t\tID3D12PipelineState* pso = getPipelineState(\n"
     "\t\t\t\t\t\t  state\n"
     "\t\t\t\t\t\t, draw.m_stencil\n"
     "\t\t\t\t\t\t, numStreams\n"
     "\t\t\t\t\t\t, layouts\n"
     "\t\t\t\t\t\t, key.m_program\n"
     "\t\t\t\t\t\t, uint8_t(draw.m_instanceDataStride/16)\n"
     "\t\t\t\t\t\t);\n",
     "JCE: skip draw on PSO fail",
     "\t\t\t\t\tID3D12PipelineState* pso = getPipelineState(\n"
     "\t\t\t\t\t\t  state\n"
     "\t\t\t\t\t\t, draw.m_stencil\n"
     "\t\t\t\t\t\t, numStreams\n"
     "\t\t\t\t\t\t, layouts\n"
     "\t\t\t\t\t\t, key.m_program\n"
     "\t\t\t\t\t\t, uint8_t(draw.m_instanceDataStride/16)\n"
     "\t\t\t\t\t\t);\n"
     "\t\t\t\t\tif (NULL == pso) { continue; } /* JCE: skip draw on PSO fail */\n", 1),
]


# ── GL compute→draw visibility barrier (GPU-driven cull fix, 2026-07-03) ──
# bgfx's GL backend issues only GL_SHADER_STORAGE_BARRIER_BIT after a compute
# dispatch (accumulated from the buffer bind types).  That makes SSBO writes
# visible to LATER SHADER reads, but NOT to the fixed-function client paths a
# GPU-driven renderer feeds next in the SAME frame:
#   * vertex/instance attribute fetch  (compacted survivors as instance data)
#     -> needs GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT
#   * glDraw*Indirect argument fetch   (compute-built indirect args)
#     -> needs GL_COMMAND_BARRIER_BIT
#   * index fetch from compute-written index buffers
#     -> needs GL_ELEMENT_ARRAY_BARRIER_BIT
# Symptom (JCE foliage GPU cull): compute compacts survivors + builds the
# drawIndexedIndirect args, the same-frame indirect instanced draw reads STALE
# ZEROS -> the whole scatter renders empty on OpenGL while D3D11 (automatic
# hazard tracking) is correct.  GPU particles dodge it only because their pool
# is consumed one frame later (swap-boundary sync).  Fix: widen the barrier
# after the dispatch.  Idempotent; anchored on the (unique) dispatch line.
_GL_BARRIER_OLD = (
    "\t\t\t\t\t\t\t\tGL_CHECK(glDispatchCompute(compute.m_numX, compute.m_numY, compute.m_numZ) );\n"
    "\t\t\t\t\t\t\t}\n"
    "\n"
    "\t\t\t\t\t\t\tGL_CHECK(glMemoryBarrier(barrier) );\n"
)
_GL_BARRIER_MARKER = "JCE: compute->draw client-path visibility"
_GL_BARRIER_NEW = (
    "\t\t\t\t\t\t\t\tGL_CHECK(glDispatchCompute(compute.m_numX, compute.m_numY, compute.m_numZ) );\n"
    "\t\t\t\t\t\t\t}\n"
    "\n"
    "\t\t\t\t\t\t\t/* JCE: compute->draw client-path visibility — compute-written\n"
    "\t\t\t\t\t\t\t   buffers are consumed as instance/vertex attributes and\n"
    "\t\t\t\t\t\t\t   draw-indirect args in the SAME frame. */\n"
    "\t\t\t\t\t\t\tbarrier |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT\n"
    "\t\t\t\t\t\t\t        |  GL_ELEMENT_ARRAY_BARRIER_BIT\n"
    "\t\t\t\t\t\t\t        |  GL_COMMAND_BARRIER_BIT;\n"
    "\t\t\t\t\t\t\tGL_CHECK(glMemoryBarrier(barrier) );\n"
)


def _patch_gl_compute_barrier(conanfile):
    src = os.path.join(conanfile.source_folder, "bgfx", "src",
                       "renderer_gl.cpp")
    if not os.path.exists(src):
        conanfile.output.warning("[bgfx_fix hook] renderer_gl.cpp not found")
        return
    with open(src, "r", newline="") as f:
        content = f.read()
    if _GL_BARRIER_MARKER in content:
        conanfile.output.info("[bgfx_fix hook] GL compute barrier: already present")
        return
    n = content.count(_GL_BARRIER_OLD)
    if n != 1:
        conanfile.output.warning(
            "[bgfx_fix hook] GL compute-barrier anchor not found (%d != 1) — "
            "bgfx drift, patch skipped" % n)
        return
    content = content.replace(_GL_BARRIER_OLD, _GL_BARRIER_NEW)
    with open(src, "w", newline="") as f:
        f.write(content)
    conanfile.output.info("[bgfx_fix hook] GL compute barrier: applied")


def _patch_d3d12_pso_guards(conanfile):
    src = os.path.join(conanfile.source_folder, "bgfx", "src",
                       "renderer_d3d12.cpp")
    if not os.path.exists(src):
        conanfile.output.warning("[bgfx_fix hook] renderer_d3d12.cpp not found")
        return
    with open(src, "r", newline="") as f:
        content = f.read()
    # Apply each patch INDEPENDENTLY: a prior partial application (e.g. an
    # earlier compute-only patch left in the cached source) must not block the
    # remaining guards.  Skip a patch whose distinctive marker is already
    # present; apply when its exact anchor matches; warn (but continue) on a
    # genuine anchor miss (bgfx version drift).
    applied, skipped = 0, 0
    for name, old, marker, new, want in _PSO_PATCHES:
        if marker in content:
            skipped += 1
            continue
        if content.count(old) == want:
            content = content.replace(old, new)
            applied += 1
        else:
            conanfile.output.warning(
                "[bgfx_fix hook] d3d12 PSO anchor '%s' not found (%d != %d) — "
                "bgfx drift, that guard skipped" % (name, content.count(old), want))
    if applied:
        with open(src, "w", newline="") as f:
            f.write(content)
    conanfile.output.info(
        "[bgfx_fix hook] d3d12 PSO guards: %d applied, %d already present"
        % (applied, skipped))

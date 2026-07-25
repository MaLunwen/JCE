"""
Conan 2 hook: normalise bgfx's CMake sources for JCE's target matrix.

Three cross-platform issues patched:

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

3. bx enables ``-msse4.2`` for every non-MSVC compiler. Emscripten 5 rejects
   x86 SIMD flags unless ``-msimd128`` is also present. JCE's Web baseline is
   wasm SIMD128, so select ``-msimd128`` on Emscripten and retain ``-msse4.2``
   on native non-MSVC targets.
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

_BX_SIMD_OLD = (
    "\ttarget_compile_options(bx PUBLIC "
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-msse4.2>)"
)
_BX_SIMD_MARKER = "JCE: Emscripten SIMD128 baseline"
_BX_SIMD_NEW = (
    'if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")\n'
    '    # JCE: Emscripten SIMD128 baseline; do not pass x86 ISA flags.\n'
    '    target_compile_options(bx PUBLIC -msimd128)\n'
    'else()\n'
    '    target_compile_options(bx PUBLIC '
    '$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-msse4.2>)\n'
    'endif()'
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
    _patch_d3d12_resource_guards(conanfile)
    _patch_gl_compute_barrier(conanfile)
    _patch_bx_wasm_simd(conanfile)


def _patch_wasm_simd_flags_text(content):
    if _BX_SIMD_MARKER in content:
        return content, 0
    if content.count(_BX_SIMD_OLD) != 1:
        return content, 0
    return content.replace(_BX_SIMD_OLD, _BX_SIMD_NEW), 1


def _patch_bx_wasm_simd(conanfile):
    cmake_file = os.path.join(conanfile.source_folder, "cmake", "bx", "bx.cmake")
    if not os.path.exists(cmake_file):
        conanfile.output.warning("[bgfx_fix hook] cmake/bx/bx.cmake not found")
        return

    with open(cmake_file, "r", newline="") as f:
        content = f.read()
    patched, applied = _patch_wasm_simd_flags_text(content)
    if patched != content:
        with open(cmake_file, "w", newline="") as f:
            f.write(patched)
    if applied:
        conanfile.output.info("[bgfx_fix hook] bx wasm SIMD128 flags: applied")
    elif _BX_SIMD_MARKER in patched:
        conanfile.output.info("[bgfx_fix hook] bx wasm SIMD128 flags: already present")
    else:
        conanfile.output.warning(
            "[bgfx_fix hook] bx SIMD anchor not found - bgfx drift, patch skipped"
        )


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
     "\t\t\t\tDX_CHECK(m_device->CreateComputePipelineState(\n"
     "\t\t\t\t\t  &desc\n"
     "\t\t\t\t\t, IID_ID3D12PipelineState\n"
     "\t\t\t\t\t, (void**)&pso\n"
     "\t\t\t\t\t) );\n\t\t\t}\n",
     "JCE: compute PSO create failed",
     "\t\t\tif (NULL == pso)\n\t\t\t{\n"
     "\t\t\t\tHRESULT jceHr = m_device->CreateComputePipelineState(\n"
     "\t\t\t\t\t  &desc\n"
     "\t\t\t\t\t, IID_ID3D12PipelineState\n"
     "\t\t\t\t\t, (void**)&pso\n"
     "\t\t\t\t\t);\n"
     "\t\t\t\tif (FAILED(jceHr) || NULL == pso)\n"
     "\t\t\t\t{\n"
     "\t\t\t\t\tBX_TRACE(\"JCE: compute PSO create failed (0x%08x).\", uint32_t(jceHr) );\n"
     "\t\t\t\t\tif (NULL != cachedData) { bx::free(g_allocator, cachedData); }\n"
     "\t\t\t\t\treturn NULL;\n\t\t\t\t}\n\t\t\t}\n", 1),
    # graphics: replace the fatal with a soft NULL return (the cache add +
    # blob read that follow are skipped by the early return).
    ("graphics-create",
     "\t\t\tBGFX_FATAL(NULL != pso, Fatal::InvalidShader, \"Failed to create PSO!\");\n",
     "JCE: graphics PSO create failed; draw skipped",
     "\t\t\tif (NULL == pso)\n"
     "\t\t\t{\n"
     "\t\t\t\tBX_TRACE(\"JCE: graphics PSO create failed; draw skipped.\");\n"
     "\t\t\t\tif (NULL != temp) { release(temp); }\n"
     "\t\t\t\tif (NULL != cachedData) { bx::free(g_allocator, cachedData); }\n"
     "\t\t\t\treturn NULL;\n"
     "\t\t\t}\n", 1),
    # compute dispatch site: skip on NULL pso.
    ("compute-use",
     "\t\t\t\t\tID3D12PipelineState* pso = getPipelineState(key.m_program);\n"
     "\t\t\t\t\tif (pso != currentPso)\n",
     "JCE: skip compute on PSO fail",
     "\t\t\t\t\tID3D12PipelineState* pso = getPipelineState(key.m_program);\n"
     "\t\t\t\t\tif (NULL == pso) { continue; } /* JCE: skip compute on PSO fail */\n"
     "\t\t\t\t\tif (pso != currentPso)\n", 1),
    # graphics draw site: skip on NULL pso.
    ("graphics-use",
     "\t\t\t\t\tID3D12PipelineState* pso = getPipelineState(\n"
     "\t\t\t\t\t\t  state\n"
     "\t\t\t\t\t\t, draw.m_rgba\n"
     "\t\t\t\t\t\t, draw.m_stencil\n"
     "\t\t\t\t\t\t, numStreams\n"
     "\t\t\t\t\t\t, layouts\n"
     "\t\t\t\t\t\t, key.m_program\n"
     "\t\t\t\t\t\t, uint8_t(draw.m_instanceDataStride/16)\n"
     "\t\t\t\t\t\t);\n",
     "JCE: skip draw on PSO fail",
     "\t\t\t\t\tID3D12PipelineState* pso = getPipelineState(\n"
     "\t\t\t\t\t\t  state\n"
     "\t\t\t\t\t\t, draw.m_rgba\n"
     "\t\t\t\t\t\t, draw.m_stencil\n"
     "\t\t\t\t\t\t, numStreams\n"
     "\t\t\t\t\t\t, layouts\n"
     "\t\t\t\t\t\t, key.m_program\n"
     "\t\t\t\t\t\t, uint8_t(draw.m_instanceDataStride/16)\n"
     "\t\t\t\t\t\t);\n"
     "\t\t\t\t\tif (NULL == pso) { continue; } /* JCE: skip draw on PSO fail */\n", 1),
    # Debug text and mip generation also bind dynamically-created PSOs outside
    # the main draw/dispatch paths.
    ("debug-blit-use",
     "\t\t\t\t);\n"
     "\t\t\tm_commandList->SetPipelineState(pso);\n"
     "\t\t\tm_commandList->SetGraphicsRootSignature(m_rootSignature);\n",
     "JCE: skip debug blit on PSO fail",
     "\t\t\t\t);\n"
     "\t\t\tif (NULL == pso) { return; } /* JCE: skip debug blit on PSO fail */\n"
     "\t\t\tm_commandList->SetPipelineState(pso);\n"
     "\t\t\tm_commandList->SetGraphicsRootSignature(m_rootSignature);\n", 1),
    ("mipgen-use",
     "\t\t\t_commandList->SetPipelineState(getPipelineState(prog) );\n",
     "JCE: skip mip generation on PSO fail",
     "\t\t\tID3D12PipelineState* pso = getPipelineState(prog);\n"
     "\t\t\tif (NULL == pso) { break; } /* JCE: skip mip generation on PSO fail */\n"
     "\t\t\t_commandList->SetPipelineState(pso);\n", 1),
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


def _d3d12_pso_patch_candidates(name, old, new):
    candidates = [(old, new)]
    if name == "compute-create":
        candidates.append((
            old.replace(
                "CreateComputePipelineState(\n\t\t\t\t\t  &desc",
                "CreateComputePipelineState(&desc",
            ),
            new,
        ))
    elif name == "graphics-create":
        candidates.append((
            "\t\t\tif (NULL == pso) { BX_TRACE(\"JCE: graphics PSO "
            "create failed - skipping draw.\"); return NULL; }\n",
            new,
        ))
    elif name == "compute-use":
        candidates.append((
            old.replace(
                "if (pso != currentPso)",
                "if (NULL == pso) { continue; } /* JCE: skip on PSO fail */\n"
                "\t\t\t\t\tif (pso != currentPso)",
            ),
            new,
        ))
    elif name == "graphics-use":
        rgba = "\t\t\t\t\t\t, draw.m_rgba\n"
        candidates.append((old.replace(rgba, ""), new.replace(rgba, "")))
    return candidates


def _patch_d3d12_pso_guards_text(content):
    applied = 0
    for name, old, marker, new, want in _PSO_PATCHES:
        if marker in content:
            continue
        for candidate_old, candidate_new in _d3d12_pso_patch_candidates(
            name, old, new
        ):
            if content.count(candidate_old) == want:
                content = content.replace(candidate_old, candidate_new)
                applied += 1
                break
    return content, applied


def _patch_d3d12_pso_guards(conanfile):
    src = os.path.join(conanfile.source_folder, "bgfx", "src",
                       "renderer_d3d12.cpp")
    if not os.path.exists(src):
        conanfile.output.warning("[bgfx_fix hook] renderer_d3d12.cpp not found")
        return
    with open(src, "r", newline="") as f:
        content = f.read()
    patched, applied = _patch_d3d12_pso_guards_text(content)
    missing = [
        name for name, _, marker, _, _ in _PSO_PATCHES
        if marker not in patched
    ]
    for name in missing:
        conanfile.output.warning(
            "[bgfx_fix hook] d3d12 PSO anchor '%s' not found - bgfx drift, "
            "that guard skipped" % name
        )
    if patched != content:
        with open(src, "w", newline="") as f:
            f.write(patched)
    present = len(_PSO_PATCHES) - len(missing)
    conanfile.output.info(
        "[bgfx_fix hook] d3d12 PSO guards: %d applied, %d/%d present"
        % (applied, present, len(_PSO_PATCHES)))


# ── D3D12 resource-creation failure diagnostics (P0 crash fix, 2026-07-19) ──
# bgfx release builds compile DX_CHECK/BX_WARN out.  createCommittedResource
# therefore returned an uninitialised/null pointer after a failed D3D12 call,
# and BufferD3D12 immediately dereferenced it in GetGPUVirtualAddress or Map.
# Keep the backend alive long enough to report both the original HRESULT and
# GetDeviceRemovedReason through bgfx's callback, then skip the failed upload.
_D3D12_RESOURCE_PATCHES = [
    ("committed-resource-result",
     "\t\tID3D12Resource* resource;\n"
     "\t\tDX_CHECK(_device->CreateCommittedResource(&heapProperty.m_properties\n"
     "\t\t\t, D3D12_HEAP_FLAG_NONE\n"
     "\t\t\t, _resourceDesc\n"
     "\t\t\t, heapProperty.m_state\n"
     "\t\t\t, _clearValue\n"
     "\t\t\t, IID_ID3D12Resource\n"
     "\t\t\t, (void**)&resource\n"
     "\t\t\t) );\n",
     "JCE D3D12 CreateCommittedResource failed",
     "\t\tID3D12Resource* resource = NULL;\n"
     "\t\tHRESULT jceHr = _device->CreateCommittedResource(&heapProperty.m_properties\n"
     "\t\t\t, D3D12_HEAP_FLAG_NONE\n"
     "\t\t\t, _resourceDesc\n"
     "\t\t\t, heapProperty.m_state\n"
     "\t\t\t, _clearValue\n"
     "\t\t\t, IID_ID3D12Resource\n"
     "\t\t\t, (void**)&resource\n"
     "\t\t\t);\n"
     "\t\tif (FAILED(jceHr) || NULL == resource)\n"
     "\t\t{\n"
     "\t\t\tHRESULT jceRemoved = _device->GetDeviceRemovedReason();\n"
     "\t\t\tbgfx::trace(__FILE__, uint16_t(__LINE__),\n"
     "\t\t\t\t\"JCE D3D12 CreateCommittedResource failed: hr=0x%08x \"\n"
     "\t\t\t\t\"removed=0x%08x heap=%u size=%llu\\n\",\n"
     "\t\t\t\tuint32_t(jceHr), uint32_t(jceRemoved), uint32_t(_heapProperty),\n"
     "\t\t\t\t(unsigned long long)_resourceDesc->Width);\n"
     "\t\t\treturn NULL;\n"
     "\t\t}\n"),
    ("buffer-create-null",
     "\t\tm_ptr   = createCommittedResource(device, HeapProperty::Default, _size, D3D12_RESOURCE_FLAGS(flags) );\n"
     "\t\tm_gpuVA = m_ptr->GetGPUVirtualAddress();\n",
     "JCE D3D12 buffer allocation failed",
     "\t\tm_ptr   = createCommittedResource(device, HeapProperty::Default, _size, D3D12_RESOURCE_FLAGS(flags) );\n"
     "\t\tif (NULL == m_ptr)\n"
     "\t\t{\n"
     "\t\t\tbgfx::trace(__FILE__, uint16_t(__LINE__),\n"
     "\t\t\t\t\"JCE D3D12 buffer allocation failed: size=%u\\n\", _size);\n"
     "\t\t\tm_gpuVA = 0;\n"
     "\t\t\treturn;\n"
     "\t\t}\n"
     "\t\tm_gpuVA = m_ptr->GetGPUVirtualAddress();\n"),
    ("buffer-update-null",
     "\t\tID3D12Resource* staging = createCommittedResource(s_renderD3D12->m_device, HeapProperty::Upload, _size);\n"
     "\t\tuint8_t* data;\n",
     "JCE D3D12 staging allocation failed",
     "\t\tif (NULL == m_ptr)\n"
     "\t\t{\n"
     "\t\t\tbgfx::trace(__FILE__, uint16_t(__LINE__),\n"
     "\t\t\t\t\"JCE D3D12 buffer update skipped: destination is null\\n\");\n"
     "\t\t\treturn;\n"
     "\t\t}\n"
     "\t\tID3D12Resource* staging = createCommittedResource(s_renderD3D12->m_device, HeapProperty::Upload, _size);\n"
     "\t\tif (NULL == staging)\n"
     "\t\t{\n"
     "\t\t\tbgfx::trace(__FILE__, uint16_t(__LINE__),\n"
     "\t\t\t\t\"JCE D3D12 staging allocation failed: size=%u\\n\", _size);\n"
     "\t\t\treturn;\n"
     "\t\t}\n"
     "\t\tuint8_t* data;\n"),
]

_D3D12_RESOURCE_TEST_SOURCE = "\n".join(
    old for _, old, _, _ in _D3D12_RESOURCE_PATCHES
)


def _patch_d3d12_resource_guards_text(content):
    applied = 0
    for _, old, marker, new in _D3D12_RESOURCE_PATCHES:
        if marker in content:
            continue
        candidates = [(old, new)]
        if "D3D12_HEAP_FLAG_NONE" in old:
            candidates.append((
                old.replace("D3D12_HEAP_FLAG_NONE", "_heapFlags"),
                new.replace("D3D12_HEAP_FLAG_NONE", "_heapFlags"),
            ))
        for candidate_old, candidate_new in candidates:
            if content.count(candidate_old) == 1:
                content = content.replace(candidate_old, candidate_new)
                applied += 1
                break
    return content, applied


def _patch_d3d12_resource_guards(conanfile):
    src = os.path.join(conanfile.source_folder, "bgfx", "src",
                       "renderer_d3d12.cpp")
    if not os.path.exists(src):
        conanfile.output.warning("[bgfx_fix hook] renderer_d3d12.cpp not found")
        return
    with open(src, "r", newline="") as f:
        content = f.read()

    patched, applied = _patch_d3d12_resource_guards_text(content)
    missing = []
    for name, old, marker, _ in _D3D12_RESOURCE_PATCHES:
        if marker not in patched and old not in content:
            missing.append(name)
    if missing:
        conanfile.output.warning(
            "[bgfx_fix hook] d3d12 resource anchors missing: %s — bgfx drift"
            % ", ".join(missing))
    if patched != content:
        with open(src, "w", newline="") as f:
            f.write(patched)
    conanfile.output.info(
        "[bgfx_fix hook] d3d12 resource guards: %d applied" % applied)

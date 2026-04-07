"""
Conan 2 hook: fix minicoro/0.1.3 Emscripten build.

minicoro.h wraps its entire implementation section in `extern "C" { ... }`
(lines 309-311) when compiled as C++.  On Emscripten, the FIBERS backend
includes <emscripten/fiber.h> which transitively pulls in <emscripten/em_asm.h>.
Since Emscripten 3.x, em_asm.h uses C++ template specializations that are
illegal inside an `extern "C"` block, causing:

    error: templates must have C++ linkage

Fix: surround the `#include <emscripten/fiber.h>` line with a temporary
extern "C" guard close/reopen so C++ templates in em_asm.h are valid.

This fires on `post_source` (before packaging), ensuring fresh installs are
patched.  For already-cached packages, patch minicoro.h in-place and delete
the behaviortree.cpp binary cache entry to trigger a rebuild.
"""

import os


_NEEDLE = "#include <emscripten/fiber.h>"

_REPLACEMENT = """\
/* hook_minicoro_wasm_fix: temporarily close extern "C" guard before
   <emscripten/fiber.h> because em_asm.h (Emscripten 3.x) uses C++ template
   specializations that are illegal inside an extern "C" block. */
#ifdef __cplusplus
} /* end extern "C" */
#endif
#include <emscripten/fiber.h>
#ifdef __cplusplus
extern "C" {
#endif"""


def _patch_file(path, conanfile):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    if "hook_minicoro_wasm_fix" in content:
        conanfile.output.info("[minicoro_wasm_fix] already patched: %s" % path)
        return
    if _NEEDLE not in content:
        conanfile.output.warning("[minicoro_wasm_fix] needle not found in: %s" % path)
        return
    patched = content.replace(_NEEDLE, _REPLACEMENT, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(patched)
    conanfile.output.info("[minicoro_wasm_fix] patched: %s" % path)


def post_source(conanfile):
    """Patch after source download, before package creation."""
    if conanfile.name != "minicoro":
        return

    # Try the standard include layout first, then the source root
    candidates = [
        os.path.join(conanfile.source_folder, "include", "minicoro", "minicoro.h"),
        os.path.join(conanfile.source_folder, "minicoro.h"),
    ]
    for header in candidates:
        if os.path.exists(header):
            _patch_file(header, conanfile)
            return

    conanfile.output.warning("[minicoro_wasm_fix] minicoro.h not found under %s" %
                             conanfile.source_folder)

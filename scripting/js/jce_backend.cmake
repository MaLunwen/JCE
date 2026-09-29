# jce_backend.cmake — js's entry in the scripting roster.
#
# DISCOVERED, not listed anywhere.  scripting/cmake/JCEScriptEnable.cmake globs
# scripting/*/jce_backend.cmake (in-tree) or lib/cmake/JCE/backends/*/ (from an
# installed SDK), so the BUILD layer needs no central edit.
#
# THAT IS THE BUILD LAYER ONLY.  A second, independent seam --
# tools/scriptgen/scriptgen_core.py's discover_backends() globbing
# tools/scriptgen/emit_*.py -- generates a language's BINDINGS to the engine.
# js is wired into BOTH seams: tools/scriptgen/emit_js.py emits the `jce`
# object a .jcejs script calls the engine through.  Adding a seventh
# language means adding a directory HERE and an emit_*.py THERE -- two
# edits, neither of them to a central list.

set(JCE_BACKEND_LANGUAGE   "js")
set(JCE_BACKEND_VM_TARGET  "jce_script_vm_js")

# .jcejs, NOT .js.  editor/src/core/jce_assetdb.cpp already classifies .js and
# .ts as project-side WEB TOOLING the engine does not execute; claiming .js
# would offer every build script in a project as an attachable gameplay script
# and ship it as readable source.  Same reason .jcecpp and .jcec exist.
set(JCE_BACKEND_EXTENSIONS "jcejs")

# A .jcejs script reaches the engine through the host vtable, the way lua does,
# not through the c_abi shared library.  NEEDS_SCRIPT_API only decides whether
# JCE_SCRIPT_API is pointed beside the executable; nothing here loads it.
set(JCE_BACKEND_NEEDS_SCRIPT_API OFF)

# Vendored: no runtime input the backend cannot discover, so nothing to name
# and nothing to warn about.
set(JCE_BACKEND_REGISTER_INCLUDE "#include <jce/script_vm/jce_script_vm_js.h>")
set(JCE_BACKEND_REGISTER_FRAGMENT "register.c.in")

# Not a "simple" SDK import: the VM archive links quickjs-ng, which a consumer
# resolves through its OWN conan graph rather than from the SDK tree, so the
# imported target needs a find_dependency and not just an archive path.  A
# hand-written block in JCEScripting.cmake.in declares it.
set(JCE_BACKEND_SDK_IMPORT_SIMPLE OFF)

# quickjs-ng is installed with the SDK and linked by ScriptVmJs.  Include it
# in the shipped-language contract so a stale VM archive cannot make the SDK
# report JavaScript support after qjs was pruned.
set(JCE_BACKEND_SDK_REQUIRES_LINK "qjs")

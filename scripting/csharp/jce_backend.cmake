# jce_backend.cmake — csharp's entry in the scripting roster.
#
# DISCOVERED, not listed anywhere.  scripting/cmake/JCEScriptEnable.cmake globs
# scripting/*/jce_backend.cmake (in-tree) or lib/cmake/JCE/backends/*/ (from an
# installed SDK), so the BUILD layer needs no central edit.
#
# THAT IS THE BUILD LAYER ONLY.  A second, independent seam --
# tools/scriptgen/scriptgen_core.py's discover_backends() globbing
# tools/scriptgen/emit_*.py -- generates a language's BINDINGS to the engine.
# csharp is wired into BOTH: tools/scriptgen/emit_csharp.py emits the `Jce`
# class a .cs script calls the engine through.

set(JCE_BACKEND_LANGUAGE   "csharp")
set(JCE_BACKEND_VM_TARGET  "jce_script_vm_csharp")

# .cs, the real extension, unlike js's .jcejs -- and the difference is not
# taste.  editor/src/core/jce_assetdb.cpp already classifies .js and .ts as
# project-side WEB TOOLING the engine does not execute, so claiming .js would
# offer every build script as an attachable gameplay script.  Nothing claims
# .cs, and a .cs file in a JCE project IS a gameplay script.
#
# It is a REFERENCE form (engine/src/resource/jce_asset_ext.c): the path names
# the type, the bytes are never read, and the cooker therefore does not pack
# the source into the shipped game.
set(JCE_BACKEND_EXTENSIONS "cs")

# TRUE, and this is the one backend for which it is load-bearing at RUNTIME:
# managed code P/Invokes jce_script_api by name, so the shared library must be
# findable beside the executable or every binding fails at the first call with
# a DllNotFoundException a designer cannot act on.
set(JCE_BACKEND_NEEDS_SCRIPT_API ON)

set(JCE_BACKEND_REGISTER_INCLUDE "#include <jce/script_vm/jce_script_vm_csharp.h>")
set(JCE_BACKEND_REGISTER_FRAGMENT "register.c.in")

# One runtime input the backend cannot discover: where the managed assembly
# is.  Beside the executable is the answer on a player's machine (staging puts
# it there); the baked path is the in-tree fallback, the same two-step the java
# backend uses for its JNI shim.
set(JCE_BACKEND_RUNTIME_VARS "JCE_SCRIPT_CSHARP_ASSEMBLY")

# STAGE_FILES, not STAGE_TARGETS: `dotnet build` is deliberately outside CMake
# (so a machine with no .NET SDK still builds every native target), so there is
# no target to copy from.  Both files, and both are load-bearing -- hostfxr
# finds the runtimeconfig BY NAME beside the assembly, so an assembly staged
# without it initialises nothing.
# nethost.dll is here for a reason worth reading: this backend links the
# IMPORT library, not the static one, because Microsoft builds libnethost.lib
# against /MT and this engine is /MD -- LNK2038, measured.  So the DLL is a
# runtime input like the other two.
set(JCE_BACKEND_STAGE_FILES
    "${JCE_SCRIPT_CSHARP_ASSEMBLY};${JCE_SCRIPT_CSHARP_RUNTIMECONFIG};${JCE_SCRIPT_CSHARP_NETHOST}")

# Not a "simple" SDK import: the VM archive links the .NET app-host's nethost,
# whose path is a machine fact the probe discovered.  A hand-written block in
# JCEScripting.cmake.in re-probes it for the consumer.
set(JCE_BACKEND_SDK_IMPORT_SIMPLE OFF)
set(JCE_BACKEND_SDK_HEADER_DIR "include")

# All four are runtime/link inputs, not optional examples.  The native VM by
# itself can be archived successfully while every managed script fails before
# registration, so the SDK report must require the same closure as staging.
set(JCE_BACKEND_SDK_REQUIRES_LINK "nethost")
set(JCE_BACKEND_SDK_REQUIRES_SHARED "nethost")
set(JCE_BACKEND_SDK_REQUIRES_FILE
    "share/jce/scripting/csharp/JceScript.dll;share/jce/scripting/csharp/JceScript.runtimeconfig.json")

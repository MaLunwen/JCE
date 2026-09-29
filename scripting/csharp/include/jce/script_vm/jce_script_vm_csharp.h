/*
 * jce_script_vm_csharp.h — the "csharp" JceScriptVM, hosting the machine's
 * .NET runtime through nethost/hostfxr.
 *
 * ══ A .cs FILE NAMES A CLASS, IT DOES NOT CARRY ONE ═════════════════════
 *
 * A C# script is COMPILED BEFORE THE PROCESS STARTS, by `dotnet build`, and
 * what the engine loads is the assembly.  So the path in a Script component
 * is a REFERENCE (JCEASSET_SCRIPT_FORM_REFERENCE, the form .jcecpp and .jcec
 * already use): `Assets/Turret.cs` resolves to the type `Turret`, and the
 * bytes at that path are never read by the engine.
 *
 * That is Unity's convention — the class name matches the file name — and it
 * is chosen over the alternative rather than inherited: the alternative is
 * compiling C# at runtime, which needs Roslyn, which ships with the .NET SDK
 * and NOT with the runtime a player has.  A shipping game would then carry
 * ~10 MB of compiler to re-derive an answer the build already had.
 *
 * The consequence is stated rather than hidden: jce_script_instantiate_source
 * REFUSES, once, with a message naming jce_script_instantiate.  The cpp
 * backend answers the same way for the same reason.
 *
 * ══ THE RUNTIME IS THE MACHINE'S ════════════════════════════════════════
 *
 * Like java's JVM and unlike lua and quickjs, the .NET runtime is not
 * vendored.  Measured: a self-contained net8.0 publish of a hello-world is
 * 70.3 MB, and framework-dependent is 77 MB of shared runtime the machine
 * usually already has.  Six languages cannot each add that.
 *
 * jce_script_vm_csharp_register() therefore FAILS on a machine with no .NET,
 * and says so.  It is not a fatal error for the process: every other language
 * still registers, exactly as java behaves without a JVM.
 *
 * ══ NO EXCEPTION MAY CROSS INTO THE ENGINE ══════════════════════════════
 *
 * This is the one hard rule of the managed side and it is not symmetric with
 * the others.  A .NET exception that escapes an [UnmanagedCallersOnly] method
 * does not propagate — the runtime FAILS FAST and kills the process, because
 * the frame below is C and has no unwind information.  There is nothing this
 * C file can catch.
 *
 * So the barrier lives inside EVERY UnmanagedCallersOnly entry point on the
 * managed side (scripting/csharp/managed/JceScript/Runtime.cs), the same
 * placement scripting/cpp uses for its noexcept+catch(...) thunks: on the
 * module side of the boundary, never on the engine side.
 */

#ifndef JCE_SCRIPT_VM_CSHARP_H
#define JCE_SCRIPT_VM_CSHARP_H

#include <jce/middleware/script/jce_script.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* The language key this backend registers, spelled ONCE.
 * tools/audit/check_script_language_catalog.py resolves this macro when it
 * cross-checks the extension claim against engine/src/resource/jce_asset_ext.c
 * and against scripting/csharp/jce_backend.cmake, so the three cannot drift. */
#define JCE_SCRIPT_VM_CSHARP_LANGUAGE  "csharp"
#define JCE_SCRIPT_VM_CSHARP_EXTENSION "cs"

/* Where the managed assembly and its runtimeconfig are.
 *
 * Set BEFORE register(), or register() looks beside the executable — the same
 * resolution order every other backend's runtime inputs use.  `assembly` is
 * the path to JceScript.dll; its .runtimeconfig.json must sit beside it,
 * because that file is what tells hostfxr which runtime to pick and hostfxr
 * finds it by name, not by argument.
 *
 * Both may be NULL to clear an earlier call.  Copied, not retained. */
JCE_API void jce_script_vm_csharp_set_assembly(const char *assembly);

/* Register the "csharp" language and claim ".cs" for it.
 *
 * Idempotent: a second call returns true without re-registering, matching
 * every other backend.  Returns FALSE — and logs which step failed — when
 * there is no .NET runtime on this machine, when the managed assembly is not
 * where set_assembly or the executable's directory says, or when the
 * registry refuses the table.
 *
 * CALL IT BEFORE THE FIRST SCENE LOADS.  Script components are instantiated
 * inside jce_runtime_create(), and a language registered after that is a
 * language the scene has already been refused by.  jce_script_enable()
 * generates the call for you; see scripting/csharp/register.c.in. */
JCE_API bool jce_script_vm_csharp_register(void);

/* Make the types in `path` (a managed assembly) findable by this VM.
 *
 * A compiled language needs an explicit "here is the code" step: there is no
 * path a scene could name that implies which assembly a type lives in.  The
 * cpp backend's jce_script_vm_cpp_load_library exists for the same reason.
 *
 * Loaded into the DEFAULT AssemblyLoadContext, so it is never unloaded.  A
 * collectible context would allow unload-and-reload and is deliberately not
 * used: the engine's reload path calls compile_module and rebind_instance and
 * never asks anyone to unload, so a collectible context would only add a way
 * for a live instance to keep a dead context alive.
 *
 * `s` must be a handle from THIS language; false, with a log line, otherwise. */
JCE_API bool jce_script_vm_csharp_load_assembly(JceScript *s, const char *path);

/* How many .NET runtimes this process has started (0 or 1) — the answer a
 * test asserts on to prove the host is a singleton.  hostfxr can initialise
 * once per process for a given runtimeconfig and every later VM shares it. */
JCE_API int jce_script_vm_csharp_runtime_starts(void);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_VM_CSHARP_H */

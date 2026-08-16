/*
 * jce_script_vm_python.h — the Python backend's registration seam.
 *
 * The engine calls UP into CPython through this backend, so a `.py` attached
 * to an entity receives on_start / on_update / collisions / messages through
 * exactly the paths Lua uses.  The vtable, the ABI rules, the frozen handle
 * header and the owning-thread rule are all in
 * engine/include/jce/middleware/script/jce_script_vm.h and none of them is
 * restated here.  The lifecycle SEMANTICS are in
 * scripting/python/jce_script/vm.py.  This header is the seam between them and
 * the application.
 *
 * NOTHING LINKS THIS BY DEFAULT, AND THAT IS THE DESIGN.  There is no central
 * list of backends — `jce_script_vm_register()` is a call — so a game that
 * wants Python links one more static library and says so:
 *
 *     #include <jce_script_vm_python.h>
 *
 *     jce_script_vm_python_add_path("scripting/python");   // or PYTHONPATH
 *     if (!jce_script_vm_python_register())
 *         ;   // logged with the reason; the engine keeps running on Lua
 *
 *     JceScript *s = jce_script_vm_create("python", &host, sizeof host);
 *     JceScriptInstance i = jce_script_instantiate(s, "scripts/bob.py", e);
 *     jce_script_call_start(s, i);
 *     ...
 *     jce_script_call_update(s, i, dt);
 *
 * From `jce_script_vm_create` onward NOTHING is Python-specific: every call is
 * the same public `jce_script_*` entry point the engine already makes for Lua.
 * That is the whole point of JceScriptVM, and it is what lets the lifecycle
 * differential drive both languages through one code path.
 *
 * WHAT MUST BE FINDABLE AT RUNTIME
 *
 *   1. the CPython this was linked against (CMake's Python3::Python);
 *   2. the `jce_script` package — the SAME pure-Python ctypes binding a
 *      standalone tool would `pip install`, on `sys.path`;
 *   3. `jce_script_api` (the shared library that binding calls), found through
 *      $JCE_SCRIPT_API or beside the package.
 *
 * (2) and (3) are what make the two directions compose: the surface a Python
 * script calls is not re-declared in the shim, it IS the binding a
 * cross-language differential already accepted against Lua.
 */
#ifndef JCE_SCRIPT_VM_PYTHON_H
#define JCE_SCRIPT_VM_PYTHON_H

#include <jce/middleware/script/jce_script_vm.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Must equal `jce_script.vm.PROTOCOL`.  The C shim calls that module's methods
 * by name and passes their arguments positionally, and the two SHIP
 * SEPARATELY — the engine as a binary, the package through pip — so a skew is
 * a real configuration rather than a hypothetical.  create refuses when they
 * differ, naming both numbers, instead of the skew surfacing as a TypeError
 * inside somebody's on_update and being blamed on their script.
 * *Enforced by:* tests/scripting/python/test_emit_python.py ::
 * test_the_vm_protocol_matches_the_shim, which reads this line and that
 * constant.  Bump it whenever a ScriptVM method changes name, arity or return
 * shape. */
#define JCE_PY_VM_PROTOCOL 1

/* Largest number of directories jce_script_vm_python_add_path() will hold. */
#define JCE_PY_VM_MAX_PATHS 8
/* Longest one, including the NUL. */
#define JCE_PY_VM_PATH_MAX  512
/* Longest error line forwarded to host.log. */
#define JCE_PY_VM_MSG_MAX   512

/* This backend's table.  Exposed for the same reason jce_script_vm_lua() is:
 * an application may name it, and a handle's header must point at it. */
const JceScriptVM *jce_script_vm_python(void);

/* Register the "python" language.  Returns false — with the reason logged — if
 * the name is already taken, the registry is full, or any slot is NULL.  A
 * second call for the same name is refused rather than replacing a live
 * registration; handles hold a pointer into the registry. */
bool jce_script_vm_python_register(void);

/* Prepend a directory to `sys.path`.
 *
 * PYTHONPATH works too and CPython honours it directly; this exists because an
 * environment variable is process-wide and inherited, which is the wrong
 * lifetime for "this game ships its scripts here".  Entries are applied when
 * the first VM is created and again for any added afterwards, so call order is
 * not a trap.  Returns false, logging the reason, for a NULL/empty directory,
 * one longer than JCE_PY_VM_PATH_MAX, or when JCE_PY_VM_MAX_PATHS is full. */
bool jce_script_vm_python_add_path(const char *dir);

/* GIL bookkeeping — the answer to "how does a test see it?".
 *
 * Every slot's Python work goes through ONE door (`py_call`), which acquires
 * nothing itself but REFUSES to proceed when PyGILState_Check() says the GIL
 * is not held.  That converts the failure mode of a slot that forgot
 * PyGILState_Ensure from an access violation inside CPython — which prints
 * nothing, including the named failures it had already produced — into a
 * counted, cleanly-returned no-op.
 *
 *   enters == leaves     across any balanced run of dispatches
 *   body_unheld == 0     no Python work was ever attempted without the GIL
 *   still_held  == 0     no Release left the GIL at a different level than
 *                        the matching Ensure found it
 *
 * *Enforced by:* tests/scripting/python/test_jce_script_vm_python.c ::
 * test_every_slot_balances_the_gil and test_no_slot_body_ran_without_the_gil.
 */
typedef struct JceScriptVmPythonGilStats {
    uint64_t enters;
    uint64_t leaves;
    uint64_t body_unheld;
    uint64_t still_held;
} JceScriptVmPythonGilStats;

void jce_script_vm_python_gil_stats(JceScriptVmPythonGilStats *out);

/* Handles created and not yet destroyed.
 *
 * Exported because INTERPRETER LIFETIME is the decision here whose failure
 * mode is a crash at shutdown that no lifecycle test would notice.  The
 * interpreter is started once, on the first create, and DELIBERATELY never
 * finalised (the .c states the measurement and the cost).  A test can
 * therefore assert this count returns to zero across create/destroy cycles —
 * i.e. that per-handle state really is released — separately from the
 * interpreter, which by design outlives them all. */
int jce_script_vm_python_live_handles(void);

/* True when THIS backend called Py_InitializeFromConfig, false when it found a
 * CPython an embedder had already started.  A tenant configures nothing and
 * finalises nothing; the distinction is exported so a test can tell which
 * process it is in rather than assuming. */
bool jce_script_vm_python_owns_interpreter(void);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_VM_PYTHON_H */

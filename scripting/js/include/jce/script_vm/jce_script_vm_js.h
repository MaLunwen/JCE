/*
 * jce_script_vm_js.h — the "js" JceScriptVM, over a vendored quickjs-ng.
 *
 * ══ WHY .jcejs AND NOT .js ══════════════════════════════════════════════
 *
 * MEASURED, not chosen for taste.  editor/src/core/jce_assetdb.cpp already
 * classifies `.js` and `.ts` as JCE_ASSET_KIND_SCRIPT, and its comment says
 * exactly what that means: "the web-tooling extensions used by project-side
 * scripts" — build scripts, tooling, generators — listed beside `.c`/`.cpp`,
 * i.e. code the ENGINE DOES NOT EXECUTE.
 *
 * Claiming `.js` for this VM would collide with that decision head-on: every
 * project-side JavaScript file in a project would become an attachable
 * gameplay script in the editor's Script picker, and — through
 * jce_build_asset_policy.cpp, which treats "has a script language" as "ship
 * it" — would be packed into the shipped game as readable source.  A build
 * tool is not a gameplay script and must not be shipped as one.
 *
 * `.jcejs` follows the precedent `.jcecpp` and `.jcec` already set for exactly
 * this reason: a language whose natural extension means something else in a
 * project tree gets an unambiguous one.
 *
 * ══ ONE CONTEXT, N OBJECTS ══════════════════════════════════════════════
 *
 * A script instance is a JS OBJECT in one shared JSContext, never a context
 * of its own.  Measured on quickjs-ng v0.16.2, x64:
 *
 *     JS_NewContext                       49,680 B marginal, 0.120 ms
 *     a JS object with one property          120 B
 *
 * At 10,000 scripted entities that is ~500 MB and 1.2 s of construction
 * against ~1.2 MB and nothing — a 400x ratio.  A context per entity would
 * also give every entity its OWN globalThis and its own copy of every
 * intrinsic prototype, so two scripts could not share a value and an engine
 * binding would have to be installed 10,000 times.
 *
 * ══ NO EXCEPTION CAN CROSS INTO THE ENGINE ══════════════════════════════
 *
 * QuickJS does not throw; it RETURNS an exception JSValue and sets a pending
 * exception on the context.  So the barrier here is a check-and-clear at every
 * dispatch point, the same shape scripting/java uses (java_take_exception:
 * check, CLEAR, report through the host), and structurally the same guarantee
 * the cpp backend gets from being a C translation unit.  There is no unwind
 * path through the engine's C frames to protect against, which is one reason
 * this backend is cheaper to get right than a managed runtime would be.
 *
 * A dispatch that leaves a pending exception un-cleared poisons every later
 * call on that context, so js_take_exception() is called on EVERY path that
 * can fail, not only the ones that usually do.
 */

#ifndef JCE_SCRIPT_VM_JS_H
#define JCE_SCRIPT_VM_JS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* The language key this backend registers, spelled ONCE.
 * tools/audit/check_script_language_catalog.py resolves this macro when it
 * cross-checks the extension claim against engine/src/resource/jce_asset_ext.c
 * and against scripting/js/jce_backend.cmake, so the three cannot drift. */
#define JCE_SCRIPT_VM_JS_LANGUAGE  "js"
#define JCE_SCRIPT_VM_JS_EXTENSION "jcejs"

/* Register the "js" language and claim ".jcejs" for it.
 *
 * Idempotent: a second call returns true without re-registering, matching
 * every other backend.  Returns false when the registry refuses — which it
 * does for a duplicate language name, or for a table with a NULL slot.
 *
 * CALL IT BEFORE THE FIRST SCENE LOADS.  Script components are instantiated
 * inside jce_runtime_create(), and a language registered after that is a
 * language the scene has already been refused by.  jce_script_enable()
 * generates the call for you; see scripting/js/register.c.in. */
JCE_API bool jce_script_vm_js_register(void);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_VM_JS_H */

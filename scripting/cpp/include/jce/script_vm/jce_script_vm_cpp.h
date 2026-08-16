/*
 * jce_script_vm_cpp.h — the "cpp" JceScriptVM, and the ABI between the host
 * and a native script module.
 *
 * ── WHAT A C++ "SCRIPT" IS ───────────────────────────────────────────────
 *
 * Every other backend on this seam owns an interpreter: the engine hands it a
 * path, it reads bytes and compiles them.  A C++ script is COMPILED NATIVE
 * CODE — the bytes were compiled before the process started — so the two
 * questions the other backends never have to answer are the whole design of
 * this one:
 *
 *   1. What does "instantiate a script" mean when the script is a class?
 *   2. What keeps the code alive while an instance of it exists?
 *
 * ANSWER TO (1): `jce_script_instantiate(s, path, owner)`'s `path` NAMES A
 * CLASS rather than containing one, and it is resolved through a registry of
 * named classes that MODULES fill.  A module is a JceCppModuleDesc: a
 * struct_size-first table of named JceCppScriptClass entries, each of which is
 * a factory plus one thunk per lifecycle callback.  A module reaches the
 * registry two ways and they are the same code path after the first line:
 *
 *   - jce_script_vm_cpp_add_module()   — the module is linked into this
 *     binary.  This is what a game that compiles its gameplay classes into
 *     its own executable uses, and it needs no loader at all.
 *   - jce_script_vm_cpp_load_library() — the module is a shared object.  The
 *     path must be ABSOLUTE (see the note on that function).
 *
 * ANSWER TO (2) is `jce_script_vm_cpp_unload()`, and it is the reason this
 * header exists rather than "just call jce_library_close":
 *
 *   THE ENGINE HOLDS FUNCTION POINTERS THE MODULE SUPPLIED.  Unloading a
 *   module while one of its instances is live unmaps the code that every
 *   subsequent on_update dispatches into.  That is a use-after-free that
 *   presents as a crash inside the engine's own forwarder, with no script in
 *   the backtrace, and NO lifecycle test catches it because every lifecycle
 *   test unloads after it is done.  So the registry refcounts live instances
 *   per module and unload is REFUSED, loudly, while the count is non-zero.
 *   *Enforced by:* tests/scripting/cpp/test_jce_script_vm_cpp_lifecycle.cpp ::
 *   "unloading a module with a live instance is refused" and
 *   "...and the instance still dispatches after the refusal".
 *
 * ── EXCEPTIONS MUST NOT CROSS THE C ABI ──────────────────────────────────
 *
 * Every slot of JceScriptVM is a C function pointer.  A C++ on_update that
 * throws through one is undefined behaviour, not an error: with MSVC's
 * default /EHsc the compiler is entitled to assume the C frames in between
 * cannot throw, and unwinding through them is not a diagnosable event.
 *
 * The policy is: NO EXCEPTION EVER LEAVES A MODULE.  It is enforced where it
 * has to be enforced — in the thunk, which the MODULE compiles — and the
 * shape of that enforcement is baked into this ABI rather than left to a
 * convention: every callback returns JceCppStatus, `NULL` for "returned
 * normally" and otherwise the exception's text.  A thunk that did not catch
 * would have nothing to return.  jce_script_cpp.hpp generates the thunks, so
 * the try/catch is compiled in the module's own translation unit with the
 * module's own runtime — catching in the ENGINE would be a second undefined
 * behaviour, because an exception object thrown by one CRT and caught by
 * another is not a defined interaction on Windows.
 * *Enforced by:* ... :: "an erroring callback is logged on both sides, both
 * sides then stop calling it, and neither side stops calling anything else"
 * — the process surviving three dispatches around a throw is the half of that
 * case which proves the thunk caught.
 *
 * What the ENGINE does with a non-NULL status is deliberately what LUA DOES,
 * measured rather than assumed — see the note above `JceCppStatus`.
 *
 * ── THE HAND-WRITTEN SEVEN ARE NOT HERE ──────────────────────────────────
 *
 * A script class calls the engine through jce::script::Api — the C++ wrapper
 * over scripting/c_abi, which is the shipped call-DOWN path and which does
 * not declare `log`, `asset_read_text`, `asset_read_json`, `play_sound`,
 * `start_coroutine`, `wait_seconds` or `stop_coroutine`.  Nothing in this
 * header adds them back.  In particular `asset_read_text`/`asset_read_json`
 * carry sandbox policy (path validation, a 1 MiB cap, a depth-capped JSON
 * walk) that lives in static functions inside jce_script.c; a convenience
 * that reached the raw `read_file` host member instead is the escape the
 * manifest calls P0-2.
 * *Enforced by:* ... :: "the module-facing surface names none of the seven
 * hand-written entries", which reads THIS FILE and jce_script_cpp.hpp.
 */
#ifndef JCE_SCRIPT_VM_CPP_H
#define JCE_SCRIPT_VM_CPP_H

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ── HOW A `scriptPath` REACHES A CLASS ──────────────────────────────────
 *
 * Two questions, answered by two different mechanisms, and conflating them is
 * what kept C++ scripts out of a multi-language scene for as long as it did:
 *
 *   WHICH VM?   jce_script_vm_language_for_path(), which answers only by file
 *               extension.  jce_script_vm_cpp_register() therefore CLAIMS one
 *               — ".jcecpp" — for the same reason python claims ".py".  It is
 *               deliberately not ".cpp": that would classify every translation
 *               unit in the project as an attachable script and would tell the
 *               cooker to pack the project's C++ source into the shipped game.
 *               The offline half of the same answer is the row in
 *               engine/src/resource/jce_asset_ext.c, and
 *               check_script_language_catalog.py fails when the
 *               two disagree.
 *
 *   WHICH CLASS?  cpp_instantiate(), which tries three candidates in a fixed
 *               order: the whole stored string exactly; its last path
 *               component; and that component without its final extension.
 *               So "gameplay/FlowerSway.jcecpp", "FlowerSway.jcecpp" and a
 *               bare "FlowerSway" all reach a class published as
 *               "FlowerSway", and a class published under the WHOLE stored
 *               string still wins — which is what makes the added forms
 *               additive rather than a redefinition.  The full reasoning,
 *               including why no extension is validated there, is on
 *               class_resolve() in jce_script_vm_cpp.c.
 *
 * A module author who names the class the way C++ spells it — plain
 * "FlowerSway" — needs to know none of this. */

/* The language key this backend registers under.  Spelled once. */
#define JCE_SCRIPT_VM_CPP_LANGUAGE "cpp"

/* The module ABI's version.  Independent of JCE_SCRIPT_API_VERSION (the
 * call-down surface) and of the engine ABI: those version what a module may
 * CALL, this versions how the engine calls IN.  A module compiled against a
 * newer major of this ABI is refused at load; see jce_script_vm_cpp_load_
 * library.  Bumped only by a change this header's struct_size clamps cannot
 * absorb — i.e. never for an append. */
#define JCE_CPP_MODULE_ABI_VERSION 1u

/* The one symbol a shared-object module must export. */
#define JCE_CPP_MODULE_ENTRY_SYMBOL "jce_cpp_script_module"

/* Longest module / class / global-handler name the registry stores, NUL
 * included.  The registry COPIES every name: a name is often a string literal
 * in the module's image, and the registry outlives an unload. */
#define JCE_CPP_NAME_MAX 64

/* Ceilings.  Fixed arrays rather than growable ones for the same reason
 * jce_script_vm.c's registry is fixed: live instances hold a pointer into a
 * module entry, and a realloc would dangle every one of them. */
#define JCE_CPP_MAX_MODULES        16
#define JCE_CPP_MAX_CLASSES_TOTAL 128
#define JCE_CPP_MAX_GLOBALS_TOTAL 128

/* The return of every lifecycle thunk.
 *
 *   NULL      the callback returned normally.
 *   non-NULL  it threw, and this is the exception's text.  The pointer must
 *             stay valid until the next call into the same instance; the
 *             generated thunks keep it in a per-instance buffer.
 *
 * WHAT THE ENGINE DOES WITH A NON-NULL STATUS IS WHAT LUA DOES, AND THAT IS
 * MEASURED, NOT READ.  It is now THE FAILING-CALLBACK RULE, stated in full in
 * jce_script.h: the status is logged as "<method> error: <text>" through
 * host->log and the engine log, the published disable notice follows it
 * through the same two sinks, and that hook is not called again on that
 * instance.
 *
 * THIS PARAGRAPH USED TO SAY THE OPPOSITE, AND THE HISTORY IS WORTH KEEPING.
 * When this backend was written, jce_script.h already promised the disable and
 * jce_script.c's call_method() only logged and returned — no per-instance flag
 * existed anywhere in the reference.  So this backend deliberately matched the
 * CODE and not the comment, because its acceptance test is a differential
 * against Lua and obeying the documentation would have made every erroring
 * case disagree with the reference for a reason that was not this backend's
 * defect.  That reasoning was right and it is now spent: the reference
 * implements the sentence, so matching the reference and matching the
 * documentation are the same thing again.
 *
 * ONE DIVERGENCE, PINNED.  jce_script.h says a rebind clears the disables.
 * This backend's `rebind_instance` is a documented no-op (a compiled class
 * cannot be recompiled in-process), so a disabled hook comes back only when
 * the instance does: jce_script_vm_cpp_unload + _load_library +
 * re-instantiate, which is this backend's only hot-reload path anyway.
 * *Enforced by:* ... :: "an erroring callback is logged on both sides, both
 * sides then stop calling it, and neither side stops calling anything else"
 * and ... :: "PINNED: a rebind cannot re-enable a disabled callback for
 * compiled code, and re-instantiating does". */
typedef const char *JceCppStatus;

/* What a class factory is handed.
 *
 * ALLOCATED BY THE ENGINE, READ BY THE MODULE — the mirror of JceScriptVM,
 * so the clamp is the module's job here.  A module built against a newer
 * header than the engine it is loaded into would read past the end of this
 * struct; `struct_size` is how it does not.  jce_script_cpp.hpp checks it
 * before touching any member, so a module written with that header gets the
 * check without knowing this paragraph exists.
 *
 * `host` points at the VM handle's OWN copy of JceScriptHost, already clamped
 * to min(the creator's host_size, the engine's sizeof) over a zeroed table,
 * and `host_size` is that clamped size — not the engine's sizeof and not the
 * creator's.  Pass BOTH to jce_script_api_open(); passing sizeof(JceScriptHost)
 * instead would re-widen a short host that the engine already narrowed. */
typedef struct JceCppScriptContext {
    size_t               struct_size;   /* sizeof as the ENGINE saw it */
    const JceScriptHost *host;
    size_t               host_size;
    JceScriptEntity      entity;        /* the owner passed to instantiate() */
    const char          *class_name;
} JceCppScriptContext;

/* One named script class.
 *
 * ALLOCATED BY THE MODULE, READ BY THE ENGINE — so this one is the direction
 * JceScriptVM is, and the engine copies min(struct_size, its own sizeof) over
 * a zeroed entry.  struct_size FIRST, APPEND ONLY after.
 *
 * `create` and `destroy` are REQUIRED; a class missing either is rejected at
 * registration, by name.  Every callback below them is OPTIONAL and NULL
 * means "this class does not define that handler" — a clean no-op, which is
 * exactly what the Lua path does when lua_getfield finds no function.  That
 * is why the optional ones are NOT the JceScriptVM rule ("fill every slot"):
 * there, a NULL slot silently disables a whole dispatch path for every
 * script; here it is one class declining one callback, which is the normal
 * case and is observable from the class's own source. */
typedef struct JceCppScriptClass {
    size_t       struct_size;    /* sizeof as the MODULE saw it */
    /* The key instantiate(path) resolves TO; copied.  Name it the way C++
     * spells the class ("FlowerSway"), not the way the scene stores the path
     * — see HOW A `scriptPath` REACHES A CLASS above.  A name that IS a whole
     * stored path still works and is matched first, which is why no existing
     * module changed meaning when the other two forms were added. */
    const char  *name;

    void        *(*create)(const JceCppScriptContext *ctx);  /* NULL == failed */
    void         (*destroy)(void *self);

    JceCppStatus (*on_start)    (void *self);
    JceCppStatus (*on_update)   (void *self, float dt);
    /* Called from jce_script_release BEFORE destroy, mirroring the Lua path,
     * whose release() calls on_destroy and then unrefs the instance. */
    JceCppStatus (*on_destroy)  (void *self);
    JceCppStatus (*on_collision)(void *self, JceScriptEntity other_entity);
    JceCppStatus (*on_message)  (void *self, const char *msg_name,
                                 double number_arg, const char *str_arg);
    JceCppStatus (*on_anim_event)(void *self, uint32_t id, const char *name,
                                  float f0, float f1, int i0);
    /* ── APPEND ONLY BELOW THIS LINE ─────────────────────────────────── */
} JceCppScriptClass;

/* One named global handler — the C++ answer to a global Lua function, which
 * is what jce_script_call_named / _num / _str dispatch to.
 *
 * Lua has one function per name and calls it with one, two or three values
 * depending on which entry point fired; extra arguments a Lua function does
 * not declare are simply dropped.  C cannot overload on arity through one
 * pointer, so a name carries up to three pointers and the engine picks the
 * one matching the entry point.  When the exact arity is absent but
 * `fn_entity` is present, `fn_entity` is called and the trailing argument is
 * dropped — which is precisely what Lua does for `function f(e) end` invoked
 * as f(e, v), and is the only way call_named_num can agree with the reference
 * for a handler that ignores its value.
 * *Enforced by:* ... :: "a global that takes only the entity is still invoked
 * by call_named_num, exactly as Lua drops the extra argument".
 *
 * These are PROCESS-WIDE, not per-JceScript, and that is a real divergence
 * from Lua, whose globals live in one lua_State: native code has one copy of
 * a function per process and pretending otherwise would be a fiction with a
 * registry behind it.  *Pinned by:* ... :: "cpp globals are process-wide,
 * Lua globals are per-VM (pinned divergence)". */
typedef struct JceCppScriptGlobal {
    size_t       struct_size;
    const char  *name;
    /* EVERY FORM TAKES A CONTEXT, and that is not symmetry for its own
     * sake.  A global handler has no instance, so without a context it has
     * no host and no way to call the engine at all -- a slot the core
     * requires to be filled, filled with something that can do nothing.  The
     * context the engine builds here carries the VM's clamped host with
     * `entity` set to the argument entity; `class_name` is NULL, because a
     * global belongs to no class. */
    JceCppStatus (*fn_entity)(const JceCppScriptContext *ctx,
                              JceScriptEntity e);
    JceCppStatus (*fn_num)   (const JceCppScriptContext *ctx,
                              JceScriptEntity e, double value);
    JceCppStatus (*fn_str)   (const JceCppScriptContext *ctx,
                              JceScriptEntity e, const char *str);
    /* ── APPEND ONLY BELOW THIS LINE ─────────────────────────────────── */
} JceCppScriptGlobal;

/* What a module publishes.  Arrays of POINTERS, not of structs: each class
 * carries its own struct_size and is clamped individually, which an array of
 * structs could not express without also publishing a stride. */
typedef struct JceCppModuleDesc {
    size_t      struct_size;     /* sizeof as the MODULE saw it */
    uint32_t    abi_version;     /* JCE_CPP_MODULE_ABI_VERSION at compile time */
    const char *module_name;     /* unique; copied by the registry */

    const JceCppScriptClass  *const *classes;
    size_t                           class_count;
    const JceCppScriptGlobal *const *globals;
    size_t                           global_count;
    /* ── APPEND ONLY BELOW THIS LINE ─────────────────────────────────── */
} JceCppModuleDesc;

/* The signature of JCE_CPP_MODULE_ENTRY_SYMBOL.  The engine passes its own
 * JCE_CPP_MODULE_ABI_VERSION so a module can refuse a host it cannot serve
 * and return NULL, rather than handing over a table that will be misread. */
typedef const JceCppModuleDesc *(*JceCppModuleEntry)(uint32_t engine_abi_version);

/* An opaque registry entry.  Stable for the life of the process even after an
 * unload (the slot is retired, not recycled), so a stale handle is a refusal
 * rather than a wrong module. */
typedef struct JceCppModule JceCppModule;

/* ── Host-side API ───────────────────────────────────────────────────────── */

/* The table.  Exposed so a host can name it, and so the handle-header rule in
 * jce_script_vm.h ("hdr.vm MUST point at YOUR table") has an address to be
 * about. */
const JceScriptVM *JCE_CALL jce_script_vm_cpp(void);

/* Create a handle that dispatches through THIS backend, for a JceScriptVM
 * table that is not `jce_script_vm_cpp()`.
 *
 * WHY THIS IS PUBLIC, AND ITS ONLY CALLER.  scripting/c registers a SECOND
 * language, "c", whose every slot is this backend's: the module ABI is a
 * plain C ABI, a compiled class has no language at run time, and one native
 * class registry serving both is the whole point (see THE "c" LANGUAGE in
 * <jce/script_vm/jce_script_vm_c.h>).  It cannot simply reuse
 * cpp_create_sized, because jce_script_vm_create() REFUSES a handle whose
 * JceScriptVMHeader does not point at the table it dispatched through — it
 * checks `hdr.vm == e->origin || hdr.vm == &e->vm`, and the cpp table is
 * neither of those for the "c" entry.  So the second language needs one
 * create function of its own, and this is what that function is made of.
 *
 * `table` MUST be the address the caller passed to jce_script_vm_register().
 * Passing anything else produces a handle jce_script_vm_create() rejects —
 * loudly, by name, having already destroyed it — which is the failure mode
 * this parameter exists to make impossible to get silently wrong.
 *
 * Everything downstream of the header is identical to a cpp handle: the same
 * clamped host copy, the same instance table, the same registry.  The table's
 * `language` is what the backend's own diagnostics print, so a script that
 * fails under "c" says c and not cpp.
 * *Enforced by:* tests/scripting/c/test_jce_script_vm_c.c ::
 * "the c VM is a second language over ONE shared native class registry". */
JceScript *JCE_CALL jce_script_vm_cpp_create_for(const JceScriptVM *table,
                                                 const JceScriptHost *host,
                                                 size_t host_size);

/* Register "cpp" with jce_script_vm_register() AND claim ".jcecpp" with
 * jce_script_vm_register_extension().  Both, because a VM nothing routes to is
 * reachable only through the whole-process JCE_SCRIPT_LANGUAGE override, which
 * a scene with a second language cannot use.
 *
 * Idempotent HERE, unlike the core calls it wraps, and idempotent in each half
 * separately: a second call returns true without re-registering, and a call
 * that follows a refused claim retries the claim.  A host that links several
 * modules would otherwise have to remember which one of them called first.
 *
 * Returns false when either half is refused; both log their own reason. */
bool JCE_CALL jce_script_vm_cpp_register(void);

/* Publish a module that is LINKED INTO THIS BINARY.  `desc` is read, clamped
 * and copied; it need not stay alive, but the function pointers in it must
 * (they are the code).  Returns NULL and logs when the ABI major disagrees,
 * a name is empty/too long/already registered, a class lacks create/destroy,
 * or a ceiling is reached. */
JceCppModule *JCE_CALL jce_script_vm_cpp_add_module(const JceCppModuleDesc *desc);

/* Publish a module from a SHARED OBJECT.
 *
 * `absolute_path` must be absolute, and a relative path is REFUSED rather
 * than resolved: jce_library.h:36-39 documents that on Windows the open
 * "attaches to an already-resident module of the same name rather than
 * loading a second copy", so a bare or relative name can silently bind to a
 * different file than the one on disk — for a module whose function pointers
 * the engine is about to call, that is the wrong kind of surprise.
 * *Enforced by:* ... :: "a relative module path is refused".
 *
 * The library is kept mapped until jce_script_vm_cpp_unload() succeeds. */
JceCppModule *JCE_CALL jce_script_vm_cpp_load_library(const char *absolute_path);

/* Retire a module: its classes stop resolving and, if it came from a shared
 * object, the library is closed.
 *
 * REFUSED (returns false, logs, changes nothing) while the module has live
 * instances — see ANSWER TO (2) at the top of this file.  Release the
 * instances with jce_script_release() first. */
bool JCE_CALL jce_script_vm_cpp_unload(JceCppModule *m);

/* Diagnostics.  live_instances is the refcount unload checks; -1 for NULL. */
int         JCE_CALL jce_script_vm_cpp_module_count(void);
JceCppModule *JCE_CALL jce_script_vm_cpp_module_at(int index);
const char *JCE_CALL jce_script_vm_cpp_module_name(const JceCppModule *m);
int         JCE_CALL jce_script_vm_cpp_live_instances(const JceCppModule *m);

/* Resolve a name the way instantiate() does — all three candidate forms, so a
 * stored `scriptPath` may be passed straight in.  Exists so a host can
 * validate an authored path at load time instead of discovering it as a 0 from
 * instantiate at spawn time.
 * *Enforced by:* tests/scripting/cpp/test_jce_script_vm_cpp_modules.cpp ::
 * "has_class answers for every form instantiate accepts" — the two resolving
 * differently is a validator that passes what will fail, or refuses what would
 * have run. */
bool JCE_CALL jce_script_vm_cpp_has_class(const char *class_name);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_VM_CPP_H */

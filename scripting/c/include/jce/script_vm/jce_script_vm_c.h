/*
 * jce_script_vm_c.h — the "c" JceScriptVM: writing an entity script in C99.
 *
 * ── THE DEFECT THIS FILE CLOSES ──────────────────────────────────────────
 *
 * C was MECHANICALLY reachable long before it was a language.  The module ABI
 * the "cpp" backend publishes is a plain C ABI — JceCppScriptClass is a
 * struct_size-first table of function pointers taking `void *self` — and a
 * pure C translation unit compiles against it under MSVC `-TC` and produces a
 * working object file.  Measured, not assumed.
 *
 * And that was worth nothing to a user, because the language was:
 *
 *   UNNAMED       jce_script_vm_count() listed lua, python, java, cpp.  A C
 *                 author asking "can I write this in C?" read no.
 *   UNCLAIMED     no extension routed to it, so a Script component could not
 *                 name a C script at all: jce_script_vm_language_for_path()
 *                 answered nothing and the entity was refused before any VM
 *                 was asked.
 *   UNCATALOGUED  no row in engine/src/resource/jce_asset_ext.c, so the
 *                 cooker labelled the file "binary", the editor's Script
 *                 picker never offered it, and the publication policy could
 *                 drop it from a packaged build.
 *   UNTESTED, UNDOCUMENTED, and — the part with a measurable cost — the
 *                 header a C author needed was called jce_script_vm_cpp.h,
 *                 which tells them in its filename that it is not for them.
 *
 * For a user, "reachable if you read the C++ backend's source and ignore what
 * its filename says" is indistinguishable from unsupported.
 *
 * ── SEPARATE LANGUAGE, ONE REGISTRY ──────────────────────────────────────
 *
 * "c" is its OWN registered language: its own jce_script_vm_register() name,
 * its own claimed extension (".jcec"), its own row in the offline catalog.
 * It is NOT an alias of "cpp", and the difference is not cosmetic — a user who
 * writes C and reads `registered languages: cpp` has been told the wrong thing
 * by a message that was in a position to tell them the right one.
 *
 * What it deliberately does NOT duplicate is the NATIVE CLASS REGISTRY.  One
 * registry serves both languages, and that is the honest arrangement rather
 * than a shortcut:
 *
 *   * A COMPILED CLASS HAS NO LANGUAGE AT RUN TIME.  What reaches the engine
 *     is a table of C function pointers.  There is no bit to branch on, and
 *     inventing one would be a fiction with state behind it.
 *   * TWO REGISTRIES WOULD MEAN TWO LOADERS, two unload refcounts, and two
 *     answers to "is this module already loaded" — for one set of .dll files.
 *   * A SECOND jce_script_vm_register("c") IS ALL THAT IS NEEDED, and the
 *     core would refuse a duplicate name anyway: live handles hold pointers
 *     into the registry, so a language name is claimed once, for good.
 *
 * So the language boundary is drawn where a user can see it — the header they
 * include, the extension they type, the name the editor prints, the language
 * every refusal names — and not where nothing could observe it.
 *
 * The consequences of one registry, stated rather than discovered:
 *
 *   * CLASS NAMES ARE SHARED ACROSS BOTH LANGUAGES AND ACROSS ALL LOADED
 *     MODULES.  A C class and a C++ class cannot both be called "Spinner";
 *     the second registration is refused by name, exactly as two C++ classes
 *     with one name are today.
 *   * NOTHING CHECKS THAT A `.jcec` PATH REACHES A CLASS WRITTEN IN C.  It
 *     cannot: see above.  The extension states the AUTHOR's language, which is
 *     what the editor, the picker and the diagnostics need it for.
 *   * jce_script_vm_cpp_add_module() / _load_library() / _unload() are the
 *     module calls for BOTH languages.  They keep their names because they
 *     are shipped API; their diagnostics say "native script module".
 *   * ONE MODULE IS ONE LANGUAGE, because one module descriptor is one
 *     translation unit and a translation unit is C or C++.  A project that
 *     wants both publishes TWO modules — two add_module() calls, or two
 *     entries in jce_project.json's "script_modules".  That is also how a
 *     project ports one script at a time.
 *
 * ── WHAT A C SCRIPT IS ───────────────────────────────────────────────────
 *
 *     #include <jce/script_vm/jce_script_vm_c.h>
 *
 *     typedef struct Spinner { float angle; } Spinner;
 *
 *     static void *spinner_create(const JceCScriptContext *ctx)
 *     {
 *         Spinner *self = (Spinner *)calloc(1, sizeof *self);
 *         (void)ctx;                 // ctx->entity and ctx->host are here
 *         return self;               // NULL == this instance failed
 *     }
 *     static void spinner_destroy(void *self) { free(self); }
 *
 *     static JceCStatus spinner_update(void *self, float dt)
 *     {
 *         ((Spinner *)self)->angle += 90.0f * dt;
 *         return JCE_C_OK;
 *     }
 *
 *     JCE_C_SCRIPT_CLASS_BEGIN(Spinner, "Spinner",
 *                              spinner_create, spinner_destroy)
 *         JCE_C_ON_UPDATE(spinner_update)
 *     JCE_C_SCRIPT_CLASS_END()
 *
 *     JCE_C_MODULE_BEGIN()
 *         JCE_C_MODULE_CLASS(Spinner)
 *     JCE_C_MODULE_GLOBALS()
 *     JCE_C_MODULE_END("demo", demo_module)
 *
 * The scene stores `"Spinner.jcec"` in the Script component's scriptPath, and
 * the host does this once, before the first scene loads:
 *
 *     jce_script_vm_c_register();                       // language + ".jcec"
 *     jce_script_vm_cpp_add_module(demo_module());      // linked-in module
 *     // or jce_script_vm_cpp_load_library(absolute);   // shared object
 *
 * ── THERE ARE NO THUNKS HERE, AND THAT IS THE POINT ──────────────────────
 *
 * jce_script_cpp.hpp exists mostly to GENERATE thunks: a C++ callback that
 * throws through a C function pointer is undefined behaviour, so every
 * lifecycle call has to be wrapped in a try/catch compiled in the module's own
 * translation unit.  C has nothing to catch.  A C author writes the ABI's
 * signature directly — `JceCStatus f(void *self, float dt)` IS the slot — and
 * the macros below only fill in struct_size, the name, and the NULLs.
 *
 * That also means the return value is yours to use.  Return JCE_C_OK for
 * "fine"; return a string to report an error, and the engine applies THE
 * FAILING-CALLBACK RULE to it exactly as it does to an escaped C++ exception:
 * logged as "<method> error: <text>" through the host log and the engine log,
 * then that hook is not called again on that instance.  The pointer must stay
 * valid until the next call into the same instance — a string literal or a
 * per-instance buffer, never a local array.
 *
 * ── THE TYPE NAMES ARE ALIASES, NOT A PARALLEL ABI ───────────────────────
 *
 * JceCScriptClass IS JceCppScriptClass, by typedef, with no conversion
 * anywhere.  They cannot drift, there is no second struct_size and no second
 * version handshake.  The C spellings exist so that a .c file reads as C — the
 * whole complaint this header answers — and so that a reader who greps for
 * either name finds every use of the other.
 *
 * ── WHAT A C SCRIPT CAN CALL ─────────────────────────────────────────────
 *
 * The same thing a C++ script can, minus the wrapper: scripting/c_abi's
 * generated <jce/script_api/jce_script_api.h>, opened with
 * jce_script_api_open(ctx->host, ctx->host_size, JCE_SCRIPT_API_VERSION).
 * Pass BOTH — ctx->host_size is the VM's already-clamped number, and passing
 * sizeof(JceScriptHost) instead would re-widen a host the engine narrowed.
 *
 * Like a C++ script and unlike Lua, Python and Java, a C script has NO `log`:
 * it is one of the seven hand-written entries the C ABI deliberately does not
 * export.  A C script that must report liveness writes it into the scene.
 *
 * ── C99, AND WHY THE MACROS USE DESIGNATED INITIALISERS ──────────────────
 *
 * This header is C99 and is meant for a C translation unit.  It is safe to
 * include from C++ (JCE_EXTERN_C_BEGIN, and every declaration is a C
 * declaration), but the authoring macros below emit designated initialisers,
 * which C++ accepted only in C++20 and only in declaration order — a C++
 * module should use jce_script_cpp.hpp, which is what it is for.
 *
 * The optional handlers are designated ON PURPOSE, and it is the opposite of
 * the rule that governs the JceScriptVM tables (jce_script_vm_cpp.c: "POSITIONAL,
 * like both of the engine's own vtables ... designated initialisers would
 * survive a reorder silently").  That rule is for a table where EVERY slot
 * must be filled and a reorder must break the build.  This one is a table
 * where most slots are meant to be absent — a class that defines only
 * on_update is the normal case — and with positional initialisers the single
 * line `JCE_C_ON_UPDATE(f)` would land f in the on_start slot.  Named slots
 * are what makes "write the handlers you have, in any order" true rather than
 * a comment.
 */
#ifndef JCE_SCRIPT_VM_C_H
#define JCE_SCRIPT_VM_C_H

/* THE C++ BACKEND'S HEADER, INCLUDED ON PURPOSE AND IN THIS DIRECTION.
 *
 * It declares the ABI both languages share, and it is C99-clean: it includes
 * only jce_script.h, jce_script_vm.h and <stdbool/stddef/stdint>, and every
 * declaration in it is a C declaration.  A C author never has to open it —
 * that was the defect — but the types have to come from ONE place, or the two
 * languages would have two ABIs under one name. */
#include <jce/script_vm/jce_script_vm_cpp.h>

JCE_EXTERN_C_BEGIN

/* The language key this backend registers under.  Spelled once.
 *
 * DELIBERATELY NOT USED in the jce_script_vm_register_extension() call in
 * jce_script_vm_c.c.  check_script_language_catalog.py reads those
 * calls statically and reports a claim it cannot resolve as "could not be
 * checked", which would leave the catalog row unguarded; a literal at the call
 * site is what keeps the runtime claim and the offline row checked against
 * each other.  Same shape as scripting/python, scripting/java and
 * scripting/cpp, for the same reason. */
#define JCE_SCRIPT_VM_C_LANGUAGE "c"

/* The extension a C script's scriptPath carries.  Claimed as a literal in
 * jce_script_vm_c.c (see above); the offline half is the row in
 * engine/src/resource/jce_asset_ext.c. */
#define JCE_SCRIPT_VM_C_EXTENSION "jcec"

/* ── The ABI, spelled the way a C file should spell it ───────────────────
 *
 * Aliases.  See THE TYPE NAMES ARE ALIASES above: same types, same sizes, same
 * struct_size fields, no conversion. */
typedef JceCppStatus        JceCStatus;
typedef JceCppScriptContext JceCScriptContext;
typedef JceCppScriptClass   JceCScriptClass;
typedef JceCppScriptGlobal  JceCScriptGlobal;
typedef JceCppModuleDesc    JceCModuleDesc;
typedef JceCppModule        JceCModule;

/* "the callback returned normally".  A named constant rather than a bare NULL,
 * because `return NULL;` from a function returning `const char *` reads as "no
 * string" and means "no error"; the two are the same value and not the same
 * statement. */
#define JCE_C_OK ((JceCStatus)0)

/* Register "c" with jce_script_vm_register() AND claim ".jcec" with
 * jce_script_vm_register_extension().  Both, for the same reason
 * jce_script_vm_cpp_register() does both: a VM nothing routes to is reachable
 * only through the whole-process JCE_SCRIPT_LANGUAGE override, which a scene
 * with a second language cannot use.
 *
 * INDEPENDENT OF jce_script_vm_cpp_register().  This does not register "cpp"
 * and that does not register "c": a host that wants both calls both, and a
 * host that wants only C gets a process in which ".jcecpp" resolves to no
 * language.  They are separate because a language a host did not ask for
 * should not appear in the list editor Play prints.
 *
 * Idempotent, and idempotent in each half separately, exactly as the cpp one
 * is: a second call returns true without re-registering, and a call following
 * a refused claim retries the claim.
 *
 * Returns false when either half is refused; both log their own reason. */
bool JCE_CALL jce_script_vm_c_register(void);

/* The table.  Exposed so a host can name it, and so the handle-header rule in
 * jce_script_vm.h has an address to be about.  Valid BEFORE register() — the
 * table is derived on first use, not at registration. */
const JceScriptVM *JCE_CALL jce_script_vm_c(void);

/* ── Authoring macros ────────────────────────────────────────────────────
 *
 * Everything below is optional.  A C author may fill a JceCScriptClass by hand
 * — it is a plain struct — and these exist only so that the two things that
 * MUST be right are not typed by hand: `struct_size` (the engine clamps to
 * min(module, engine), so a wrong one silently truncates the table) and the
 * slot each handler goes in. */

/* Open a class definition.
 *
 *   IDENT        a C identifier; it names the objects these macros generate,
 *                and it is what JCE_C_MODULE_CLASS() takes.
 *   NAME         the string instantiate() resolves to.  Spell it the way the
 *                scriptPath does, without the extension — "Spinner" for
 *                "Spinner.jcec".  See HOW A `scriptPath` REACHES A CLASS in
 *                jce_script_vm_cpp.h for the three candidate forms; a name
 *                that IS a whole stored path is matched FIRST and still works,
 *                which is why nothing that resolved before still resolves.
 *   CREATE_FN    void *(*)(const JceCScriptContext *) — NULL means the
 *   DESTROY_FN   void  (*)(void *)                      instance failed.
 *
 * create and destroy are ARGUMENTS and not optional lines because the registry
 * REQUIRES them: a class missing either is rejected at add_module() time, by
 * name.  Making them parameters means the omission is a compile error in the
 * module's own file rather than a log line at startup. */
#define JCE_C_SCRIPT_CLASS_BEGIN(IDENT, NAME, CREATE_FN, DESTROY_FN)          \
    static const JceCScriptClass jce_c_class_##IDENT = {                      \
        sizeof(JceCScriptClass), (NAME), (CREATE_FN), (DESTROY_FN),

/* The optional handlers.  Write the ones this class has, in any order; each is
 * a named slot (see C99, AND WHY THE MACROS USE DESIGNATED INITIALISERS), and
 * one you do not write stays NULL, which the engine reads as "this class does
 * not define that handler" — a clean no-op, the same thing the Lua path does
 * when lua_getfield finds no function. */
#define JCE_C_ON_START(FN)      .on_start      = (FN),
#define JCE_C_ON_UPDATE(FN)     .on_update     = (FN),
#define JCE_C_ON_DESTROY(FN)    .on_destroy    = (FN),
#define JCE_C_ON_COLLISION(FN)  .on_collision  = (FN),
#define JCE_C_ON_MESSAGE(FN)    .on_message    = (FN),
#define JCE_C_ON_ANIM_EVENT(FN) .on_anim_event = (FN),

#define JCE_C_SCRIPT_CLASS_END() };

/* A named global handler — the C answer to a global Lua function, reached by
 * jce_script_call_named / _num / _str.  Pass NULL for the arities this handler
 * does not take; see JceCppScriptGlobal for why the entity-only form is the
 * fallback when the exact arity is absent. */
#define JCE_C_SCRIPT_GLOBAL(IDENT, NAME, FN_ENTITY, FN_NUM, FN_STR)           \
    static const JceCScriptGlobal jce_c_global_##IDENT = {                    \
        sizeof(JceCScriptGlobal), (NAME), (FN_ENTITY), (FN_NUM), (FN_STR),    \
    };

/* ── The module ──────────────────────────────────────────────────────────
 *
 * A TRAILING NULL IN EACH ARRAY, not counted.  It is what makes a module with
 * no classes, or no globals, legal C: a zero-length array is not, and a module
 * that publishes only globals is a real shape (jce_script_call_named handlers
 * with no per-entity class). */
#define JCE_C_MODULE_BEGIN()                                                  \
    static const JceCScriptClass *const jce_c_module_classes_[] = {

#define JCE_C_MODULE_CLASS(IDENT) &jce_c_class_##IDENT,

#define JCE_C_MODULE_GLOBALS()                                                \
        (const JceCScriptClass *)0                                            \
    };                                                                        \
    static const JceCScriptGlobal *const jce_c_module_globals_[] = {

#define JCE_C_MODULE_GLOBAL(IDENT) &jce_c_global_##IDENT,

/* Emit the descriptor, the in-process accessor, AND the exported entry point.
 *
 * BOTH DOORS FROM ONE SOURCE, exactly as JCE_CPP_MODULE_END does it: a module
 * compiled INTO the game reaches the registry through ACCESSOR() +
 * jce_script_vm_cpp_add_module(), and the very same file built as a shared
 * object is loaded by jce_script_vm_cpp_load_library() through the exported
 * symbol.  A module that had only the accessor would be reachable from its own
 * executable and from nothing else — which is precisely the state that left
 * editor Play unable to run a project's compiled scripts.
 *
 * THE EXPORTED SYMBOL IS jce_cpp_script_module FOR BOTH LANGUAGES, because one
 * loader looks it up and JCE_CPP_MODULE_ENTRY_SYMBOL is the name it looks up.
 * A "jce_c_script_module" would be a second symbol needing a second loader,
 * and the editor's "loads but is NOT A JCE SCRIPT MODULE" diagnostic would
 * then be wrong for half the modules it is asked about.
 *
 * The entry REFUSES an engine whose module-ABI major differs, by returning
 * NULL, rather than handing over a table that would be misread. */
#define JCE_C_MODULE_END(MODULE_NAME, ACCESSOR)                               \
        (const JceCScriptGlobal *)0                                           \
    };                                                                        \
    static const JceCModuleDesc jce_c_module_desc_ = {                        \
        sizeof(JceCModuleDesc),                                               \
        JCE_CPP_MODULE_ABI_VERSION,                                           \
        (MODULE_NAME),                                                        \
        jce_c_module_classes_,                                                \
        (sizeof(jce_c_module_classes_) /                                      \
         sizeof(jce_c_module_classes_[0])) - 1u,                              \
        jce_c_module_globals_,                                                \
        (sizeof(jce_c_module_globals_) /                                      \
         sizeof(jce_c_module_globals_[0])) - 1u,                              \
    };                                                                        \
    const JceCModuleDesc *ACCESSOR(void);                                     \
    const JceCModuleDesc *ACCESSOR(void) { return &jce_c_module_desc_; }      \
    JCE_C_MODULE_ENTRY_

/* ── THE EXPORTED ENTRY IS PER SHARED OBJECT, NOT PER TRANSLATION UNIT ───
 *
 * `jce_cpp_script_module` is ONE external symbol and the loader looks up
 * exactly that name, so a shared object may define it once — and an
 * EXECUTABLE that statically links TWO native script modules gets two
 * definitions of it and does not link:
 *
 *     MSVC : error LNK2005: jce_cpp_script_module already defined in <obj>
 *            fatal error LNK1169: one or more multiply defined symbols found
 *     GNU  : multiple definition of `jce_cpp_script_module`
 *
 * Two statically linked modules is not an exotic arrangement: it is what
 * "ONE MODULE IS ONE LANGUAGE ... a project that wants both publishes TWO
 * modules" above ASKS a five-language project to do, and a shipped game
 * prefers static modules precisely because there is then no plugin to find.
 * elemental_serenity hit it the first time it compiled a C module beside its
 * C++ one, and the whole diagnosis a project gets is a linker error naming a
 * symbol its source never mentions.
 *
 * DEFINE JCE_SCRIPT_MODULE_NO_ENTRY on the compile that goes INTO THE BINARY.
 * The entry is dead code there — a statically linked host reaches the module
 * through the accessor JCE_C_MODULE_END emits and calls
 * jce_script_vm_cpp_add_module() with it, and nothing dlsym()s a symbol out of
 * its own executable.  Leave it UNDEFINED for the shared-object build, where
 * the entry is the only door: suppressing it there produces a module that
 * loads and is then rejected as "not a JCE script module", which is a much
 * worse failure than a link error because it happens after shipping.
 *
 * The default is unchanged — the entry is emitted unless the macro is defined
 * — so no existing module needs editing.  The same macro governs
 * JCE_CPP_MODULE_END; one spelling, because a project that mixes the two
 * languages should not have to know which header emitted the collision. */
#if defined(JCE_SCRIPT_MODULE_NO_ENTRY)
#  define JCE_C_MODULE_ENTRY_ /* suppressed: this module is linked INTO a binary */
#else
#  define JCE_C_MODULE_ENTRY_                                                 \
    JCE_C_MODULE_EXPORT const JceCModuleDesc *                                \
    jce_cpp_script_module(uint32_t engine_abi_version);                       \
    JCE_C_MODULE_EXPORT const JceCModuleDesc *                                \
    jce_cpp_script_module(uint32_t engine_abi_version)                        \
    {                                                                         \
        if (engine_abi_version != JCE_CPP_MODULE_ABI_VERSION)                 \
            return (const JceCModuleDesc *)0;                                 \
        return &jce_c_module_desc_;                                           \
    }
#endif

#if defined(_WIN32)
#  define JCE_C_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define JCE_C_MODULE_EXPORT __attribute__((visibility("default")))
#else
#  define JCE_C_MODULE_EXPORT
#endif

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_VM_C_H */

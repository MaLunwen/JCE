/*
 * jce_script_vm.h — JceScriptVM, the dual of JceScriptHost.
 *
 * JceScriptHost hands capabilities DOWN: the engine fills a table of
 * callbacks and a script calls into them.  JceScriptVM is the mirror.  The
 * engine calls UP into a language runtime through a table the RUNTIME fills,
 * so that a Python or Java script attached to an entity receives on_start /
 * on_update / collisions / messages through exactly the paths Lua uses today.
 *
 * ── THE VTABLE IS NOT INVENTED ───────────────────────────────────────────
 *
 * Every slot below is one of the public `jce_script_*` lifecycle functions
 * from jce_script.h, SIGNATURE COPIED VERBATIM.  Copying rather than
 * designing is the point: an invented vtable would be a second definition of
 * the lifecycle, and two definitions drift.  Because the signatures are
 * identical, today's Lua implementation became the first JceScriptVM with no
 * behaviour change at all — the public functions in jce_script_vm.c are now
 * forwarders and jce_script.c kept its bodies verbatim.
 *
 * Was enforced by (no longer checked — tools/audit/ was removed):
 *   - check_script_vm_parity.py — every public lifecycle
 *     declaration in jce_script.h has a slot of the same name and the same
 *     normalised parameter list, and jce_script.c defines none of them with
 *     external linkage (i.e. nobody can add a 20th lifecycle function that
 *     bypasses the vtable and silently reaches only Lua).
 *   - the `k_public_signature_pin` initialiser in jce_script_vm.c, which
 *     assigns the PUBLIC symbols into the slots.  If a lifecycle signature
 *     changes in jce_script.h and the slot is not changed with it, that
 *     initialiser stops compiling — but only because the pragma block below
 *     makes the compiler's diagnostic fatal.  Read that comment before
 *     removing it: without it the mismatch is a warning and the build is
 *     green.
 *
 * ── THE TWO SLOTS THAT FAIL SILENTLY ─────────────────────────────────────
 *
 * `call_named_num` and `call_named_str` are how UISlider, UIToggle,
 * UIDropdown and UIInputField handlers dispatch (jce_runtime.c), and BOTH
 * return false for "no such global" — indistinguishable from a correctly
 * absent handler.  A VM that omitted them would break every UI callback in
 * the game with no error anywhere.
 *
 * That is why jce_script_vm_register() REQUIRES EVERY SLOT TO BE NON-NULL and
 * refuses the registration otherwise, naming the missing slot in the log.  A
 * runtime that genuinely cannot implement one must supply an explicit no-op —
 * a decision visible in its own source, not a hole nobody can see.
 * *Enforced by:* test_jce_script_vm_abi.c ::
 * test_vm_missing_a_silent_slot_is_refused.
 *
 * ── struct_size FIRST, APPEND ONLY AFTER ─────────────────────────────────
 *
 * Mirroring JceScriptHost, and the direction is reversed and the payload is
 * worse.  The PLUGIN allocates JceScriptVM and the ENGINE reads it, so an
 * engine that copied the table at its own sizeof would read past the end of a
 * plugin built against an older header — and file whatever followed under a
 * FUNCTION POINTER IT THEN CALLS.  jce_script_vm_register() copies
 * min(vm->struct_size, sizeof(JceScriptVM)) over a zeroed table instead.
 * *Enforced by:* test_jce_script_vm_abi.c, modelled on
 * tests/middleware/script/test_jce_script_host_abi.c: it registers a table
 * truncated at the offset of the struct's CURRENT last member with 0xFF after
 * it, and asserts the withheld slot is treated as absent rather than called.
 *
 * NEVER insert a member.  NEVER reorder.  Append only, at the end, and only
 * after the existing tail.
 *
 * check_abi_snapshot.py does NOT enforce that here.  Measured: it records this
 * struct as `opaque JceScriptVM = struct JceScriptVM` — the forward typedef —
 * so it never sees the member list and its ordered-prefix rule, the one that
 * catches a mid-record insertion in JceSceneRenderingSettings, has nothing to
 * compare.  What catches an insertion is that BOTH vtable initialisers are
 * POSITIONAL (`k_public_signature_pin` in jce_script_vm.c, `k_lua_vm` in
 * jce_script.c): shifting the members retypes every slot after the insertion
 * point and the build stops.  An APPEND that forgets the k_vm_slots entry is
 * caught separately, by the compile-time slot-count assertion in
 * jce_script_vm.c.  Both were mutation-tested; do not replace either
 * initialiser with designated initialisers, which would survive a reorder.
 *
 * ── THE HANDLE HEADER IS FROZEN, NOT APPEND-ONLY ─────────────────────────
 *
 * JceScriptVMHeader (below) is embedded at the FRONT of the implementation's
 * own handle, so growing it would move every field the implementation put
 * after it.  Its size is frozen; `engine_flags` is the room reserved for
 * anything the engine later needs.  See the comment on the struct.
 *
 * ── REGISTERING A LANGUAGE — one new directory, nothing else edited ──────
 *
 * Backends are added in parallel and must not collide.  The build seam
 * already exists: scripting/CMakeLists.txt globs every immediate
 * subdirectory's CMakeLists.txt with CONFIGURE_DEPENDS, so a new language is
 * a new directory and no existing CMake file changes.  (The glob pattern is
 * not written out here on purpose: it contains the two characters that end a
 * C block comment, which is how this paragraph first broke the build.)
 * This header is the RUNTIME half of that seam:
 * registration is by call, keyed by a language name the backend owns, into a
 * registry with no central list.
 *
 * Worked example — the whole of a new backend's registration:
 *
 *     // scripting/python/src/jce_script_vm_python.c
 *     #include <jce/middleware/script/jce_script_vm.h>
 *
 *     typedef struct PyScript {
 *         JceScriptVMHeader hdr;   // MUST be first; see JceScriptVMHeader
 *         PyObject         *module;
 *         JceScriptHost     host;  // copied at min(host_size, sizeof)
 *         bool              have_host;
 *     } PyScript;
 *
 *     static JceScript *py_create_sized(const JceScriptHost *host,
 *                                       size_t host_size)
 *     {
 *         PyScript *s = calloc(1, sizeof *s);
 *         if (!s) return NULL;
 *         s->hdr.vm = jce_script_vm_python();   // MUST point at YOUR table
 *         if (host && host_size > 0) {
 *             const size_t n = host_size < sizeof s->host ? host_size
 *                                                         : sizeof s->host;
 *             memcpy(&s->host, host, n);        // never sizeof(*host)
 *             s->have_host = true;
 *         }
 *         return (JceScript *)s;
 *     }
 *
 *     static void py_call_start(JceScript *sc, JceScriptInstance inst) { ... }
 *     // ... one function per slot; NONE may be NULL ...
 *
 *     static const JceScriptVM k_python_vm = {
 *         .struct_size        = sizeof(JceScriptVM),
 *         .language           = "python",
 *         .create_sized       = py_create_sized,
 *         .destroy            = py_destroy,
 *         // ... every remaining slot, explicitly, including the two that
 *         //     fail silently: call_named_num and call_named_str ...
 *     };
 *
 *     const JceScriptVM *jce_script_vm_python(void) { return &k_python_vm; }
 *
 *     // Called once by whoever links or loads this backend.
 *     bool jce_script_vm_python_register(void)
 *     {
 *         if (!jce_script_vm_register(&k_python_vm)) return false;
 *         // Claim the file extensions this language owns.  THIS is what
 *         // makes bob.lua and turret.py coexist in one scene: the runtime
 *         // asks jce_script_vm_language_for_path() which VM a script path
 *         // belongs to, and the answer comes from claims like this one.
 *         return jce_script_vm_register_extension("py", "python");
 *     }
 *
 * Nothing above touches a file another backend author owns: not this header,
 * not jce_script_vm.c, not a shared list, not another backend's directory.
 * The registry is keyed by `language`, and a second registration of the same
 * name is refused rather than silently overriding the first.  The EXTENSION
 * registry works the same way and for the same reason — a sixth backend
 * claims ".rb" from inside its own register() and no engine file changes.
 *
 * Three things a backend MUST do, because each has a failure mode nothing
 * downstream can see:
 *
 *   1. Put JceScriptVMHeader FIRST in your handle and set `hdr.vm` to your
 *      own table before returning.  jce_script_vm_create() verifies it and
 *      REFUSES a handle whose first word is not the table it just dispatched
 *      through, because every later forwarder reads the vtable from there and
 *      would otherwise call through whatever your struct starts with.
 *      *Enforced by:* test_handle_without_the_header_is_refused.
 *
 *   2. Copy JceScriptHost at min(host_size, sizeof yours) over a ZEROED
 *      destination, and null-check every member before calling it.  The host
 *      is CALLER-allocated and grows; `*host` at your own sizeof over-reads.
 *      This is the same ABI fact tools/scriptgen/scriptgen_core.py states for
 *      the binding generators, in the direction the VM sees it.
 *      *Enforced by:* your own test — tests/middleware/script/
 *      test_jce_script_host_abi.c is the model, and it only covers Lua.
 *
 *   3. Fill EVERY slot.  Registration refuses a NULL one.
 *
 *   4. Claim your file extension(s), or nothing authored in your language
 *      will ever be selected.  A registered VM that claims no extension is
 *      reachable only by name (jce_script_vm_create) or through the
 *      JCE_SCRIPT_LANGUAGE override — an entity whose Script component says
 *      `turret.rb` will not find you, and JCE_SCRIPT_LANGUAGE is a
 *      WHOLE-PROCESS setting that a scene with a second language cannot use.
 *      *Enforced by:* tests/middleware/script/test_jce_script_vm_ext.c ::
 *      test_registered_language_without_a_claim_is_not_selected_by_path.
 *
 *      THIS APPLIES EVEN WHEN YOUR "PATH" IS NOT A FILENAME.  The cpp backend
 *      resolves a CLASS in a compiled module and for a long time claimed
 *      nothing, on the reasoning that there was no file to name.  The
 *      reasoning was sound and the conclusion was wrong: routing happens
 *      before instantiation, so its scripts resolved to no language and were
 *      refused before the VM was ever asked.  It now claims ".jcecpp" — an
 *      engine-namespaced extension, chosen so it can never collide with a
 *      real C++ translation unit — and strips it when resolving the class.
 *      If your backend's path is a name rather than a file, do the same:
 *      claim a spelling of your own and resolve through it.
 *
 * ── SELECTING A LANGUAGE PER SCRIPT ──────────────────────────────────────
 *
 * The runtime holds ONE VM PER LANGUAGE, not one VM, and picks per script:
 *
 *     jce_script_vm_language_for_path("assets/turret.py")  ->  "python"
 *     jce_script_vm_language_for_path("assets/bob.lua")    ->  "lua"
 *     jce_script_vm_language_for_path("assets/turret.rb")  ->  NULL
 *
 * A JceScriptInstance is a uint32 whose meaning is PRIVATE TO THE VM THAT
 * ISSUED IT (Lua's is a luaL_ref into its own registry), so handing one VM's
 * instance to another is not an error return — it is a wrong-pointer
 * dereference.  Anything that stores an instance must store the handle that
 * issued it in the same value; the runtime's RtScriptRef
 * (engine/src/application/jce_rt_internal.h) is that value, and there is no
 * `rt->script_vm` for a caller to reach for instead.
 *
 * ── THREADING ────────────────────────────────────────────────────────────
 *
 * A JceScript handle is owned by the thread that created it and may only be
 * used from that thread.  This is CHECKED, in release as well as debug, at
 * every forwarder in jce_script_vm.c; a call from another thread is refused
 * (the same clean no-op / false / 0 those functions already return for bad
 * input) and logged once per handle.
 *
 * It is deliberately NOT phrased as "the main thread".  The engine cannot
 * prove that: jce_thread_is_main() returns TRUE when jce_thread_mark_main()
 * was never called (engine/src/os/core/jce_thread.c:155, documented as
 * intended at engine/include/jce/os/core/jce_thread.h:78-81), and
 * jce_thread_mark_main() has exactly one first-party non-test call site
 * (engine/src/application/jce_engine.c:515) — which unit-test processes never
 * reach, because they never create an engine.  A main-thread assertion would
 * therefore be vacuously true in every test process, and there is no
 * JCE_ASSERT in this repo: the engine uses <assert.h>, which NDEBUG compiles
 * out in release.  jce_thread_current_id() needs no marking, so the
 * owning-thread rule is the one that is true everywhere it is written down.
 * *Enforced by:* test_jce_script_vm_abi.c :: test_worker_thread_is_refused.
 */
#ifndef JCE_SCRIPT_VM_H
#define JCE_SCRIPT_VM_H

#include <jce/middleware/script/jce_script.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A slot filled with a function of the WRONG SIGNATURE is a diagnostic every
 * C compiler emits and no C compiler stops for.  Measured, on the toolchain
 * this ships on: retyping JceScriptVM::call_update's `dt` from float to double
 * produced MSVC C4113 ("differs in parameter lists") at both vtable
 * initialisers — the public signature pin in jce_script_vm.c AND the Lua table
 * in jce_script.c — and the build SUCCEEDED, because /W4 is not /WX here.  A
 * mis-typed slot then reads its arguments off the wrong stack layout at every
 * dispatch.
 *
 * So the pin is only an enforcement if the diagnostic is fatal.  The scope of
 * this pragma is exactly right: it applies to translation units that include
 * this header, which are the ones that build JceScriptVM tables — the engine's
 * two and every backend's.  A backend author gets the hard error for free,
 * without opting in and without knowing this paragraph exists.
 *
 * check_script_vm_parity.py covers what a compiler structurally cannot: a slot
 * that was never added at all. */
#if defined(_MSC_VER)
#  pragma warning(error : 4113)   /* differs in parameter lists           */
#  pragma warning(error : 4133)   /* incompatible types (pointer assign)  */
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic error "-Wincompatible-pointer-types"
#endif

JCE_EXTERN_C_BEGIN

typedef struct JceScriptVM JceScriptVM;

/* The first bytes of every JceScript an implementation returns.
 *
 * SIZE FROZEN — NOT append-only.  Unlike JceScriptVM, this struct is embedded
 * at offset 0 of the IMPLEMENTATION's own handle, so growing it would move
 * every field the implementation placed after it: a plugin compiled against
 * today's header would have its second field overwritten by an engine writing
 * a member that did not exist when the plugin was built.  `engine_flags` is
 * the reserved room for whatever the engine needs later; add nothing else. */
typedef struct JceScriptVMHeader {
    /* Set by the IMPLEMENTATION, before returning from create_sized, to its
     * OWN table — the address it passed to jce_script_vm_register(), which is
     * the only one it can know from inside create_sized.
     *
     * jce_script_vm_create() checks it against that address and then REPOINTS
     * this field at the registry's clamped copy, which is what every forwarder
     * dispatches through for the rest of the handle's life.  (Dispatching
     * through the implementation's own table would read slots past the end of
     * a plugin built against an older header.)  So: write it, do not rely on
     * it staying equal to what you wrote. */
    const JceScriptVM *vm;
    /* Set by the ENGINE after a successful create.  The thread that owns this
     * handle; see THREADING above.  0 means "not yet adopted". */
    uint64_t owner_thread;
    /* Engine-owned scratch.  Implementations must neither read nor write it.
     * Reserved so this struct can gain engine state without changing size. */
    uint64_t engine_flags;
} JceScriptVMHeader;

/* One language runtime, filled by that runtime.
 *
 * struct_size FIRST, APPEND ONLY after.  Read the header comment before
 * touching this struct — the plugin allocates it and the engine reads it, so
 * an over-read here yields a function pointer the engine then calls. */
struct JceScriptVM {
    /* sizeof(JceScriptVM) AS THE IMPLEMENTATION SAW IT.  Not optional: the
     * engine copies min(this, its own sizeof) over a zeroed table, which is
     * the whole reason a backend built against an older header is safe. */
    size_t struct_size;

    /* Stable, NUL-terminated language key ("lua", "python", "java", "cpp").
     * The registry copies it, so it need not outlive the register() call, but
     * it must be unique: a second registration of the same name is refused. */
    const char *language;

    /* ── The 18 lifecycle slots.  Signatures verbatim from jce_script.h. ──
     *
     * jce_script_create is deliberately NOT a slot.  It is the size-less
     * legacy spelling of create_sized, and it is a real exported symbol, not
     * "just a macro": JCE_API at jce_script.h:464, defined at jce_script.c:585
     * (the macro at jce_script.h:467 is guarded on JCE_BUILDING_ENGINE, which
     * engine/CMakeLists.txt:66 sets PRIVATE on every layer, so inside the
     * engine the call resolves to the real function).  A slot for it would be
     * a way for a backend to supply the size-less form and NOT the sized one,
     * losing exactly the short-host protection that
     * tests/middleware/script/test_jce_script_host_abi.c exists to guarantee.
     * Omitting it makes that structurally impossible; jce_script_create stays
     * a public engine-side wrapper that calls create_sized with
     * sizeof(JceScriptHost).
     * *Was enforced by* (no longer checked — tools/audit/ was removed):
     * test_legacy_create_reaches_create_sized_with_our_sizeof,
     * and check_script_vm_parity.py's explicit exclusion entry. */

    JceScript *(*create_sized)(const JceScriptHost *host, size_t host_size);
    void       (*destroy)(JceScript *s);

    JceScriptInstance (*instantiate)(JceScript *s, const char *path,
                                     JceScriptEntity owner);
    JceScriptInstance (*instantiate_source)(JceScript *s, const char *name,
                                            const char *source,
                                            JceScriptEntity owner);

    void (*call_start) (JceScript *s, JceScriptInstance inst);
    void (*call_update)(JceScript *s, JceScriptInstance inst, float dt);
    void (*release)    (JceScript *s, JceScriptInstance inst);

    void (*call_collision)(JceScript *s, JceScriptInstance inst,
                           JceScriptEntity other_entity);
    void (*call_message)(JceScript *s, JceScriptInstance inst,
                         const char *msg_name, double number_arg,
                         const char *str_arg);
    void (*call_anim_event)(JceScript *s, JceScriptInstance inst,
                            uint32_t id, const char *name,
                            float f0, float f1, int i0);

    /* The three global-handler dispatchers.  The latter two are the ones
     * whose absence is invisible — see THE TWO SLOTS THAT FAIL SILENTLY. */
    bool (*call_named)(JceScript *s, const char *fn_name,
                       JceScriptEntity arg_entity);
    bool (*call_named_num)(JceScript *s, const char *fn_name,
                           JceScriptEntity arg_entity, double value);
    bool (*call_named_str)(JceScript *s, const char *fn_name,
                           JceScriptEntity arg_entity, const char *str);

    int  (*instance_count)(const JceScript *s);
    void (*update_coroutines)(JceScript *s, float dt);

    JceScriptModule (*compile_module)(JceScript *s, const char *name,
                                      const char *source, size_t len);
    void (*rebind_instance)(JceScript *s, JceScriptInstance inst,
                            JceScriptModule mod);
    void (*release_module)(JceScript *s, JceScriptModule mod);

    /* ── APPEND ONLY BELOW THIS LINE ──────────────────────────────────────
     * A new slot goes here and nowhere else, and every existing member keeps
     * its offset.  Add the matching public function to jce_script.h and the
     * forwarder to jce_script_vm.c in the SAME commit, or
     * check_script_vm_parity.py fails. */
};

/* Smallest struct_size a registration may declare: a table that does not
 * reach `create_sized` cannot make a VM at all, and reading `language` out of
 * something shorter would be the very over-read this API exists to prevent. */
#define JCE_SCRIPT_VM_MIN_STRUCT_SIZE \
    (offsetof(JceScriptVM, create_sized) + sizeof(JceScript *(*)(const JceScriptHost *, size_t)))

/* Longest language key the registry stores, including the NUL. */
#define JCE_SCRIPT_VM_LANGUAGE_MAX 32

/* Largest number of languages that may be registered at once.  Four are
 * planned (lua, python, java, cpp); the rest is headroom for probes and for
 * an application that registers a runtime of its own. */
#define JCE_SCRIPT_VM_MAX 16

/* Longest file extension the registry stores, including the NUL and NOT
 * counting the dot ("lua", "py", "class", "java"). */
#define JCE_SCRIPT_VM_EXTENSION_MAX 16

/* Largest number of extension claims.  More than JCE_SCRIPT_VM_MAX because a
 * single language legitimately claims several (java: .java and .class). */
#define JCE_SCRIPT_VM_EXTENSION_COUNT_MAX 32

/* Register a language runtime.  The table is COPIED (clamped to
 * min(vm->struct_size, sizeof(JceScriptVM)) over a zeroed destination), so it
 * need not stay alive; the `language` string is copied too.
 *
 * Refuses, returning false and logging the reason, when:
 *   - `vm` is NULL, or vm->struct_size is below
 *     JCE_SCRIPT_VM_MIN_STRUCT_SIZE (including 0);
 *   - `language` is NULL/empty, or does not fit
 *     JCE_SCRIPT_VM_LANGUAGE_MAX;
 *   - a language of that name is already registered;
 *   - ANY slot the engine knows about is NULL after the clamped copy — see
 *     THE TWO SLOTS THAT FAIL SILENTLY.  The log names the slot.
 *   - the registry is full (JCE_SCRIPT_VM_MAX).
 *
 * Idempotent only in the sense that a second call with the same name fails;
 * it never replaces a live registration, because handles created through the
 * first one hold a pointer into the registry. */
JCE_API bool JCE_CALL jce_script_vm_register(const JceScriptVM *vm);

/* The registered table for `language`, or NULL.  The returned pointer is the
 * registry's COPY and stays valid for the life of the process. */
JCE_API const JceScriptVM *JCE_CALL jce_script_vm_find(const char *language);

/* Number of registered languages, and the name at `index` (NULL when out of
 * range).  Diagnostics and editor UI; the order is registration order. */
JCE_API int         JCE_CALL jce_script_vm_count(void);
JCE_API const char *JCE_CALL jce_script_vm_language_at(int index);

/* The language a live handle belongs to, or NULL.  Reads the handle header. */
JCE_API const char *JCE_CALL jce_script_vm_language_of(const JceScript *s);

/* ── THE EXTENSION REGISTRY ───────────────────────────────────────────────
 *
 * Claim a file extension for an ALREADY-REGISTERED language.  `extension` may
 * be written with or without the leading dot ("py" and ".py" are the same
 * claim) and is stored lowercased; matching is case-insensitive, so
 * `Turret.PY` resolves.
 *
 * A backend calls this from its own register() function (see REGISTERING A
 * LANGUAGE).  There is deliberately no central table: adding a sixth language
 * must not require editing an engine file, which is the same rule
 * jce_script_vm_register() follows and the reason there is no list of
 * backends anywhere.
 *
 * Refuses, returning false and logging the reason, when:
 *   - `extension` is NULL, empty, longer than JCE_SCRIPT_VM_EXTENSION_MAX,
 *     or contains a path separator or an interior dot (".tar.gz" is not an
 *     extension this registry can match, and accepting it would claim
 *     something that never resolves);
 *   - `language` is NULL/empty or is NOT REGISTERED.  Claiming an extension
 *     for a language the process cannot run would turn "backend not linked"
 *     into "extension resolves to a VM that does not exist", which is the
 *     harder of the two to diagnose and the one this ordering removes;
 *   - the extension is already claimed — by any language, INCLUDING the same
 *     one.  A second claim is refused rather than silently accepted for the
 *     same reason a second language registration is: the first claim may
 *     already have decided which VM a live script is running in.
 *   - the extension registry is full (JCE_SCRIPT_VM_EXTENSION_COUNT_MAX). */
JCE_API bool JCE_CALL jce_script_vm_register_extension(const char *extension,
                                                       const char *language);

/* The language claiming `path`'s extension, or NULL when nobody claims it —
 * which includes a path with no extension at all and a path whose extension
 * belongs to a backend this executable did not link.  The returned pointer is
 * the registry's copy of the language name and stays valid for the life of the
 * process.
 *
 * A NULL here is NOT "this file has no language": it is "no VM IN THIS
 * PROCESS claims it".  The build-independent answer is
 * jce_asset_script_language_from_ext() in <jce/resource/jce_asset_format.h>,
 * and a consumer that reports a refusal should consult both — an unlinked
 * backend and an unknown extension have different fixes.
 *
 * Only the final path component is considered, so "scripts.v2/bob" has NO
 * extension and "scripts.v2/bob.lua" has "lua". */
JCE_API const char *JCE_CALL jce_script_vm_language_for_path(const char *path);

/* Number of extension claims, and the claim at `index`: the extension without
 * its dot, and the language that claims it (both NULL when out of range).
 * Diagnostics — this is what a "no VM claims '.py'" message prints so the
 * reader can tell an unknown language from an unlinked backend. */
JCE_API int         JCE_CALL jce_script_vm_extension_count(void);
JCE_API const char *JCE_CALL jce_script_vm_extension_at(int index);
JCE_API const char *JCE_CALL jce_script_vm_extension_language_at(int index);

/* Create a VM in a named language.  `host_size` carries the same meaning as
 * in jce_script_create_sized: pass sizeof(JceScriptHost).
 *
 * Returns NULL when the language is not registered, when the implementation's
 * create_sized returns NULL, or when the returned handle does not begin with
 * a JceScriptVMHeader pointing at the table we dispatched through — that last
 * one is refused rather than used, because every later forwarder would read a
 * vtable pointer out of whatever the implementation's struct starts with.
 *
 * The handle is adopted by the CALLING thread; see THREADING. */
JCE_API JceScript *JCE_CALL jce_script_vm_create(const char *language,
                                                 const JceScriptHost *host,
                                                 size_t host_size);

/* The name of the language jce_script_create / jce_script_create_sized use.
 * "lua" — the built-in, and the only registration the engine performs for
 * itself.  Every other language registers itself; see REGISTERING A LANGUAGE.
 *
 * It is NOT a fallback for an unresolved path.  Nothing in the engine turns
 * "no VM claims this extension" into "run it as Lua": that would load a
 * turret.py as a Lua chunk and fail with a syntax error attributed to the
 * script rather than to the missing backend.  See rt_script_language_for in
 * engine/src/application/jce_rt_script.c. */
#define JCE_SCRIPT_VM_DEFAULT_LANGUAGE "lua"

/* The engine registers this claim for the built-in when the built-in is
 * registered, so a stock build resolves .lua with no application wiring at
 * all.  Named here rather than spelled inline so the header, the registry and
 * the tests cannot disagree about it. */
#define JCE_SCRIPT_VM_DEFAULT_EXTENSION "lua"

/* The built-in Lua implementation, registered lazily on first use of any
 * entry point in this header or in jce_script.h.  Exposed so an app can name
 * it explicitly and so a backend author has a complete worked implementation
 * to read: engine/src/middleware/script/jce_script.c. */
JCE_API const JceScriptVM *JCE_CALL jce_script_vm_lua(void);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_VM_H */

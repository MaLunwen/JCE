/*
 * jce_script_vm.c — the JceScriptVM registry and the public lifecycle
 * forwarders.
 *
 * This translation unit owns every public `jce_script_*` lifecycle symbol.
 * Each one is a forwarder: it finds the vtable in the handle's
 * JceScriptVMHeader and calls the slot.  The Lua bodies those functions used
 * to have now live in jce_script.c behind `static` linkage and are reached
 * through jce_script_vm_lua()'s table — which is what makes "today's
 * jce_script.c became the first JceScriptVM with zero behaviour change" a
 * mechanical claim rather than an aspiration.
 *
 * There is deliberately NO Lua in this file.  The only thing it knows about
 * the built-in is the name of the function that returns its table.
 *
 * Read engine/include/jce/middleware/script/jce_script_vm.h before editing:
 * the ABI rules, the two silently-failing slots, the frozen handle header and
 * the threading decision are all stated there.
 */

#include <jce/middleware/script/jce_script_vm.h>

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>

#include <stddef.h>
#include <string.h>

#define LOG_TAG "script"

/* ── The slot table ───────────────────────────────────────────────────────
 *
 * Every function-pointer slot, by offset and by name, so that a refused
 * registration can say WHICH slot was NULL instead of "something is missing".
 * The two that matter most are in here for the reason the header spells out:
 * call_named_num and call_named_str fail silently when absent.
 *
 * The completeness assertion below is what keeps this list honest.  Appending
 * a slot to JceScriptVM without adding it here would leave the new slot
 * unchecked — the exact "immune mutation with a silent reason" this repo
 * keeps rediscovering — so the sizes are compared at COMPILE time. */
typedef struct {
    size_t      offset;
    const char *name;
} VmSlot;

#define VM_SLOT(member) { offsetof(JceScriptVM, member), #member }

static const VmSlot k_vm_slots[] = {
    VM_SLOT(create_sized),
    VM_SLOT(destroy),
    VM_SLOT(instantiate),
    VM_SLOT(instantiate_source),
    VM_SLOT(call_start),
    VM_SLOT(call_update),
    VM_SLOT(release),
    VM_SLOT(call_collision),
    VM_SLOT(call_message),
    VM_SLOT(call_anim_event),
    VM_SLOT(call_named),
    VM_SLOT(call_named_num),
    VM_SLOT(call_named_str),
    VM_SLOT(instance_count),
    VM_SLOT(update_coroutines),
    VM_SLOT(compile_module),
    VM_SLOT(rebind_instance),
    VM_SLOT(release_module),
    VM_SLOT(call_fixed_update),   /* APPENDED -- see jce_script_vm.h */
};

#define JCE_SCRIPT_VM_SLOT_COUNT \
    (sizeof(k_vm_slots) / sizeof(k_vm_slots[0]))

/* Compile-time proof that k_vm_slots covers the whole tail of JceScriptVM.
 * Every member from create_sized onward is a function pointer, so the byte
 * span divided by one pointer's size IS the slot count.  Append a slot and
 * forget this table and the build stops here, naming the type.
 * (C99 has no _Static_assert; a negative array bound is the portable form.) */
typedef char jce_script_vm_slot_table_covers_every_slot[
    (JCE_SCRIPT_VM_SLOT_COUNT ==
     (sizeof(JceScriptVM) - offsetof(JceScriptVM, create_sized)) /
         sizeof(void (*)(void))) ? 1 : -1];

/* ── The public signature pin ─────────────────────────────────────────────
 *
 * Assigning the PUBLIC lifecycle symbols into the slots proves the slot types
 * and the public prototypes are the same types.  If a signature changes in
 * jce_script.h and JceScriptVM is not changed with it, initialising this
 * object is a constraint violation and the build fails HERE — at the one
 * place that names both halves — instead of silently narrowing an argument at
 * every dispatch.
 *
 * It is never called and never dispatched through; calling it would recurse.
 * check_script_vm_parity.py covers the other half (a slot that is MISSING
 * rather than mistyped, which an initialiser cannot see). */
static const JceScriptVM k_public_signature_pin = {
    sizeof(JceScriptVM),
    "!pin",
    jce_script_create_sized,
    jce_script_destroy,
    jce_script_instantiate,
    jce_script_instantiate_source,
    jce_script_call_start,
    jce_script_call_update,
    jce_script_release,
    jce_script_call_collision,
    jce_script_call_message,
    jce_script_call_anim_event,
    jce_script_call_named,
    jce_script_call_named_num,
    jce_script_call_named_str,
    jce_script_instance_count,
    jce_script_update_coroutines,
    jce_script_compile_module,
    jce_script_rebind_instance,
    jce_script_release_module,
    jce_script_call_fixed_update,   /* APPENDED -- see the header */
};

/* ── The registry ─────────────────────────────────────────────────────────
 *
 * Registration is a startup-time operation and is NOT thread-safe: register
 * every language before the first jce_script_create*.  Lookup after that is
 * read-only and safe.  The table is a fixed array rather than a growable one
 * because jce_script_vm_find() returns a pointer INTO it that handles keep
 * for their whole life — a realloc would dangle every live handle's vtable. */
typedef struct {
    char        language[JCE_SCRIPT_VM_LANGUAGE_MAX];
    JceScriptVM vm;
    /* The address the implementation passed to register().  An implementation
     * sets hdr.vm to its OWN table — the only address it can know — while the
     * engine dispatches through the clamped copy above.  Keeping the origin
     * is what lets jce_script_vm_create() tell "the header is really there"
     * from "the first word of this struct happens to be a pointer". */
    const JceScriptVM *origin;
} VmEntry;

static VmEntry g_vms[JCE_SCRIPT_VM_MAX];
static int     g_vm_count;
static bool    g_builtin_registered;

/* ── The extension registry ───────────────────────────────────────────────
 *
 * A SECOND table, deliberately not a member of JceScriptVM.
 *
 * A `const char *extensions` member would have to be appended after
 * release_module, and everything from create_sized onward is a function
 * pointer — which is exactly what the compile-time completeness assertion
 * above measures (byte span / sizeof(fn ptr) == slot count).  A data member
 * in that span would either break that assertion or force it to be weakened,
 * and it is the assertion that makes "you appended a slot and forgot
 * k_vm_slots" a build failure.  So the claim is a CALL, like the
 * registration itself, and for the same reason: no central list, nothing an
 * engine file has to learn about a sixth language. */
typedef struct {
    char        extension[JCE_SCRIPT_VM_EXTENSION_MAX];  /* lowercase, no dot */
    const char *language;   /* points INTO g_vms[].language — stable for the
                             * process, which is why claims are only accepted
                             * for an already-registered language. */
} VmExtEntry;

static VmExtEntry g_vm_exts[JCE_SCRIPT_VM_EXTENSION_COUNT_MAX];
static int        g_vm_ext_count;

static char vm_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* The one hard-wired registration: the built-in Lua runtime.  Every other
 * language registers itself (see REGISTERING A LANGUAGE in the header), which
 * is why there is no list here for three parallel authors to collide on. */
static void vm_register_builtin_once(void)
{
    if (g_builtin_registered) return;
    g_builtin_registered = true;      /* set first: register() cannot recurse */
    /* The pin exists to be type-checked, not to be called; touching it here
     * keeps it out of reach of an unused-object warning without giving it
     * external linkage. */
    (void)k_public_signature_pin.struct_size;
    if (jce_script_vm_register(jce_script_vm_lua())) {
        /* The built-in claims its own extension the same way every backend
         * does — through the public call, not through a private back door —
         * so the claim path is exercised by every process that touches
         * scripting at all, not only by one with a plugin linked in. */
        (void)jce_script_vm_register_extension(JCE_SCRIPT_VM_DEFAULT_EXTENSION,
                                               JCE_SCRIPT_VM_DEFAULT_LANGUAGE);
    }
}

bool JCE_CALL jce_script_vm_register(const JceScriptVM *vm)
{
    VmEntry *e;
    size_t   copy;
    size_t   i;

    if (!vm) {
        LOG_ERROR(LOG_TAG, "script VM registration refused: NULL table");
        return false;
    }
    /* struct_size is the FIRST member, so it is the one field readable from a
     * table shorter than ours.  Everything below this line depends on it. */
    if (vm->struct_size < (size_t)JCE_SCRIPT_VM_MIN_STRUCT_SIZE) {
        LOG_ERROR(LOG_TAG,
                  "script VM registration refused: struct_size %zu is below "
                  "the minimum %zu (a table that short cannot even hold "
                  "create_sized)",
                  vm->struct_size, (size_t)JCE_SCRIPT_VM_MIN_STRUCT_SIZE);
        return false;
    }
    if (!vm->language || !vm->language[0]) {
        LOG_ERROR(LOG_TAG, "script VM registration refused: no language name");
        return false;
    }
    if (strlen(vm->language) >= (size_t)JCE_SCRIPT_VM_LANGUAGE_MAX) {
        LOG_ERROR(LOG_TAG,
                  "script VM registration refused: language name '%s' is "
                  "longer than %d bytes",
                  vm->language, JCE_SCRIPT_VM_LANGUAGE_MAX - 1);
        return false;
    }
    for (i = 0; i < (size_t)g_vm_count; ++i) {
        if (strcmp(g_vms[i].language, vm->language) == 0) {
            LOG_ERROR(LOG_TAG,
                      "script VM registration refused: '%s' is already "
                      "registered (live handles hold a pointer into the "
                      "registry, so a replacement would repoint them)",
                      vm->language);
            return false;
        }
    }
    if (g_vm_count >= JCE_SCRIPT_VM_MAX) {
        LOG_ERROR(LOG_TAG,
                  "script VM registration refused: '%s' does not fit (%d "
                  "languages already registered)",
                  vm->language, g_vm_count);
        return false;
    }

    e = &g_vms[g_vm_count];
    memset(e, 0, sizeof(*e));

    /* THE CLAMPED COPY.  The plugin allocated this table and the engine is
     * reading it, so copying at OUR sizeof would read past the end of a
     * plugin built against an older header and file the bytes that followed
     * under a FUNCTION POINTER WE THEN CALL.  min() over a zeroed
     * destination: slots the plugin never knew about stay NULL, and the
     * required-slot scan below rejects the table if any slot WE know about is
     * one of them.  A table LONGER than ours (plugin newer than the engine)
     * is the mirror case — copy what we understand, ignore the tail. */
    copy = vm->struct_size < sizeof(e->vm) ? vm->struct_size : sizeof(e->vm);
    memcpy(&e->vm, vm, copy);
    /* Record what we actually hold, not what the plugin claimed: a later
     * reader of this entry must not believe in members we did not copy. */
    e->vm.struct_size = copy;

    /* Every slot must be present.  A NULL call_named_num or call_named_str
     * would break every UISlider / UIToggle / UIDropdown / UIInputField
     * handler in the game and return false — which is exactly what a
     * correctly absent handler returns, so nothing anywhere would notice.
     * Refusing the whole registration is the only signal that cannot be
     * mistaken for normal operation. */
    for (i = 0; i < JCE_SCRIPT_VM_SLOT_COUNT; ++i) {
        const void *const *slot =
            (const void *const *)((const char *)&e->vm + k_vm_slots[i].offset);
        if (*slot == NULL) {
            LOG_ERROR(LOG_TAG,
                      "script VM registration refused: '%s' leaves slot '%s' "
                      "NULL. Every slot must be filled; supply an explicit "
                      "no-op if the runtime cannot implement it.",
                      vm->language, k_vm_slots[i].name);
            memset(e, 0, sizeof(*e));
            return false;
        }
    }

    /* The language key is copied because the caller's string may live in a
     * plugin image that gets unloaded.  Length was bounded above. */
    memcpy(e->language, vm->language, strlen(vm->language) + 1u);
    e->vm.language = e->language;
    e->origin      = vm;

    ++g_vm_count;
    LOG_INFO(LOG_TAG, "script VM registered: '%s' (%zu slot(s) verified)",
             e->language, (size_t)JCE_SCRIPT_VM_SLOT_COUNT);
    return true;
}

static VmEntry *vm_entry_find(const char *language)
{
    int i;
    vm_register_builtin_once();
    if (!language || !language[0]) return NULL;
    for (i = 0; i < g_vm_count; ++i) {
        if (strcmp(g_vms[i].language, language) == 0)
            return &g_vms[i];
    }
    return NULL;
}

const JceScriptVM *JCE_CALL jce_script_vm_find(const char *language)
{
    const VmEntry *e = vm_entry_find(language);
    return e ? &e->vm : NULL;
}

int JCE_CALL jce_script_vm_count(void)
{
    vm_register_builtin_once();
    return g_vm_count;
}

const char *JCE_CALL jce_script_vm_language_at(int index)
{
    vm_register_builtin_once();
    if (index < 0 || index >= g_vm_count) return NULL;
    return g_vms[index].language;
}

/* ── Extension claims ─────────────────────────────────────────────────────
 *
 * Normalise `in` into `out`: drop one optional leading dot, lowercase, and
 * refuse anything that could not be the tail of a filename.  Returns false
 * (leaving `out` unspecified) for every refusal the header lists.
 *
 * The interior-dot refusal is not pedantry: jce_script_vm_language_for_path()
 * matches the text after the LAST dot, so a claim of "tar.gz" could never be
 * matched by anything.  Accepting it would register a claim that is silently
 * unreachable — the shape of failure this whole file is built to refuse. */
static bool vm_ext_normalise(const char *in, char out[JCE_SCRIPT_VM_EXTENSION_MAX])
{
    size_t n, i;

    if (!in) return false;
    if (in[0] == '.') ++in;                     /* ".py" and "py" are one claim */
    n = strlen(in);
    if (n == 0 || n >= (size_t)JCE_SCRIPT_VM_EXTENSION_MAX) return false;
    for (i = 0; i < n; ++i) {
        const char c = in[i];
        if (c == '.' || c == '/' || c == '\\' || c == ' ' || c == '\t')
            return false;
        out[i] = vm_lower(c);
    }
    out[n] = '\0';
    return true;
}

bool JCE_CALL jce_script_vm_register_extension(const char *extension,
                                               const char *language)
{
    char      norm[JCE_SCRIPT_VM_EXTENSION_MAX];
    VmEntry  *owner;
    int       i;

    if (!vm_ext_normalise(extension, norm)) {
        LOG_ERROR(LOG_TAG,
                  "script extension claim refused: '%s' is not a usable file "
                  "extension (empty, longer than %d bytes, or containing a "
                  "path separator or an interior dot)",
                  extension ? extension : "(null)",
                  JCE_SCRIPT_VM_EXTENSION_MAX - 1);
        return false;
    }
    /* vm_entry_find() registers the built-in on first use, so a claim made
     * before anything else has touched scripting still sees "lua". */
    owner = vm_entry_find(language);
    if (!owner) {
        LOG_ERROR(LOG_TAG,
                  "script extension claim refused: '.%s' names language '%s', "
                  "which is NOT REGISTERED. Register the VM first — a claim "
                  "pointing at a language this executable cannot run would "
                  "turn 'backend not linked' into 'extension resolves to a VM "
                  "that does not exist'.",
                  norm, language ? language : "(null)");
        return false;
    }
    for (i = 0; i < g_vm_ext_count; ++i) {
        if (strcmp(g_vm_exts[i].extension, norm) == 0) {
            LOG_ERROR(LOG_TAG,
                      "script extension claim refused: '.%s' is already "
                      "claimed by '%s' (claimed second by '%s'). The first "
                      "claim may already have decided which VM a live script "
                      "is running in, so it is never replaced.",
                      norm, g_vm_exts[i].language, owner->language);
            return false;
        }
    }
    if (g_vm_ext_count >= JCE_SCRIPT_VM_EXTENSION_COUNT_MAX) {
        LOG_ERROR(LOG_TAG,
                  "script extension claim refused: '.%s' does not fit (%d "
                  "extensions already claimed)",
                  norm, g_vm_ext_count);
        return false;
    }

    memcpy(g_vm_exts[g_vm_ext_count].extension, norm, strlen(norm) + 1u);
    /* Point at the REGISTRY's copy of the name, not the caller's: the caller
     * may live in a plugin image that gets unloaded, and the registry entry
     * outlives the process's interest in it. */
    g_vm_exts[g_vm_ext_count].language = owner->language;
    ++g_vm_ext_count;
    LOG_INFO(LOG_TAG, "script extension claimed: '.%s' -> '%s'",
             norm, owner->language);
    return true;
}

const char *JCE_CALL jce_script_vm_language_for_path(const char *path)
{
    const char *base;
    const char *dot;
    char        norm[JCE_SCRIPT_VM_EXTENSION_MAX];
    size_t      i;
    int         j;

    vm_register_builtin_once();
    if (!path || !path[0]) return NULL;

    /* Only the LAST path component has an extension.  "scripts.v2/bob" has
     * none; reading the directory's dot would resolve it to whatever claims
     * "v2/bob". */
    base = path;
    for (i = 0; path[i]; ++i)
        if (path[i] == '/' || path[i] == '\\') base = path + i + 1;
    if (!base[0]) return NULL;                  /* trailing separator */

    dot = NULL;
    for (i = 0; base[i]; ++i)
        if (base[i] == '.') dot = base + i;
    if (!dot || !dot[1]) return NULL;           /* no extension, or "bob." */

    if (!vm_ext_normalise(dot, norm)) return NULL;   /* too long to be a claim */

    for (j = 0; j < g_vm_ext_count; ++j)
        if (strcmp(g_vm_exts[j].extension, norm) == 0)
            return g_vm_exts[j].language;
    return NULL;
}

int JCE_CALL jce_script_vm_extension_count(void)
{
    vm_register_builtin_once();
    return g_vm_ext_count;
}

const char *JCE_CALL jce_script_vm_extension_at(int index)
{
    vm_register_builtin_once();
    if (index < 0 || index >= g_vm_ext_count) return NULL;
    return g_vm_exts[index].extension;
}

const char *JCE_CALL jce_script_vm_extension_language_at(int index)
{
    vm_register_builtin_once();
    if (index < 0 || index >= g_vm_ext_count) return NULL;
    return g_vm_exts[index].language;
}

/* ── Handle plumbing ──────────────────────────────────────────────────────
 *
 * A JceScript* is whatever the implementation allocated, and it BEGINS with a
 * JceScriptVMHeader.  C guarantees a pointer to a struct is a pointer to its
 * first member, so this cast is the whole dispatch mechanism. */
/* One accessor, taking the const-qualified handle every caller can produce.
 * The header it returns is NOT const: `owner_thread` and `engine_flags` are
 * engine-owned scratch that the engine writes even on the one const-qualified
 * entry point (jce_script_instance_count), and nothing else in the handle is
 * reachable from this file. */
static JceScriptVMHeader *vm_header(const JceScript *s)
{
    union { const JceScript *in; JceScriptVMHeader *out; } u;
    u.in = s;
    return u.out;
}

const char *JCE_CALL jce_script_vm_language_of(const JceScript *s)
{
    const JceScriptVMHeader *h;
    if (!s) return NULL;
    h = vm_header(s);
    return h->vm ? h->vm->language : NULL;
}

/* THREADING — see the header.  The rule is "the thread that created the
 * handle", not "the main thread", because jce_thread_is_main() returns true
 * when jce_thread_mark_main() was never called and no unit-test process ever
 * calls it, which would make a main-thread check vacuously true exactly where
 * it is most needed.  jce_thread_current_id() needs no marking, so this check
 * is live in every process AND in release.  It predates
 * <jce/os/core/jce_assert.h> (2026-09-20) and is deliberately NOT rewritten
 * onto JCE_ENSURE: the shape is the same -- check, log, refuse, never abort --
 * but the dedup is not.  JCE_ENSURE remembers a SITE; this remembers a
 * HANDLE, which is what keeps a per-frame dispatch from the wrong thread from
 * turning the log into the failure while a second offending handle still gets
 * its own line.
 *
 * Returns true when the call may proceed.  A refusal is the same clean no-op
 * every one of these functions already performs for a NULL handle, plus one
 * log line per handle — a per-frame dispatch from the wrong thread must not
 * turn the log into the failure. */
static bool vm_thread_ok(const JceScript *s, const char *what)
{
    JceScriptVMHeader *h = vm_header(s);
    uint64_t           now;

    if (h->owner_thread == 0u) return true;   /* never adopted; nothing to check */
    now = jce_thread_current_id();
    if (now == h->owner_thread) return true;

    if ((h->engine_flags & 1u) == 0u) {
        h->engine_flags |= 1u;    /* engine-owned scratch; complain once */
        LOG_ERROR(LOG_TAG,
                  "jce_script_%s called from thread %llu but this VM is owned "
                  "by thread %llu — refused. A JceScript handle may only be "
                  "used from the thread that created it.",
                  what ? what : "?",
                  (unsigned long long)now,
                  (unsigned long long)h->owner_thread);
    }
    return false;
}

/* The vtable for a handle, or NULL when the call must be refused.  Every
 * forwarder starts here, and every forwarder tolerates NULL exactly the way
 * it did before this file existed. */
static const JceScriptVM *vm_of(const JceScript *s, const char *what)
{
    const JceScriptVMHeader *h;
    if (!s) return NULL;
    h = vm_header(s);
    if (!h->vm) return NULL;
    if (!vm_thread_ok(s, what)) return NULL;
    return h->vm;
}

JceScript *JCE_CALL jce_script_vm_create(const char *language,
                                         const JceScriptHost *host,
                                         size_t host_size)
{
    VmEntry           *e = vm_entry_find(language);
    JceScript         *s;
    JceScriptVMHeader *h;

    if (!e) {
        LOG_ERROR(LOG_TAG, "no script VM registered for language '%s'",
                  language ? language : "(null)");
        return NULL;
    }

    s = e->vm.create_sized(host, host_size);
    if (!s) return NULL;

    /* The handle MUST begin with a JceScriptVMHeader whose `vm` names the
     * implementation we just dispatched through — either its own table (the
     * address it passed to register(), which is the only one it can know) or
     * the registry's copy.  Anything else means the header is not actually at
     * offset 0, and every later forwarder would read a vtable pointer out of
     * whatever the implementation's struct starts with: a call through
     * arbitrary bytes.  Refuse the handle instead of using it. */
    h = vm_header(s);
    if (h->vm != e->origin && h->vm != &e->vm) {
        LOG_ERROR(LOG_TAG,
                  "script VM '%s' returned a handle that does not begin with "
                  "a JceScriptVMHeader pointing at its own table — refused. "
                  "Put JceScriptVMHeader FIRST in your handle struct and set "
                  "hdr.vm before returning.",
                  e->language);
        e->vm.destroy(s);
        return NULL;
    }

    /* Repoint at the REGISTRY's copy for the rest of the handle's life.  The
     * implementation's own table may be shorter than ours (older plugin), and
     * dispatching through it would read slots past its end — the very
     * over-read the clamped copy exists to prevent.  The copy is the one whose
     * every slot was verified non-NULL. */
    h->vm = &e->vm;

    /* Adopt the handle for this thread; see vm_thread_ok. */
    h->owner_thread = jce_thread_current_id();
    h->engine_flags = 0u;
    return s;
}

/* ── The public lifecycle surface ─────────────────────────────────────────
 *
 * Everything below is a forwarder.  Behaviour for every input these functions
 * already accepted is unchanged: a NULL handle is still the same no-op, the
 * same return value, the same absence of a log line.  The Lua bodies moved to
 * jce_script.c's static functions untouched.
 *
 * Do NOT implement a new lifecycle function here directly.  Add the slot to
 * JceScriptVM, the declaration to jce_script.h and a forwarder here, all in
 * one commit; check_script_vm_parity.py fails when those three disagree. */

JceScript *jce_script_create_sized(const JceScriptHost *host, size_t host_size)
{
    return jce_script_vm_create(JCE_SCRIPT_VM_DEFAULT_LANGUAGE, host,
                                host_size);
}

JceScript *jce_script_create(const JceScriptHost *host)
{
    /* Legacy entry point.  A real exported symbol (not "just a macro" — the
     * shim at jce_script.h:467 is guarded on JCE_BUILDING_ENGINE, which
     * engine/CMakeLists.txt:66 sets PRIVATE on every layer, so in-engine
     * callers land here), kept so binaries already linked against it keep
     * resolving.  It is NOT a JceScriptVM slot: see the note on the struct. */
    return jce_script_create_sized(host, sizeof(JceScriptHost));
}

void jce_script_destroy(JceScript *s)
{
    const JceScriptVM *vm = vm_of(s, "destroy");
    if (!vm) return;
    vm->destroy(s);
}

JceScriptInstance jce_script_instantiate(JceScript *s, const char *path,
                                         JceScriptEntity owner)
{
    const JceScriptVM *vm = vm_of(s, "instantiate");
    if (!vm) return 0;
    return vm->instantiate(s, path, owner);
}

JceScriptInstance jce_script_instantiate_source(JceScript *s, const char *name,
                                                const char *source,
                                                JceScriptEntity owner)
{
    const JceScriptVM *vm = vm_of(s, "instantiate_source");
    if (!vm) return 0;
    return vm->instantiate_source(s, name, source, owner);
}

void jce_script_call_start(JceScript *s, JceScriptInstance inst)
{
    const JceScriptVM *vm = vm_of(s, "call_start");
    if (!vm) return;
    vm->call_start(s, inst);
}

void jce_script_call_update(JceScript *s, JceScriptInstance inst, float dt)
{
    const JceScriptVM *vm = vm_of(s, "call_update");
    if (!vm) return;
    vm->call_update(s, inst, dt);
}

void jce_script_call_fixed_update(JceScript *s, JceScriptInstance inst,
                                  float dt)
{
    const JceScriptVM *vm = vm_of(s, "call_fixed_update");
    if (!vm) return;
    /* The NULL check is belt-and-braces, not tolerance: jce_script_vm_register
     * refuses any table with a NULL slot, so a registered VM always has this
     * one.  It costs a predictable branch and turns a hypothetical zero into a
     * missed callback rather than a jump through it. */
    if (!vm->call_fixed_update) return;
    vm->call_fixed_update(s, inst, dt);
}

void jce_script_release(JceScript *s, JceScriptInstance inst)
{
    const JceScriptVM *vm = vm_of(s, "release");
    if (!vm) return;
    vm->release(s, inst);
}

void jce_script_call_collision(JceScript *s, JceScriptInstance inst,
                               JceScriptEntity other_entity)
{
    const JceScriptVM *vm = vm_of(s, "call_collision");
    if (!vm) return;
    vm->call_collision(s, inst, other_entity);
}

void jce_script_call_message(JceScript *s, JceScriptInstance inst,
                             const char *msg_name, double number_arg,
                             const char *str_arg)
{
    const JceScriptVM *vm = vm_of(s, "call_message");
    if (!vm) return;
    vm->call_message(s, inst, msg_name, number_arg, str_arg);
}

void jce_script_call_anim_event(JceScript *s, JceScriptInstance inst,
                                uint32_t id, const char *name,
                                float f0, float f1, int i0)
{
    const JceScriptVM *vm = vm_of(s, "call_anim_event");
    if (!vm) return;
    vm->call_anim_event(s, inst, id, name, f0, f1, i0);
}

bool jce_script_call_named(JceScript *s, const char *fn_name,
                           JceScriptEntity arg_entity)
{
    const JceScriptVM *vm = vm_of(s, "call_named");
    if (!vm) return false;
    return vm->call_named(s, fn_name, arg_entity);
}

bool jce_script_call_named_num(JceScript *s, const char *fn_name,
                               JceScriptEntity arg_entity, double value)
{
    const JceScriptVM *vm = vm_of(s, "call_named_num");
    if (!vm) return false;
    return vm->call_named_num(s, fn_name, arg_entity, value);
}

bool jce_script_call_named_str(JceScript *s, const char *fn_name,
                               JceScriptEntity arg_entity, const char *str)
{
    const JceScriptVM *vm = vm_of(s, "call_named_str");
    if (!vm) return false;
    return vm->call_named_str(s, fn_name, arg_entity, str);
}

int jce_script_instance_count(const JceScript *s)
{
    const JceScriptVM *vm = vm_of(s, "instance_count");
    if (!vm) return 0;
    return vm->instance_count(s);
}

void jce_script_update_coroutines(JceScript *s, float dt)
{
    const JceScriptVM *vm = vm_of(s, "update_coroutines");
    if (!vm) return;
    vm->update_coroutines(s, dt);
}

JceScriptModule jce_script_compile_module(JceScript *s, const char *name,
                                          const char *source, size_t len)
{
    const JceScriptVM *vm = vm_of(s, "compile_module");
    if (!vm) return 0;
    return vm->compile_module(s, name, source, len);
}

void jce_script_rebind_instance(JceScript *s, JceScriptInstance inst,
                                JceScriptModule mod)
{
    const JceScriptVM *vm = vm_of(s, "rebind_instance");
    if (!vm) return;
    vm->rebind_instance(s, inst, mod);
}

void jce_script_release_module(JceScript *s, JceScriptModule mod)
{
    const JceScriptVM *vm = vm_of(s, "release_module");
    if (!vm) return;
    vm->release_module(s, mod);
}

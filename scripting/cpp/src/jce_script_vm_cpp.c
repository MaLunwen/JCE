/*
 * jce_script_vm_cpp.c — the "cpp" JceScriptVM: the engine calling up into
 * compiled native code.
 *
 * Read jce_script_vm_cpp.h first; it holds the design (what "instantiate a
 * script" means when the script is a class, why the plugin boundary is BELOW
 * the VM, why unload is refcounted, and why no exception may cross the ABI).
 * This file is the mechanism.
 *
 * It is C, not C++, and that is not an accident: nothing here needs a C++
 * feature, and a C translation unit cannot accidentally acquire a
 * throw-through-C-frames path.  The only C++ in this backend is the
 * header-only module side, where the try/catch has to be.
 *
 * WHAT IS DELIBERATELY NOT SUPPORTED, and why — each of these is a slot the
 * core requires to be non-NULL, so each is an EXPLICIT no-op whose reason is
 * visible in this source rather than a hole nobody can see:
 *
 *   instantiate_source   A C++ script has no source at runtime.  Answering
 *                        anything but 0 would mean silently ignoring the
 *                        `source` the caller passed, which is worse than
 *                        refusing it.  Use jce_script_instantiate with a
 *                        class name.
 *   compile_module /     Hot-reload of a native class is not a recompile, it
 *   rebind_instance /    is unload + load + re-instantiate, and it cannot
 *   release_module       preserve `self` because `self` is a C++ object of a
 *                        type that no longer exists.  compile_module answers
 *                        0 (Lua's own "failed" value) and the other two are
 *                        no-ops on a 0 module.  The supported path is
 *                        jce_script_vm_cpp_unload + _load_library.
 *   update_coroutines    There is no cooperative scheduler here: a C++ script
 *                        that wants to wait keeps its own timer in a member,
 *                        which is what on_update is for.
 *
 * Every one of those is pinned by name in the differential, because a
 * divergence from the reference that nothing names is indistinguishable from
 * a defect.
 */

#include <jce/script_vm/jce_script_vm_cpp.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_library.h>

#include <stdio.h>
#include <string.h>

#define LOG_TAG "script"

/* ── Registry state ──────────────────────────────────────────────────────
 *
 * Fixed arrays, for the reason jce_script_vm.c's registry is fixed: a live
 * instance holds a pointer to its module entry and to its class entry, and a
 * realloc would dangle every one of them at once. */

typedef struct ClassEntry {
    char              name[JCE_CPP_NAME_MAX];
    JceCppScriptClass cls;            /* CLAMPED copy; see add_module */
    int               module_index;   /* owning module slot */
    bool              used;
} ClassEntry;

typedef struct GlobalEntry {
    char               name[JCE_CPP_NAME_MAX];
    JceCppScriptGlobal g;             /* CLAMPED copy */
    int                module_index;
    bool               used;
} GlobalEntry;

struct JceCppModule {
    char       name[JCE_CPP_NAME_MAX];
    JceLibrary library;      /* NULL for a statically linked module */
    int        live;         /* instances alive; unload refuses while > 0 */
    bool       alive;        /* false once unloaded — the slot is RETIRED */
    int        index;
};

static struct JceCppModule g_modules[JCE_CPP_MAX_MODULES];
static int                 g_module_count;      /* high-water, never shrinks */
static ClassEntry          g_classes[JCE_CPP_MAX_CLASSES_TOTAL];
static GlobalEntry         g_globals[JCE_CPP_MAX_GLOBALS_TOTAL];
static bool                g_registered;
static bool                g_ext_claimed;

/* ── The VM handle ───────────────────────────────────────────────────────
 *
 * JceScriptVMHeader FIRST.  jce_script_vm_create() refuses a handle whose
 * first word is not the table it dispatched through, then repoints it at the
 * registry's clamped copy.
 *
 * THIS FILE READS IT, for exactly one thing: `hdr.vm->language`.  It used to
 * say the field was "written once and never read", and that was true while
 * this backend served one language.  It serves two — scripting/c registers
 * "c" over this same registry (jce_script_vm_cpp_create_for) — and every
 * diagnostic below that used to spell "cpp" as a literal would then be a
 * message that names the wrong language to the only reader who needs it. */

/* Bits of CppInstance::disabled — THE FAILING-CALLBACK RULE's four repeating
 * fixed-name hooks (jce_script.h).  CB_NONE is a STATED non-participation and
 * two dispatchers pass it: `cpp_call_message`, because this backend routes
 * EVERY message name through one `on_message` thunk so there is no
 * per-message callback to disable, and `inst_free`, because on_destroy runs
 * once and the instance is freed on the next line. */
#define CB_NONE       0u
#define CB_START      (1u << 0)
#define CB_UPDATE     (1u << 1)
#define CB_COLLISION  (1u << 2)
#define CB_ANIM       (1u << 3)
/* Its own bit: a fixed-step handler that throws every step must not
 * disable the render-frame callback as collateral. */
#define CB_FIXED      (1u << 4)

typedef struct CppInstance {
    void            *obj;             /* what the class factory returned */
    const ClassEntry *cls;
    struct JceCppModule *module;
    JceScriptEntity  entity;
    bool             live;
    /* The hooks that returned a non-NULL JceCppStatus and are therefore not
     * called again on THIS instance.  Cleared only by destroying the instance:
     * `cpp_rebind_instance` is a documented no-op here because a compiled
     * class cannot be recompiled in-process, so this backend's re-enable is
     * jce_script_vm_cpp_unload + _load_library + re-instantiate, which is also
     * its only hot-reload path. */
    unsigned         disabled;
} CppInstance;

typedef struct CppScript {
    JceScriptVMHeader hdr;

    /* The host, copied at min(what the caller passed, our own sizeof) over a
     * ZEROED destination.  JceScriptHost is CALLER-allocated and grows, so
     * copying at our sizeof would read past the end of a host built against
     * an older header — the same fact tools/scriptgen/scriptgen_core.py
     * states for the generators, in the direction the VM sees it.
     * `host_size` below is the CLAMPED number and it is what reaches the
     * module through JceCppScriptContext, so the narrowing survives the whole
     * chain instead of being re-widened one layer down.
     * *Enforced by:* test_jce_script_vm_cpp_lifecycle.cpp ::
     * "a short host is not read past its end, and the clamp reaches the
     * module". */
    JceScriptHost host;
    size_t        host_size;
    bool          have_host;

    CppInstance *inst;
    int          inst_cap;
    int          inst_live;

    /* Each unsupported entry point logs ONCE per VM.  A per-frame dispatch
     * that logged every time would turn the log into the failure. */
    bool warned_instantiate_source;
    bool warned_compile_module;
} CppScript;

/* ── Small helpers ───────────────────────────────────────────────────────*/

static bool name_ok(const char *n)
{
    return n != NULL && n[0] != '\0' &&
           strlen(n) < (size_t)JCE_CPP_NAME_MAX;
}

static void name_copy(char *dst, const char *src)
{
    size_t n = strlen(src);
    memcpy(dst, src, n + 1u);
}

static struct JceCppModule *module_by_name(const char *name)
{
    int i;
    for (i = 0; i < g_module_count; ++i)
        if (g_modules[i].alive && strcmp(g_modules[i].name, name) == 0)
            return &g_modules[i];
    return NULL;
}

/* Modules currently ALIVE.  Not jce_script_vm_cpp_module_count(), which is the
 * slot high-water mark and counts retired modules too — the question a failed
 * resolution asks is "is there anything at all to resolve against", and a
 * retired module answers no. */
static int module_live_count(void)
{
    int i, n = 0;
    for (i = 0; i < g_module_count; ++i)
        if (g_modules[i].alive) ++n;
    return n;
}

static const ClassEntry *class_find(const char *name)
{
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < JCE_CPP_MAX_CLASSES_TOTAL; ++i) {
        if (!g_classes[i].used) continue;
        if (!g_modules[g_classes[i].module_index].alive) continue;
        if (strcmp(g_classes[i].name, name) == 0) return &g_classes[i];
    }
    return NULL;
}

/* ── THE PATH FORM: how a `scriptPath` becomes a class name ──────────────
 *
 * A Script component stores a STRING, and for every other backend that string
 * is a file the VM opens.  For this one it names a class in a compiled module,
 * and until this function existed the two had to be the same string: the
 * registry matched `path` with an exact strcmp, so a project whose scene said
 * "scripts/FlowerSway.jcecpp" had to declare its class under that entire
 * spelling, extension and directory and all.  Elemental Serenity did exactly
 * that — JCE_CPP_SCRIPT_CLASS(EsFlowerSway, "EsFlowerSway.escpp") — and it
 * worked; it was also a class carrying a file extension because the routing
 * layer only speaks extensions.
 *
 * THREE CANDIDATES, IN THIS ORDER, AND THE ORDER IS THE COMPATIBILITY
 * GUARANTEE:
 *
 *   1. the WHOLE string, exactly as stored.  FIRST, ALWAYS.  Every class name
 *      that resolved before this function existed resolved through candidate
 *      1, so no existing JCE_CPP_SCRIPT_CLASS call site can change meaning —
 *      including one whose name deliberately contains a dot or a slash.  A
 *      later candidate can only ever ADD a resolution, never redirect one.
 *      *Enforced by:* tests/scripting/cpp/test_jce_script_vm_cpp_modules.cpp
 *      :: "a class named after the whole stored path still wins over its own
 *      basename".
 *   2. the last path component, so "gameplay/FlowerSway.jcecpp" reaches a
 *      class published as "FlowerSway.jcecpp".
 *   3. that component with its final extension removed, so the same path
 *      reaches a class published as plain "FlowerSway" — the spelling a
 *      module author would write without knowing this file exists.
 *
 * A PATH WITH NO EXTENSION IS NOT A SPECIAL CASE: candidate 3 simply does not
 * exist for it, and candidates 1 and 2 are what a bare "FlowerSway" always
 * meant.  So the bare class name keeps working unchanged, which is what
 * jce_script_vm_cpp_add_module()'s own documentation has always promised.
 *
 * NO EXTENSION IS *VALIDATED* HERE, deliberately.  Asking "is this extension
 * claimed for cpp?" would make resolution depend on whether the claim call
 * had run yet, so the same path would resolve differently depending on
 * startup order — and a host that created this VM directly, with no claim at
 * all, would get a different answer again.  Candidate order is a property of
 * the argument alone.
 *
 * `tried` is filled with the candidates in the order they were attempted so a
 * refusal can print them; `stem` is storage for candidate 3 and is caller-
 * owned.  A candidate that cannot fit JCE_CPP_NAME_MAX is skipped rather than
 * truncated — a truncated name could collide with a DIFFERENT registered
 * class, which is the one failure mode worse than not resolving. */
static const ClassEntry *class_resolve(const char *path,
                                       const char *tried[3], int *tried_n,
                                       char stem[JCE_CPP_NAME_MAX])
{
    const ClassEntry *ce;
    const char       *base;
    const char       *dot;
    size_t            i, n;

    *tried_n = 0;
    stem[0]  = '\0';
    if (!path || !path[0]) return NULL;

    tried[(*tried_n)++] = path;
    ce = class_find(path);
    if (ce) return ce;

    base = path;
    for (i = 0; path[i]; ++i)
        if (path[i] == '/' || path[i] == '\\') base = path + i + 1;
    if (base != path && base[0]) {
        tried[(*tried_n)++] = base;
        ce = class_find(base);
        if (ce) return ce;
    }

    dot = NULL;
    for (i = 0; base[i]; ++i)
        if (base[i] == '.') dot = base + i;
    if (dot && dot != base) {
        n = (size_t)(dot - base);
        if (n < (size_t)JCE_CPP_NAME_MAX) {
            memcpy(stem, base, n);
            stem[n] = '\0';
            tried[(*tried_n)++] = stem;
            ce = class_find(stem);
            if (ce) return ce;
        }
    }
    return NULL;
}

static const GlobalEntry *global_find(const char *name)
{
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < JCE_CPP_MAX_GLOBALS_TOTAL; ++i) {
        if (!g_globals[i].used) continue;
        if (!g_modules[g_globals[i].module_index].alive) continue;
        if (strcmp(g_globals[i].name, name) == 0) return &g_globals[i];
    }
    return NULL;
}

/* THE LANGUAGE THIS HANDLE WAS CREATED FOR — "cpp" or "c".
 *
 * Read from the handle and not from a compile-time constant, because one set
 * of functions serves both registrations and the ONLY thing that distinguishes
 * them is which table the handle was created against.  jce_script_vm_create()
 * repoints hdr.vm at the registry's clamped copy, whose `language` points into
 * the registry's own name buffer — stable for the process, so this pointer is
 * safe to hand straight to a format string.
 *
 * The fallback is "cpp" and it is unreachable through jce_script_vm_create()
 * (which refuses a handle with no header); a host that hand-built a CppScript
 * gets the historical spelling rather than a NULL through %s. */
static const char *vm_lang(const CppScript *s)
{
    if (s && s->hdr.vm && s->hdr.vm->language && s->hdr.vm->language[0])
        return s->hdr.vm->language;
    return JCE_SCRIPT_VM_CPP_LANGUAGE;
}

/* Report a status a thunk returned, and apply THE FAILING-CALLBACK RULE.
 *
 * Shaped like the Lua path's message ("<method> error: <text>") and sent to
 * the SAME sinks — host->log plus LOG_ERROR — because the acceptance test for
 * this backend compares host calls against Lua and a different sink would read
 * as a missing event.  When `bit` names one of the four fixed-name hooks the
 * rule covers, the hook is then marked disabled on `in` and the notice
 * jce_script.h publishes is written through the same two sinks, in the same
 * order the reference writes it.
 *
 * `bit` is passed by the caller rather than derived from `where`, for the same
 * reason it is in jce_script.c: `cpp_call_message` reports under the fixed
 * label "on_message" but must not disable anything, and a rule keyed on the
 * label would silently disable every message the instance receives. */
static void report_status(CppScript *s, CppInstance *in, const char *where,
                          unsigned bit, JceCppStatus st)
{
    char buf[512];
    if (st == NULL) return;
    snprintf(buf, sizeof(buf), "%s error: %s", where, st);
    if (s->have_host && s->host.log) s->host.log(s->host.user, buf);
    LOG_ERROR(LOG_TAG, "%s script %s", vm_lang(s), buf);

    if (bit == CB_NONE || !in) return;
    in->disabled |= bit;
    snprintf(buf, sizeof(buf), JCE_SCRIPT_DISABLED_NOTICE_FMT, where);
    if (s->have_host && s->host.log) s->host.log(s->host.user, buf);
    LOG_ERROR(LOG_TAG, "%s script %s", vm_lang(s), buf);
}

static CppInstance *inst_at(CppScript *s, JceScriptInstance h)
{
    int idx;
    if (h == 0) return NULL;
    idx = (int)(h - 1u);
    if (idx < 0 || idx >= s->inst_cap) return NULL;
    if (!s->inst[idx].live) return NULL;
    return &s->inst[idx];
}

/* The instance to dispatch `bit` on, or NULL when there is nothing to call —
 * bad handle, or THE FAILING-CALLBACK RULE has disabled that hook here.
 *
 * ONE decision site for all six per-instance dispatchers, deliberately.  Five
 * separate `if (in->disabled & CB_X) return;` lines were four opportunities to
 * honour the rule for some hooks and not others, and a per-hook gate that no
 * case happens to drive is a gate a mutation can delete unnoticed — which is
 * exactly what happened to on_collision's before this was folded.  Passing
 * CB_NONE (call_message) asks about no bit and therefore always dispatches. */
static CppInstance *dispatch_target(CppScript *s, JceScriptInstance h,
                                    unsigned bit)
{
    CppInstance *in = s ? inst_at(s, h) : NULL;
    if (!in) return NULL;
    return (in->disabled & bit) ? NULL : in;
}

/* ── Module registration ─────────────────────────────────────────────────*/

static bool class_register(struct JceCppModule *m, const JceCppScriptClass *src)
{
    size_t      copy;
    int         i;
    ClassEntry  tmp;

    memset(&tmp, 0, sizeof(tmp));
    if (!src) {
        LOG_ERROR(LOG_TAG, "native script module '%s': NULL class entry", m->name);
        return false;
    }
    /* struct_size is the FIRST member, so it is the one field readable from a
     * table shorter than ours; everything below depends on it.  Copy
     * min(theirs, ours) over the zeroed tmp: members the module never knew
     * about stay NULL rather than being whatever followed its allocation. */
    if (src->struct_size < offsetof(JceCppScriptClass, destroy) +
                               sizeof(void (*)(void *))) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s': class struct_size %zu is too short to "
                  "reach create/destroy",
                  m->name, src->struct_size);
        return false;
    }
    copy = src->struct_size < sizeof(tmp.cls) ? src->struct_size
                                              : sizeof(tmp.cls);
    memcpy(&tmp.cls, src, copy);
    tmp.cls.struct_size = copy;   /* record what we HOLD, not what was claimed */

    if (!name_ok(tmp.cls.name)) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s': a class has an empty or over-long name "
                  "(max %d bytes incl. NUL)",
                  m->name, JCE_CPP_NAME_MAX);
        return false;
    }
    if (!tmp.cls.create || !tmp.cls.destroy) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s': class '%s' leaves %s NULL; both are "
                  "required",
                  m->name, tmp.cls.name,
                  tmp.cls.create ? "destroy" : "create");
        return false;
    }
    if (class_find(tmp.cls.name) != NULL) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s': class name '%s' is already registered by "
                  "another module — instantiate() resolves by name, so a "
                  "duplicate would make which class you get depend on load "
                  "order",
                  m->name, tmp.cls.name);
        return false;
    }
    for (i = 0; i < JCE_CPP_MAX_CLASSES_TOTAL; ++i) {
        if (g_classes[i].used) continue;
        /* The name lives in the MODULE's image; a successful unload frees it.
         * Copy it before anything can hold onto it. */
        name_copy(tmp.name, tmp.cls.name);
        tmp.cls.name     = NULL;   /* nobody may read the borrowed pointer */
        tmp.module_index = m->index;
        tmp.used         = true;
        g_classes[i]     = tmp;
        return true;
    }
    LOG_ERROR(LOG_TAG, "native script module '%s': no room for class '%s' (%d max)",
              m->name, tmp.cls.name, JCE_CPP_MAX_CLASSES_TOTAL);
    return false;
}

static bool global_register(struct JceCppModule *m,
                            const JceCppScriptGlobal *src)
{
    size_t      copy;
    int         i;
    GlobalEntry tmp;

    memset(&tmp, 0, sizeof(tmp));
    if (!src) {
        LOG_ERROR(LOG_TAG, "native script module '%s': NULL global entry", m->name);
        return false;
    }
    if (src->struct_size < offsetof(JceCppScriptGlobal, fn_entity)) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s': global struct_size %zu is too short to "
                  "reach a name",
                  m->name, src->struct_size);
        return false;
    }
    copy = src->struct_size < sizeof(tmp.g) ? src->struct_size : sizeof(tmp.g);
    memcpy(&tmp.g, src, copy);
    tmp.g.struct_size = copy;

    if (!name_ok(tmp.g.name)) {
        LOG_ERROR(LOG_TAG, "native script module '%s': a global has a bad name", m->name);
        return false;
    }
    if (!tmp.g.fn_entity && !tmp.g.fn_num && !tmp.g.fn_str) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s': global '%s' supplies no function at all",
                  m->name, tmp.g.name);
        return false;
    }
    if (global_find(tmp.g.name) != NULL) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s': global handler '%s' is already registered",
                  m->name, tmp.g.name);
        return false;
    }
    for (i = 0; i < JCE_CPP_MAX_GLOBALS_TOTAL; ++i) {
        if (g_globals[i].used) continue;
        name_copy(tmp.name, tmp.g.name);
        tmp.g.name       = NULL;
        tmp.module_index = m->index;
        tmp.used         = true;
        g_globals[i]     = tmp;
        return true;
    }
    LOG_ERROR(LOG_TAG, "native script module '%s': no room for global '%s'", m->name,
              tmp.g.name);
    return false;
}

/* Undo a partially-registered module.  A module that fails halfway must leave
 * NOTHING behind: a class from a refused module is a class whose code the
 * loader is about to unmap. */
static void module_purge(int index)
{
    int i;
    for (i = 0; i < JCE_CPP_MAX_CLASSES_TOTAL; ++i)
        if (g_classes[i].used && g_classes[i].module_index == index)
            memset(&g_classes[i], 0, sizeof(g_classes[i]));
    for (i = 0; i < JCE_CPP_MAX_GLOBALS_TOTAL; ++i)
        if (g_globals[i].used && g_globals[i].module_index == index)
            memset(&g_globals[i], 0, sizeof(g_globals[i]));
}

static JceCppModule *module_add(const JceCppModuleDesc *desc, JceLibrary lib)
{
    struct JceCppModule *m;
    JceCppModuleDesc     d;
    size_t               copy;
    size_t               i;
    int                  slot;

    if (!desc) {
        LOG_ERROR(LOG_TAG, "native script module refused: NULL descriptor");
        return NULL;
    }
    if (desc->struct_size < offsetof(JceCppModuleDesc, class_count) +
                                sizeof(size_t)) {
        LOG_ERROR(LOG_TAG,
                  "native script module refused: struct_size %zu is too short to carry "
                  "a class list",
                  desc->struct_size);
        return NULL;
    }
    memset(&d, 0, sizeof(d));
    copy = desc->struct_size < sizeof(d) ? desc->struct_size : sizeof(d);
    memcpy(&d, desc, copy);
    d.struct_size = copy;

    if (d.abi_version != JCE_CPP_MODULE_ABI_VERSION) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s' refused: module ABI %u, this engine speaks "
                  "%u",
                  d.module_name ? d.module_name : "(null)",
                  (unsigned)d.abi_version,
                  (unsigned)JCE_CPP_MODULE_ABI_VERSION);
        return NULL;
    }
    if (!name_ok(d.module_name)) {
        LOG_ERROR(LOG_TAG,
                  "native script module refused: empty or over-long module name (max %d "
                  "bytes incl. NUL)",
                  JCE_CPP_NAME_MAX);
        return NULL;
    }
    if (module_by_name(d.module_name) != NULL) {
        LOG_ERROR(LOG_TAG, "native script module '%s' refused: already registered",
                  d.module_name);
        return NULL;
    }
    if (d.class_count > 0 && d.classes == NULL) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s' refused: class_count %zu with a NULL array",
                  d.module_name, d.class_count);
        return NULL;
    }

    slot = -1;
    for (i = 0; i < (size_t)JCE_CPP_MAX_MODULES; ++i) {
        if (!g_modules[i].alive && g_modules[i].name[0] == '\0') {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        LOG_ERROR(LOG_TAG, "native script module '%s' refused: registry full (%d)",
                  d.module_name, JCE_CPP_MAX_MODULES);
        return NULL;
    }

    m = &g_modules[slot];
    memset(m, 0, sizeof(*m));
    m->index = slot;
    m->alive = true;
    m->library = lib;
    name_copy(m->name, d.module_name);
    if (slot >= g_module_count) g_module_count = slot + 1;

    for (i = 0; i < d.class_count; ++i) {
        if (!class_register(m, d.classes[i])) {
            module_purge(slot);
            memset(m, 0, sizeof(*m));
            m->index = slot;
            return NULL;
        }
    }
    for (i = 0; i < d.global_count; ++i) {
        if (d.globals == NULL || !global_register(m, d.globals[i])) {
            module_purge(slot);
            memset(m, 0, sizeof(*m));
            m->index = slot;
            return NULL;
        }
    }

    LOG_INFO(LOG_TAG,
             "native script module '%s' registered: %zu class(es), %zu global(s)%s",
             m->name, d.class_count, d.global_count,
             lib ? " (shared object)" : "");
    return m;
}

JceCppModule *JCE_CALL jce_script_vm_cpp_add_module(const JceCppModuleDesc *desc)
{
    return module_add(desc, NULL);
}

/* Absolute, by the platform's own spelling.  Not a string-tidiness check: see
 * the refusal in jce_script_vm_cpp_load_library for what a non-absolute path
 * costs. */
static bool path_is_absolute(const char *p)
{
    if (!p || !p[0]) return false;
#if defined(_WIN32)
    if (p[0] == '\\' && p[1] == '\\') return true;    /* UNC, and \\?\ too */
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':' && (p[2] == '\\' || p[2] == '/'))
        return true;
    return false;
#else
    return p[0] == '/';
#endif
}

JceCppModule *JCE_CALL jce_script_vm_cpp_load_library(const char *absolute_path)
{
    JceLibrary           lib;
    JceCppModuleEntry    entry;
    const JceCppModuleDesc *desc;
    JceCppModule        *m;

    if (!absolute_path || !absolute_path[0]) {
        LOG_ERROR(LOG_TAG, "native script module load refused: no path");
        return NULL;
    }
    /* ANYTHING NOT ABSOLUTE IS REFUSED, and a bare name is only the loudest
     * case.  jce_library.h:36-39: on Windows the open "attaches to an
     * already-resident module of the same name rather than loading a second
     * copy", so a name the loader gets to interpret can bind to a DIFFERENT
     * file that happens to be mapped -- and every function pointer in that
     * file is one the engine is about to call.  A relative path is refused
     * for the second half of the same reason: it resolves against a working
     * directory this layer does not own, so "the file I meant" is not a
     * property of the argument. */
    if (!path_is_absolute(absolute_path)) {
        LOG_ERROR(LOG_TAG,
                  "native script module load refused: '%s' is not an absolute path. "
                  "jce_library_open() attaches to an already-resident module "
                  "of the same name, and a relative path resolves against a "
                  "working directory this layer does not own -- either way "
                  "the file whose code the engine would call need not be the "
                  "one you named.",
                  absolute_path);
        return NULL;
    }

    lib = jce_library_open(absolute_path);
    if (!lib) {
        LOG_ERROR(LOG_TAG, "native script module load failed: cannot open '%s'",
                  absolute_path);
        return NULL;
    }
    entry = (JceCppModuleEntry)jce_library_symbol(
        lib, JCE_CPP_MODULE_ENTRY_SYMBOL);
    if (!entry) {
        LOG_ERROR(LOG_TAG,
                  "native script module load failed: '%s' exports no %s — it is not a "
                  "JCE script module",
                  absolute_path, JCE_CPP_MODULE_ENTRY_SYMBOL);
        jce_library_close(lib);
        return NULL;
    }
    desc = entry(JCE_CPP_MODULE_ABI_VERSION);
    if (!desc) {
        LOG_ERROR(LOG_TAG,
                  "native script module load failed: '%s' refused module ABI %u",
                  absolute_path, (unsigned)JCE_CPP_MODULE_ABI_VERSION);
        jce_library_close(lib);
        return NULL;
    }
    m = module_add(desc, lib);
    if (!m) {
        /* A refused module leaves NOTHING mapped: its code is exactly the
         * code we just decided not to trust. */
        jce_library_close(lib);
        return NULL;
    }
    return m;
}

bool JCE_CALL jce_script_vm_cpp_unload(JceCppModule *m)
{
    if (!m || !m->alive) {
        LOG_ERROR(LOG_TAG, "native script module unload refused: not a live module");
        return false;
    }
    /* THE USE-AFTER-FREE THIS FUNCTION EXISTS FOR.  Every function pointer in
     * this module's classes points into its image.  Unloading now would unmap
     * the code that the next on_update dispatches into, and the crash would
     * land inside the engine's forwarder with no script in the backtrace. */
    if (m->live > 0) {
        LOG_ERROR(LOG_TAG,
                  "native script module '%s' unload REFUSED: %d live instance(s). "
                  "Their on_update dispatches into this module's code; "
                  "jce_script_release() them first.",
                  m->name, m->live);
        return false;
    }
    module_purge(m->index);
    if (m->library) jce_library_close(m->library);
    m->library = NULL;
    m->alive   = false;
    /* The slot is RETIRED, not recycled: the name stays so a stale handle
     * reads as a refusal rather than as a different module. */
    LOG_INFO(LOG_TAG, "native script module '%s' unloaded", m->name);
    return true;
}

int JCE_CALL jce_script_vm_cpp_module_count(void)
{
    int i, n = 0;
    for (i = 0; i < g_module_count; ++i)
        if (g_modules[i].alive) ++n;
    return n;
}

JceCppModule *JCE_CALL jce_script_vm_cpp_module_at(int index)
{
    int i, n = 0;
    if (index < 0) return NULL;
    for (i = 0; i < g_module_count; ++i) {
        if (!g_modules[i].alive) continue;
        if (n == index) return &g_modules[i];
        ++n;
    }
    return NULL;
}

const char *JCE_CALL jce_script_vm_cpp_module_name(const JceCppModule *m)
{
    return m ? m->name : NULL;
}

int JCE_CALL jce_script_vm_cpp_live_instances(const JceCppModule *m)
{
    return m ? m->live : -1;
}

bool JCE_CALL jce_script_vm_cpp_has_class(const char *class_name)
{
    const char *tried[3];
    int         tried_n = 0;
    char        stem[JCE_CPP_NAME_MAX];
    /* class_resolve and NOT class_find: this function's whole contract is
     * "resolve the way instantiate() does", so a host can validate an
     * authored scriptPath at load time instead of discovering a 0 from
     * instantiate at spawn time.  Answering from a narrower lookup would make
     * it report "no such class" for a path instantiate() would have run —
     * a validator that is wrong in the direction that costs the most.
     * *Enforced by:* tests/scripting/cpp/test_jce_script_vm_cpp_modules.cpp ::
     * "has_class answers for every form instantiate accepts". */
    return class_resolve(class_name, tried, &tried_n, stem) != NULL;
}

/* ── The 18 slots ────────────────────────────────────────────────────────*/

/* THE ONE HANDLE FACTORY, parameterised by the table.  See the note on
 * jce_script_vm_cpp_create_for() in the header for why the table is an
 * argument instead of a constant: a second registered language ("c") shares
 * every one of these functions, and jce_script_vm_create() checks that the
 * handle's header names the table IT dispatched through — which for the "c"
 * entry is not this file's table. */
JceScript *JCE_CALL jce_script_vm_cpp_create_for(const JceScriptVM *table,
                                                 const JceScriptHost *host,
                                                 size_t host_size)
{
    CppScript *s;

    /* A NULL table would produce a handle jce_script_vm_create() rejects one
     * line later, having destroyed it — a refusal whose message is about the
     * header rather than about the caller's mistake.  Say it here. */
    if (!table) {
        LOG_ERROR(LOG_TAG, "%s",
                  "jce_script_vm_cpp_create_for(NULL, ...): the table must be "
                  "the address that was passed to jce_script_vm_register()");
        return NULL;
    }

    s = (CppScript *)jce_malloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    /* MUST point at the CALLER's table — the address passed to
     * jce_script_vm_register().  jce_script_vm_create() checks it and then
     * repoints it at the registry's clamped copy. */
    s->hdr.vm = table;

    if (host && host_size > 0) {
        /* min(theirs, ours) over a ZEROED destination.  Never sizeof(*host). */
        const size_t n = host_size < sizeof(s->host) ? host_size
                                                     : sizeof(s->host);
        memcpy(&s->host, host, n);
        s->host_size = n;
        s->have_host = true;
    }
    return (JceScript *)s;
}

static JceScript *cpp_create_sized(const JceScriptHost *host, size_t host_size)
{
    return jce_script_vm_cpp_create_for(jce_script_vm_cpp(), host, host_size);
}

static void inst_free(CppScript *s, CppInstance *in, bool call_on_destroy)
{
    if (!in->live) return;
    if (call_on_destroy && in->cls->cls.on_destroy)
        report_status(s, in, "on_destroy", CB_NONE,
                      in->cls->cls.on_destroy(in->obj));
    in->cls->cls.destroy(in->obj);
    if (in->module && in->module->live > 0) --in->module->live;
    in->obj    = NULL;
    in->live   = false;
    if (s->inst_live > 0) --s->inst_live;
}

static void cpp_destroy(JceScript *sc)
{
    CppScript *s = (CppScript *)sc;
    int        i;
    if (!s) return;
    /* Mirrors the Lua path: script_lua_destroy() closes the lua_State without
     * running on_destroy on anything.  The C++ objects still have to be freed
     * — a lua_close reclaims its tables, nothing reclaims a `new`ed class —
     * and the module refcounts have to come back down or every module in the
     * process becomes permanently un-unloadable.
     * *Enforced by:* "destroying a VM with live instances releases the module
     * refcount". */
    for (i = 0; i < s->inst_cap; ++i)
        inst_free(s, &s->inst[i], false);
    jce_free(s->inst);
    jce_free(s);
}

static JceScriptInstance cpp_instantiate(JceScript *sc, const char *path,
                                         JceScriptEntity owner)
{
    CppScript          *s = (CppScript *)sc;
    const ClassEntry   *ce;
    JceCppScriptContext ctx;
    void               *obj;
    int                 i;
    const char         *tried[3];
    int                 tried_n = 0;
    char                stem[JCE_CPP_NAME_MAX];
    char                list[3 * (JCE_CPP_NAME_MAX + 8)];

    if (!s || !path) return 0;
    ce = class_resolve(path, tried, &tried_n, stem);
    if (!ce) {
        /* NAME THE CANDIDATES.  "no class named X" was true and useless the
         * moment resolution stopped being one strcmp: the reader cannot tell
         * whether their class name is misspelled, whether the directory part
         * was supposed to be stripped, or whether the module simply is not
         * loaded.  Printing what was actually looked up answers all three. */
        int    k;
        size_t used = 0;
        /* tried_n is 0 only for an empty path, and an empty candidate list
         * printed as nothing at all reads like a truncated message. */
        snprintf(list, sizeof list, "%s", "(nothing: the path was empty)");
        for (k = 0; k < tried_n && used + 1 < sizeof list; ++k) {
            int w = snprintf(list + used, sizeof list - used, "%s'%s'",
                             used ? ", " : "", tried[k]);
            if (w <= 0) break;
            used += (size_t)w;
            if (used >= sizeof list) { used = sizeof list - 1; break; }
        }
        LOG_ERROR(LOG_TAG,
                  "no %s script class resolves '%s' — for the %s VM the "
                  "instantiate path names a CLASS published by a module, not "
                  "a file the engine opens. Tried, in order: %s",
                  vm_lang(s), path, vm_lang(s), list);
        if (module_live_count() == 0) {
            /* THE STATE THIS BACKEND IS ALMOST ALWAYS IN WHEN IT REFUSES, and
             * the one whose fix is nowhere near the spelling of the name.
             * A host that never published a module has an empty registry, so
             * EVERY class name misses and every message would otherwise read
             * as a typo. */
            LOG_ERROR(LOG_TAG, "%s",
                      "  NO NATIVE SCRIPT MODULE IS LOADED IN THIS PROCESS. "
                      "One registry serves both the c and the cpp language, "
                      "so the call is the same either way: "
                      "jce_script_vm_cpp_add_module() (module compiled into "
                      "this executable) or jce_script_vm_cpp_load_library() "
                      "with an ABSOLUTE path (module built as a shared "
                      "object), BEFORE the first scene loads.");
        } else {
            LOG_ERROR(LOG_TAG,
                      "  %d native script module(s) are loaded and none "
                      "publishes any of those names — check the spelling in "
                      "JCE_CPP_SCRIPT_CLASS / JCE_C_SCRIPT_CLASS, or rebuild "
                      "the module if the class was added after it was built.",
                      module_live_count());
        }
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.struct_size = sizeof(ctx);
    ctx.host        = s->have_host ? &s->host : NULL;
    ctx.host_size   = s->host_size;      /* the CLAMPED size, not our sizeof */
    ctx.entity      = owner;
    ctx.class_name  = ce->name;

    obj = ce->cls.create(&ctx);
    if (!obj) {
        LOG_ERROR(LOG_TAG, "%s script class '%s' failed to construct",
                  vm_lang(s), ce->name);
        return 0;
    }

    for (i = 0; i < s->inst_cap; ++i)
        if (!s->inst[i].live) break;
    if (i == s->inst_cap) {
        int          cap = s->inst_cap ? s->inst_cap * 2 : 16;
        CppInstance *nv  = (CppInstance *)jce_realloc(s->inst,
                                                      (size_t)cap * sizeof(*nv));
        if (!nv) {
            ce->cls.destroy(obj);
            return 0;
        }
        memset(nv + s->inst_cap, 0,
               (size_t)(cap - s->inst_cap) * sizeof(*nv));
        s->inst     = nv;
        s->inst_cap = cap;
    }
    s->inst[i].obj    = obj;
    s->inst[i].cls    = ce;
    s->inst[i].module = &g_modules[ce->module_index];
    s->inst[i].entity   = owner;
    s->inst[i].live     = true;
    s->inst[i].disabled = 0u;   /* a fresh instance has nothing disabled */
    ++s->inst[i].module->live;      /* the refcount jce_..._unload consults */
    ++s->inst_live;
    return (JceScriptInstance)(i + 1);
}

static JceScriptInstance cpp_instantiate_source(JceScript *sc, const char *name,
                                                const char *source,
                                                JceScriptEntity owner)
{
    CppScript *s = (CppScript *)sc;
    (void)owner;
    if (!s) return 0;
    if (!s->warned_instantiate_source) {
        s->warned_instantiate_source = true;
        LOG_ERROR(LOG_TAG,
                  "jce_script_instantiate_source('%s') is not supported by "
                  "the %s VM: a native script is compiled, so there is no "
                  "source to run. Use jce_script_instantiate(s, "
                  "\"<class name>\", owner).",
                  name ? name : "?", vm_lang(s));
    }
    (void)source;
    return 0;
}

static void cpp_call_start(JceScript *sc, JceScriptInstance h)
{
    CppScript   *s  = (CppScript *)sc;
    CppInstance *in = dispatch_target(s, h, CB_START);
    if (!in || !in->cls->cls.on_start) return;
    report_status(s, in, "on_start", CB_START, in->cls->cls.on_start(in->obj));
}

static void cpp_call_update(JceScript *sc, JceScriptInstance h, float dt)
{
    CppScript   *s  = (CppScript *)sc;
    CppInstance *in = dispatch_target(s, h, CB_UPDATE);
    if (!in || !in->cls->cls.on_update) return;
    report_status(s, in, "on_update", CB_UPDATE,
                  in->cls->cls.on_update(in->obj, dt));
}

static void cpp_call_fixed_update(JceScript *sc, JceScriptInstance h, float dt)
{
    CppScript   *s  = (CppScript *)sc;
    CppInstance *in = dispatch_target(s, h, CB_FIXED);
    if (!in || !in->cls->cls.on_fixed_update) return;
    report_status(s, in, "on_fixed_update", CB_FIXED,
                  in->cls->cls.on_fixed_update(in->obj, dt));
}

static void cpp_release(JceScript *sc, JceScriptInstance h)
{
    CppScript   *s  = (CppScript *)sc;
    CppInstance *in = s ? inst_at(s, h) : NULL;
    if (!in) return;
    /* Lua's release calls on_destroy and THEN unrefs; so does this. */
    inst_free(s, in, true);
}

static void cpp_call_collision(JceScript *sc, JceScriptInstance h,
                               JceScriptEntity other)
{
    CppScript   *s  = (CppScript *)sc;
    CppInstance *in = dispatch_target(s, h, CB_COLLISION);
    if (!in || !in->cls->cls.on_collision) return;
    report_status(s, in, "on_collision", CB_COLLISION,
                  in->cls->cls.on_collision(in->obj, other));
}

static void cpp_call_message(JceScript *sc, JceScriptInstance h,
                             const char *msg_name, double number_arg,
                             const char *str_arg)
{
    CppScript   *s  = (CppScript *)sc;
    /* CB_NONE asks about no bit, so a message is never gated — PINNED, see
     * jce_script.h at jce_script_call_message. */
    CppInstance *in = dispatch_target(s, h, CB_NONE);
    if (!in || !in->cls->cls.on_message || !msg_name) return;
    report_status(s, in, "on_message", CB_NONE,
                  in->cls->cls.on_message(in->obj, msg_name, number_arg,
                                          str_arg));
}

static void cpp_call_anim_event(JceScript *sc, JceScriptInstance h,
                                uint32_t id, const char *name, float f0,
                                float f1, int i0)
{
    CppScript   *s  = (CppScript *)sc;
    CppInstance *in = dispatch_target(s, h, CB_ANIM);
    if (!in || !in->cls->cls.on_anim_event) return;
    /* Lua pushes nil for an empty label, so the handler sees `nil`; the C++
     * spelling of "no label" is NULL, not "". */
    if (name && !name[0]) name = NULL;
    report_status(s, in, "on_anim_event", CB_ANIM,
                  in->cls->cls.on_anim_event(in->obj, id, name, f0, f1, i0));
}

/* The context a GLOBAL handler is given.  A global has no instance, so
 * without this it would have no host and no way to call the engine — a slot
 * the core requires to be non-NULL, filled with something that can do
 * nothing.  `class_name` is NULL because a global belongs to no class. */
static void global_context(const CppScript *s, JceScriptEntity e,
                           JceCppScriptContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->struct_size = sizeof(*ctx);
    ctx->host        = s->have_host ? &s->host : NULL;
    ctx->host_size   = s->host_size;   /* the CLAMPED size, not our sizeof */
    ctx->entity      = e;
    ctx->class_name  = NULL;
}

/* The three global-handler dispatchers.  Returning false means "no such
 * handler" and is indistinguishable from a correctly-absent one — which is
 * why every miss below LOGS.  See THE TWO SLOTS THAT FAIL SILENTLY in
 * jce_script_vm.h. */
static bool cpp_call_named(JceScript *sc, const char *fn_name,
                           JceScriptEntity arg_entity)
{
    CppScript          *s = (CppScript *)sc;
    const GlobalEntry  *g;
    JceCppScriptContext ctx;
    if (!s || !fn_name || !fn_name[0]) return false;
    g = global_find(fn_name);
    if (!g) return false;
    if (!g->g.fn_entity) {
        LOG_ERROR(LOG_TAG,
                  "%s global '%s' exists but takes no entity-only form; "
                  "call_named cannot invent the arguments it declares",
                  vm_lang(s), fn_name);
        return false;
    }
    global_context(s, arg_entity, &ctx);
    /* NULL instance + CB_NONE: a global has none, and jce_script.h's
     * FAILING-CALLBACK RULE excludes the named dispatchers for exactly that
     * reason plus the return-value one stated there. */
    report_status(s, NULL, fn_name, CB_NONE,
                  g->g.fn_entity(&ctx, arg_entity));
    return true;
}

static bool cpp_call_named_num(JceScript *sc, const char *fn_name,
                               JceScriptEntity arg_entity, double value)
{
    CppScript          *s = (CppScript *)sc;
    const GlobalEntry  *g;
    JceCppScriptContext ctx;
    if (!s || !fn_name || !fn_name[0]) return false;
    g = global_find(fn_name);
    if (!g) return false;
    if (g->g.fn_num) {
        global_context(s, arg_entity, &ctx);
        report_status(s, NULL, fn_name, CB_NONE,
                      g->g.fn_num(&ctx, arg_entity, value));
        return true;
    }
    /* Lua drops an argument a function does not declare and still returns
     * true.  A handler bound to a UISlider that ignores the value must not
     * read as "no handler". */
    if (g->g.fn_entity) {
        global_context(s, arg_entity, &ctx);
        report_status(s, NULL, fn_name, CB_NONE,
                      g->g.fn_entity(&ctx, arg_entity));
        return true;
    }
    LOG_ERROR(LOG_TAG,
              "%s global '%s' takes only a string; call_named_num has no "
              "string to give it",
              vm_lang(s), fn_name);
    return false;
}

static bool cpp_call_named_str(JceScript *sc, const char *fn_name,
                               JceScriptEntity arg_entity, const char *str)
{
    CppScript          *s = (CppScript *)sc;
    const GlobalEntry  *g;
    JceCppScriptContext ctx;
    if (!s || !fn_name || !fn_name[0]) return false;
    g = global_find(fn_name);
    if (!g) return false;
    if (g->g.fn_str) {
        global_context(s, arg_entity, &ctx);
        report_status(s, NULL, fn_name, CB_NONE,
                      g->g.fn_str(&ctx, arg_entity, str));
        return true;
    }
    if (g->g.fn_entity) {
        global_context(s, arg_entity, &ctx);
        report_status(s, NULL, fn_name, CB_NONE,
                      g->g.fn_entity(&ctx, arg_entity));
        return true;
    }
    LOG_ERROR(LOG_TAG,
              "%s global '%s' takes only a number; call_named_str has no "
              "number to give it",
              vm_lang(s), fn_name);
    return false;
}

static int cpp_instance_count(const JceScript *sc)
{
    const CppScript *s = (const CppScript *)sc;
    return s ? s->inst_live : 0;
}

/* EXPLICIT NO-OP.  There is no cooperative scheduler here: jce.start_coroutine
 * and jce.wait_seconds are two of the manifest's seven hand-written entries
 * and they park a Lua coroutine.  A C++ script that wants to wait keeps its
 * own timer, which is what on_update is for. */
static void cpp_update_coroutines(JceScript *sc, float dt)
{
    (void)sc;
    (void)dt;
}

/* EXPLICIT NO-OPs.  Hot-reloading a native class is not a recompile; see the
 * banner.  0 is Lua's own "compile failed" value, so a caller that already
 * handles that path handles this one. */
static JceScriptModule cpp_compile_module(JceScript *sc, const char *name,
                                          const char *source, size_t len)
{
    CppScript *s = (CppScript *)sc;
    (void)source;
    (void)len;
    if (!s) return 0;
    if (!s->warned_compile_module) {
        s->warned_compile_module = true;
        LOG_ERROR(LOG_TAG,
                  "jce_script_compile_module('%s') is not supported by the "
                  "%s VM: a compiled class cannot be recompiled in-process, "
                  "and its `self` is an object of a type that would no longer "
                  "exist. Reload with jce_script_vm_cpp_unload + _load_library "
                  "and re-instantiate.",
                  name ? name : "?", vm_lang(s));
    }
    return 0;
}

/* A no-op, and it stays one even though jce_script.h says a rebind clears THE
 * FAILING-CALLBACK RULE's disables.  Rebinding is the engine saying the CODE
 * may have changed; here it cannot have — `cpp_compile_module` refuses and
 * returns 0, so the only thing a rebind could do is re-enable a hook whose
 * implementation is byte for byte the one that just failed, and the next frame
 * would fail and log again.  This backend's re-enable is its hot-reload path:
 * jce_script_vm_cpp_unload + _load_library + re-instantiate.
 * *Enforced by:* ... :: "PINNED: a rebind cannot re-enable a disabled callback
 * for compiled code, and re-instantiating does". */
static void cpp_rebind_instance(JceScript *sc, JceScriptInstance h,
                                JceScriptModule mod)
{
    (void)sc;
    (void)h;
    (void)mod;
}

static void cpp_release_module(JceScript *sc, JceScriptModule mod)
{
    (void)sc;
    (void)mod;
}

/* POSITIONAL, like both of the engine's own vtables and for the same reason:
 * inserting or reordering a JceScriptVM member retypes every slot after the
 * insertion point and the build stops.  Designated initialisers would survive
 * a reorder silently. */
static const JceScriptVM k_cpp_vm = {
    sizeof(JceScriptVM),
    JCE_SCRIPT_VM_CPP_LANGUAGE,
    cpp_create_sized,
    cpp_destroy,
    cpp_instantiate,
    cpp_instantiate_source,
    cpp_call_start,
    cpp_call_update,
    cpp_release,
    cpp_call_collision,
    cpp_call_message,
    cpp_call_anim_event,
    cpp_call_named,
    cpp_call_named_num,
    cpp_call_named_str,
    cpp_instance_count,
    cpp_update_coroutines,
    cpp_compile_module,
    cpp_rebind_instance,
    cpp_release_module,
    cpp_call_fixed_update,   /* APPENDED -- see jce_script_vm.h */
};

const JceScriptVM *JCE_CALL jce_script_vm_cpp(void)
{
    return &k_cpp_vm;
}

bool JCE_CALL jce_script_vm_cpp_register(void)
{
    /* Idempotent HERE and not in the core.  The core is right to refuse a
     * duplicate name — live handles hold a pointer into the registry — but a
     * program that links two script modules would otherwise have to agree
     * about which of them calls first. */
    /* TWO HALVES, EACH IDEMPOTENT ON ITS OWN.  They are tracked separately
     * because they fail for unrelated reasons and a retry must be able to
     * complete the half that did not happen: collapsing them behind one flag
     * would let a process whose extension claim was refused report "already
     * registered" forever, with the VM present and nothing routing to it. */
    if (!g_registered) {
        if (!jce_script_vm_register(&k_cpp_vm)) return false;
        g_registered = true;
    }
    if (g_ext_claimed) return true;

    /* ".jcecpp" — THE PATH FORM, and the reason it is not ".cpp".
     *
     * This backend claimed NO extension for most of its life, on the reasoning
     * that its instantiate path is a class name and there is nothing for
     * jce_script_vm_language_for_path() to match.  The first half of that was
     * right and the conclusion was wrong, and a four-language scene is what
     * showed it: the RUNTIME picks a VM per Script component by asking
     * language_for_path(), so a component naming a C++ class resolved to NO
     * LANGUAGE and was refused before any VM was asked.  The only escapes
     * were JCE_SCRIPT_LANGUAGE — a WHOLE-PROCESS override, useless the moment
     * a second language is in the scene — or a project inventing its own
     * claim and naming its class after the entire stored string.  Elemental
     * Serenity shipped the second and it worked; it also meant the editor
     * flagged a working script amber and editor Play could not run it at all.
     *
     * So a claim is required.  ".cpp" is the one spelling it must NOT be: it
     * would classify every translation unit in the project as an attachable
     * script (the picker defect eedff3ea closed) and would tell the cooker and
     * the publication policy to pack the project's C++ SOURCE into the shipped
     * game.  ".jcecpp" is engine-namespaced and collides with no build input.
     *
     * A STRING LITERAL, not a macro, and that is deliberate:
     * check_script_language_catalog.py reads these calls
     * statically and can only resolve a literal or a macro defined in this
     * same file — a macro from the header would be reported "could not be
     * checked" and the catalog row would go unguarded.  Same shape as
     * scripting/python and scripting/java, for the same reason.
     *
     * The claim FOLLOWS the registration because a claim naming an
     * unregistered language is refused, which is what keeps the
     * registered-language list a sound test for "is this backend linked".
     * *Was enforced by* (no longer checked — tools/audit/ was removed):
     * tests/scripting/cpp/test_jce_script_vm_cpp_modules.cpp ::
     * "registering the cpp backend claims .jcecpp, and a scriptPath with that
     * extension routes to this VM" — which also pins that .cpp/.cc/.h/.hpp
     * stay unclaimed.  That the OFFLINE catalog agrees is a separate check and
     * a separate mechanism: check_script_language_catalog.py reads
     * this call and the table in engine/src/resource/jce_asset_ext.c and fails
     * when they disagree. */
    if (!jce_script_vm_register_extension("jcecpp", "cpp")) return false;

    g_ext_claimed = true;
    return true;
}

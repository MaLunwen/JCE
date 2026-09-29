/*
 * jce_script_vm_csharp.c — the "csharp" JceScriptVM.
 *
 * It is C, not C++, and that is deliberate for the same reason the cpp
 * backend gives: nothing here needs a C++ feature, and a C translation unit
 * cannot accidentally acquire a throw-through-C-frames path.
 *
 * ══ WHAT THIS FILE IS AND IS NOT ════════════════════════════════════════
 *
 * It is a HOST, not an interpreter.  It starts the machine's .NET runtime
 * once per process through nethost/hostfxr, asks the managed JceScript
 * assembly for a table of function pointers, and forwards the eighteen slots
 * into it.  Every decision about C# — how a type is found, how an instance is
 * held, what happens when a script throws — lives on the managed side, in
 * scripting/csharp/managed/JceScript/Runtime.cs.
 *
 * THE EXCEPTION BARRIER IS NOT HERE, AND CANNOT BE.  A .NET exception that
 * escapes an [UnmanagedCallersOnly] method does not unwind into C; the
 * runtime fails fast and kills the process.  There is no try/catch this file
 * could write.  The barrier is inside every managed entry point, which is the
 * same placement scripting/cpp uses for its noexcept+catch(...) thunks: on
 * the module side of the boundary.  A slot here that returned an error code
 * would be describing a state it can never observe.
 *
 * ══ WHAT IS DELIBERATELY NOT SUPPORTED, and why ═════════════════════════
 *
 * Each of these is a slot the core requires to be non-NULL, so each is an
 * EXPLICIT refusal whose reason is visible here rather than a hole nobody
 * can see:
 *
 *   instantiate_source   A C# script is compiled by `dotnet build` before the
 *                        process starts.  Answering anything but 0 would mean
 *                        silently ignoring the `source` the caller passed,
 *                        which is worse than refusing it.  Use
 *                        jce_script_instantiate with the path to the .cs file
 *                        (or the bare type name); both resolve to a type.
 *                        Compiling at runtime would need Roslyn, which ships
 *                        with the .NET SDK and not with a player's runtime.
 *
 *   update_coroutines    There is no cooperative scheduler on the managed
 *                        side: a C# script that wants to wait keeps its own
 *                        timer in a field, which is what OnUpdate is for.
 *                        async/await is available to a script and its
 *                        continuations run on the runtime's own scheduler,
 *                        not on this slot.
 *
 * compile_module / rebind_instance / release_module ARE supported and are a
 * type re-lookup, not a recompile: they exist so an editor that reloaded an
 * assembly can rebind live instances.  What they cannot preserve is script
 * FIELDS — the object is a new instance of a possibly-new type — so they
 * preserve the entity and nothing else, and say so.
 */

#include <jce/script_vm/jce_script_vm_csharp.h>

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_library.h>

#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if JCE_PLATFORM_WINDOWS
   /* Guarded: nethost.h already defines it on this platform, and redefining a
    * macro to the same value is still a warning this build treats as noise to
    * be removed rather than tolerated. */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#define LOG_TAG "script.csharp"

#define JCE_CS_PATH_MAX 1024

/* A literal in hostfxr's char_t.  One macro rather than two spellings of every
 * managed name, so the type name below cannot be right on one platform and
 * wrong on the other. */
#if JCE_PLATFORM_WINDOWS
#  define JCE_CS_T(s) L##s
#else
#  define JCE_CS_T(s) s
#endif

/* ── char_t, and the one place it is not char ────────────────────────────
 *
 * hostfxr takes `char_t`, which is wchar_t on Windows and char elsewhere.
 * Every path in this engine is UTF-8, so on Windows exactly one conversion is
 * needed and it is here.  MultiByteToWideChar and not mbstowcs: mbstowcs
 * reads the C locale, which on Windows is not UTF-8 by default, so an
 * assembly under a path with a non-ASCII character would silently not be
 * found — a failure that reproduces only on someone else's machine. */
#if JCE_PLATFORM_WINDOWS
typedef wchar_t JceCsChar;
static bool cs_widen(const char *utf8, JceCsChar *out, int cap)
{
    int n;
    if (!utf8 || !out || cap <= 0) return false;
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0 || n > cap) return false;
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap) > 0;
}
#else
typedef char JceCsChar;
static bool cs_widen(const char *utf8, JceCsChar *out, int cap)
{
    size_t n;
    if (!utf8 || !out || cap <= 0) return false;
    n = strlen(utf8);
    if (n + 1u > (size_t)cap) return false;
    memcpy(out, utf8, n + 1u);
    return true;
}
#endif

/* ── The bridge ──────────────────────────────────────────────────────────
 *
 * APPEND ONLY, and `size` is how that is enforced across the boundary: the
 * managed side writes the sizeof IT was compiled against, and this file
 * refuses a table shorter than the slots it is about to call.  A managed
 * assembly built against an older bridge therefore fails LOUDLY at startup
 * instead of having its later slots read as garbage function pointers.
 *
 * Every string is UTF-8 and owned by the caller for the duration of the call
 * only; the managed side copies with Marshal.PtrToStringUTF8 before doing
 * anything that could yield. */
typedef struct JceCsBridge {
    int32_t   size;
    int32_t   abi;

    /* host_ptr is the JceScriptHost the managed side opens the C ABI over.
     * log_fn/log_user are that host's `log` member and its user pointer,
     * passed SEPARATELY because `log` is one of the manifest's hand-written
     * entries and therefore deliberately absent from the C ABI -- managed code
     * has no other way to reach the editor console, and a barrier that could
     * not report would be a barrier that hid every script error. */
    int32_t  (*vm_open)(void *host_ptr, int64_t host_size, void *log_fn,
                        void *log_user);
    void     (*vm_close)(int32_t vm);

    uint32_t (*instantiate)(int32_t vm, const char *type_name, uint32_t owner);
    void     (*release)(int32_t vm, uint32_t inst);

    void     (*call_start)(int32_t vm, uint32_t inst);
    void     (*call_update)(int32_t vm, uint32_t inst, float dt);
    void     (*call_collision)(int32_t vm, uint32_t inst, uint32_t other);
    void     (*call_message)(int32_t vm, uint32_t inst, const char *msg,
                             double num, const char *str);
    void     (*call_anim_event)(int32_t vm, uint32_t inst, uint32_t id,
                                const char *name, float f0, float f1,
                                int32_t i0);

    int32_t  (*call_named)(int32_t vm, const char *fn, uint32_t e);
    int32_t  (*call_named_num)(int32_t vm, const char *fn, uint32_t e,
                               double v);
    int32_t  (*call_named_str)(int32_t vm, const char *fn, uint32_t e,
                               const char *v);

    int32_t  (*instance_count)(int32_t vm);

    uint32_t (*compile_module)(int32_t vm, const char *type_name);
    void     (*rebind_instance)(int32_t vm, uint32_t inst, uint32_t mod);
    void     (*release_module)(int32_t vm, uint32_t mod);

    /* The game's OWN assembly.  Without it the only types this VM can find
     * are the ones inside JceScript.dll -- which is enough for the engine's
     * diagnostics and for nothing a player would run.  Mirrors the cpp
     * backend's jce_script_vm_cpp_load_library: a compiled language needs an
     * explicit "here is the code" step, because there is no path a scene
     * could name that would imply it. */
    int32_t  (*load_assembly)(int32_t vm, const char *path);

    /* APPENDED, and it must stay last: this bridge is a LOCKSTEP
     * contract, not an append-tolerant one.  The loader rejects
     * `filled < sizeof g_bridge`, so a stale JceScript.dll fails to
     * load with a message telling you to rebuild it -- it does not
     * silently run without on_fixed_update.  JCE_CS_BRIDGE_ABI is
     * bumped for the same reason: size alone would catch it, and the
     * version number is what makes the error say WHY. */
    void     (*call_fixed_update)(int32_t vm, uint32_t inst, float dt);
} JceCsBridge;

#define JCE_CS_BRIDGE_ABI 2

/* ── Process-global host state ───────────────────────────────────────────
 *
 * ONE runtime per process, and that is hostfxr's rule rather than a choice
 * here: hostfxr_initialize_for_runtime_config succeeds once and then reports
 * an already-initialised context.  Every JceScript handle this backend hands
 * out shares it, so a scene with a hundred C# scripts starts .NET once. */
static JceLibrary   g_fxr;
static JceCsBridge  g_bridge;
static bool         g_booted;
static int          g_starts;
static char         g_assembly[JCE_CS_PATH_MAX];

JCE_API void jce_script_vm_csharp_set_assembly(const char *assembly)
{
    if (!assembly || !assembly[0]) { g_assembly[0] = '\0'; return; }
    snprintf(g_assembly, sizeof g_assembly, "%s", assembly);
}

JCE_API int jce_script_vm_csharp_runtime_starts(void) { return g_starts; }

/* Defined after the handle; declared here because set_assembly and this are
 * the backend's whole configuration surface and belong together. */
JCE_API bool jce_script_vm_csharp_load_assembly(JceScript *s, const char *path);

/* Replace the extension of `path` with `ext`, into `out`.  hostfxr finds a
 * runtimeconfig BY NAME beside the assembly, so this is how the two paths are
 * derived from one input rather than from two the caller could disagree on. */
static bool cs_swap_ext(const char *path, const char *ext, char *out,
                        size_t cap)
{
    const char *dot = strrchr(path, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    if (stem + strlen(ext) + 1u > cap) return false;
    memcpy(out, path, stem);
    memcpy(out + stem, ext, strlen(ext) + 1u);
    return true;
}

/* The assembly this backend will load: what set_assembly named, else
 * JceScript.dll beside the executable.  The same resolution order every other
 * backend's runtime inputs use, so a shipped game needs no environment. */
static bool cs_assembly_path(char *out, size_t cap)
{
    char base[JCE_CS_PATH_MAX];
    if (g_assembly[0]) {
        snprintf(out, cap, "%s", g_assembly);
        return true;
    }
    if (!jce_fs_host_get_base_path(base, (uint32_t)sizeof base) || !base[0])
        return false;
    snprintf(out, cap, "%sJceScript.dll", base);
    return true;
}

static bool cs_boot(void)
{
    char        asm_utf8[JCE_CS_PATH_MAX];
    char        cfg_utf8[JCE_CS_PATH_MAX];
    JceCsChar   fxr_path[JCE_CS_PATH_MAX];
    JceCsChar   cfg_wide[JCE_CS_PATH_MAX];
    JceCsChar   asm_wide[JCE_CS_PATH_MAX];
    size_t      fxr_len = JCE_CS_PATH_MAX;
    hostfxr_initialize_for_runtime_config_fn init_fn;
    hostfxr_get_runtime_delegate_fn          delegate_fn;
    hostfxr_close_fn                         close_fn;
    load_assembly_and_get_function_pointer_fn load_fn = NULL;
    hostfxr_handle ctx = NULL;
    int32_t (*bootstrap)(void *table, int32_t size) = NULL;
    int32_t filled;
    int     rc;

    if (g_booted) return true;

    if (!cs_assembly_path(asm_utf8, sizeof asm_utf8)) {
        LOG_ERROR(LOG_TAG,
                  "no managed assembly: jce_script_vm_csharp_set_assembly was "
                  "not called and the executable's directory is unknown.");
        return false;
    }
    if (!cs_swap_ext(asm_utf8, ".runtimeconfig.json", cfg_utf8,
                     sizeof cfg_utf8)) {
        LOG_ERROR(LOG_TAG, "assembly path too long: '%s'", asm_utf8);
        return false;
    }

    /* nethost finds the hostfxr the MACHINE installed.  Not dlopen("hostfxr")
     * by name: the library lives under a versioned directory nobody should be
     * spelling out, and get_hostfxr_path is the supported way to ask. */
    rc = get_hostfxr_path(fxr_path, &fxr_len, NULL);
    if (rc != 0) {
        LOG_ERROR(LOG_TAG,
                  "no .NET runtime on this machine (get_hostfxr_path=0x%x). "
                  "The C# backend needs a .NET runtime installed; every other "
                  "language still works.", (unsigned)rc);
        return false;
    }

    {   /* jce_library_open takes UTF-8; on Windows fxr_path is wide, so it is
         * narrowed back here rather than adding a second loader. */
#if JCE_PLATFORM_WINDOWS
        char narrow[JCE_CS_PATH_MAX];
        if (WideCharToMultiByte(CP_UTF8, 0, fxr_path, -1, narrow,
                                (int)sizeof narrow, NULL, NULL) <= 0) {
            LOG_ERROR(LOG_TAG, "hostfxr path is not representable as UTF-8");
            return false;
        }
        g_fxr = jce_library_open(narrow);
#else
        g_fxr = jce_library_open(fxr_path);
#endif
    }
    if (!g_fxr) {
        LOG_ERROR(LOG_TAG, "could not load hostfxr");
        return false;
    }

    init_fn = (hostfxr_initialize_for_runtime_config_fn)jce_library_symbol(
        g_fxr, "hostfxr_initialize_for_runtime_config");
    delegate_fn = (hostfxr_get_runtime_delegate_fn)jce_library_symbol(
        g_fxr, "hostfxr_get_runtime_delegate");
    close_fn = (hostfxr_close_fn)jce_library_symbol(g_fxr, "hostfxr_close");
    if (!init_fn || !delegate_fn || !close_fn) {
        LOG_ERROR(LOG_TAG, "hostfxr is missing an entry point");
        jce_library_close(g_fxr);
        g_fxr = NULL;
        return false;
    }

    if (!cs_widen(cfg_utf8, cfg_wide, JCE_CS_PATH_MAX) ||
        !cs_widen(asm_utf8, asm_wide, JCE_CS_PATH_MAX)) {
        LOG_ERROR(LOG_TAG, "path is not representable: '%s'", asm_utf8);
        jce_library_close(g_fxr);
        g_fxr = NULL;
        return false;
    }

    /* Success codes from hostfxr are 0 and the two "already initialised"
     * values (1 and 2); anything else is a failure.  Checking `rc != 0` alone
     * would refuse a process that legitimately hosted .NET before us. */
    rc = init_fn(cfg_wide, NULL, &ctx);
    if ((rc != 0 && rc != 1 && rc != 2) || !ctx) {
        LOG_ERROR(LOG_TAG,
                  "hostfxr could not initialise from '%s' (0x%x). That file is "
                  "produced beside the assembly by `dotnet build`; a missing "
                  "one usually means scripting/csharp/build_csharp.py has not "
                  "run.", cfg_utf8, (unsigned)rc);
        if (ctx) close_fn(ctx);
        jce_library_close(g_fxr);
        g_fxr = NULL;
        return false;
    }

    rc = delegate_fn(ctx, hdt_load_assembly_and_get_function_pointer,
                     (void **)&load_fn);
    close_fn(ctx);                       /* the delegate outlives the context */
    if (rc != 0 || !load_fn) {
        LOG_ERROR(LOG_TAG, "hostfxr gave no load delegate (0x%x)",
                  (unsigned)rc);
        jce_library_close(g_fxr);
        g_fxr = NULL;
        return false;
    }

    /* UNMANAGEDCALLERSONLY_METHOD, not a delegate type name: Bootstrap is
     * [UnmanagedCallersOnly], so there is no managed delegate to name and the
     * runtime hands back the raw entry point. */
    rc = load_fn(asm_wide, JCE_CS_T("JceScript.Runtime, JceScript"),
                 JCE_CS_T("Bootstrap"), UNMANAGEDCALLERSONLY_METHOD, NULL,
                 (void **)&bootstrap);
    if (rc != 0 || !bootstrap) {
        LOG_ERROR(LOG_TAG,
                  "'%s' has no JceScript.Runtime.Bootstrap (0x%x). The "
                  "assembly beside the executable is not the one this engine "
                  "was built against.", asm_utf8, (unsigned)rc);
        jce_library_close(g_fxr);
        g_fxr = NULL;
        return false;
    }

    memset(&g_bridge, 0, sizeof g_bridge);
    g_bridge.size = (int32_t)sizeof g_bridge;
    g_bridge.abi  = JCE_CS_BRIDGE_ABI;
    filled = bootstrap(&g_bridge, (int32_t)sizeof g_bridge);
    if (filled < (int32_t)sizeof g_bridge || g_bridge.abi != JCE_CS_BRIDGE_ABI
        || !g_bridge.vm_open) {
        LOG_ERROR(LOG_TAG,
                  "the managed bridge is not the one this build expects "
                  "(filled %d of %d, abi %d wanted %d). Rebuild the managed "
                  "assembly with scripting/csharp/build_csharp.py.",
                  (int)filled, (int)sizeof g_bridge, (int)g_bridge.abi,
                  JCE_CS_BRIDGE_ABI);
        jce_library_close(g_fxr);
        g_fxr = NULL;
        return false;
    }

    ++g_starts;
    g_booted = true;
    LOG_INFO(LOG_TAG, ".NET runtime started, managed bridge bound (%s)",
             asm_utf8);
    return true;
}

/* ── The handle ──────────────────────────────────────────────────────────
 *
 * JceScriptVMHeader FIRST.  jce_script_vm_create() refuses a handle whose
 * first word is not the table it dispatched through, because every forwarder
 * reads the vtable from offset 0. */
typedef struct JceCsScript {
    JceScriptVMHeader hdr;
    JceScriptHost     host;
    bool              have_host;
    int32_t           vm;               /* the managed VM id; 0 == none */
    bool              warned_source;
} JceCsScript;

static const JceScriptVM *jce_script_vm_csharp_table(void);

static JceScript *cs_create_sized(const JceScriptHost *host, size_t host_size)
{
    JceCsScript *s;

    if (!cs_boot()) return NULL;

    s = (JceCsScript *)jce_malloc(sizeof *s);
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->hdr.vm = jce_script_vm_csharp_table();

    if (host && host_size > 0u) {
        /* min(caller, ours) over a ZEROED destination.  JceScriptHost is
         * caller-allocated and grows; copying at our own sizeof over-reads a
         * consumer built against an older header. */
        const size_t n = host_size < sizeof s->host ? host_size
                                                    : sizeof s->host;
        memcpy(&s->host, host, n);
        s->have_host = true;
    }

    /* &s->host and not the caller's pointer: the managed side opens the C ABI
     * over it and keeps it for the VM's lifetime, and the caller's JceScriptHost
     * is usually a stack local that is gone by the next line. */
    s->vm = g_bridge.vm_open(s->have_host ? (void *)&s->host : NULL,
                             (int64_t)sizeof s->host,
                             s->have_host ? (void *)s->host.log : NULL,
                             s->have_host ? s->host.user : NULL);
    if (s->vm == 0) {
        LOG_ERROR(LOG_TAG, "the managed runtime refused to open a VM");
        jce_free(s);
        return NULL;
    }
    return (JceScript *)s;
}

static void cs_destroy(JceScript *sc)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s) return;
    if (s->vm && g_bridge.vm_close) g_bridge.vm_close(s->vm);
    jce_free(s);
}

/* `path` is a REFERENCE: "Assets/Turret.cs" names the type Turret, and the
 * bytes at that path are never read.  The managed side does the mapping,
 * because "which type does this path mean" is a C# question (namespaces,
 * nested types, an assembly-qualified name) and answering half of it here
 * would put the rule in two places. */
static JceScriptInstance cs_instantiate(JceScript *sc, const char *path,
                                        JceScriptEntity owner)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm || !path) return 0u;
    return (JceScriptInstance)g_bridge.instantiate(s->vm, path,
                                                   (uint32_t)owner);
}

static JceScriptInstance cs_instantiate_source(JceScript *sc, const char *name,
                                               const char *source,
                                               JceScriptEntity owner)
{
    JceCsScript *s = (JceCsScript *)sc;
    (void)source; (void)owner;
    if (!s) return 0u;
    if (!s->warned_source) {
        s->warned_source = true;
        LOG_ERROR(LOG_TAG,
                  "jce_script_instantiate_source('%s') is not supported by the "
                  "csharp VM: a C# script is compiled by `dotnet build` before "
                  "the process starts, so there is no source to run. Use "
                  "jce_script_instantiate(s, \"Assets/Turret.cs\", owner) or "
                  "the bare type name.", name ? name : "?");
    }
    return 0u;
}

static void cs_release(JceScript *sc, JceScriptInstance inst)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm) return;
    g_bridge.release(s->vm, (uint32_t)inst);
}

static void cs_call_start(JceScript *sc, JceScriptInstance inst)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm) return;
    g_bridge.call_start(s->vm, (uint32_t)inst);
}

static void cs_call_update(JceScript *sc, JceScriptInstance inst, float dt)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm) return;
    g_bridge.call_update(s->vm, (uint32_t)inst, dt);
}

/* The FIXED-step half.  The NULL check is belt-and-braces rather than
 * tolerance: the loader already refuses a bridge that filled fewer bytes
 * than this build expects, so a partially-filled table never reaches here.
 * It costs one predictable branch and turns a hypothetical zero into a
 * missed callback instead of a crash in a player's build. */
static void cs_call_fixed_update(JceScript *sc, JceScriptInstance inst,
                                 float dt)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !g_bridge.call_fixed_update) return;
    g_bridge.call_fixed_update(s->vm, (uint32_t)inst, dt);
}

static void cs_call_collision(JceScript *sc, JceScriptInstance inst,
                              JceScriptEntity other)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm) return;
    g_bridge.call_collision(s->vm, (uint32_t)inst, (uint32_t)other);
}

static void cs_call_message(JceScript *sc, JceScriptInstance inst,
                            const char *msg, double num, const char *str)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm) return;
    g_bridge.call_message(s->vm, (uint32_t)inst, msg ? msg : "", num, str);
}

static void cs_call_anim_event(JceScript *sc, JceScriptInstance inst,
                               uint32_t id, const char *name, float f0,
                               float f1, int i0)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm) return;
    g_bridge.call_anim_event(s->vm, (uint32_t)inst, id, name, f0, f1,
                             (int32_t)i0);
}

static bool cs_call_named(JceScript *sc, const char *fn, JceScriptEntity e)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm || !fn) return false;
    return g_bridge.call_named(s->vm, fn, (uint32_t)e) != 0;
}

static bool cs_call_named_num(JceScript *sc, const char *fn, JceScriptEntity e,
                              double v)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm || !fn) return false;
    return g_bridge.call_named_num(s->vm, fn, (uint32_t)e, v) != 0;
}

static bool cs_call_named_str(JceScript *sc, const char *fn, JceScriptEntity e,
                              const char *str)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm || !fn) return false;
    return g_bridge.call_named_str(s->vm, fn, (uint32_t)e, str) != 0;
}

static int cs_instance_count(const JceScript *sc)
{
    const JceCsScript *s = (const JceCsScript *)sc;
    if (!s || !s->vm) return 0;
    return (int)g_bridge.instance_count(s->vm);
}

/* No cooperative scheduler; see the file header.  An explicit no-op rather
 * than a NULL slot, because the registry refuses a table with a NULL slot and
 * a backend that answered NULL here would not register at all. */
static void cs_update_coroutines(JceScript *sc, float dt)
{
    (void)sc; (void)dt;
}

static JceScriptModule cs_compile_module(JceScript *sc, const char *name,
                                         const char *source, size_t len)
{
    JceCsScript *s = (JceCsScript *)sc;
    (void)source; (void)len;
    if (!s || !s->vm || !name) return 0u;
    /* A type RE-LOOKUP, not a compile: `source` is ignored on purpose and the
     * name is resolved the way instantiate resolves it.  This is what lets an
     * editor rebind live instances after reloading an assembly. */
    return (JceScriptModule)g_bridge.compile_module(s->vm, name);
}

static void cs_rebind_instance(JceScript *sc, JceScriptInstance inst,
                               JceScriptModule mod)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm) return;
    g_bridge.rebind_instance(s->vm, (uint32_t)inst, (uint32_t)mod);
}

static void cs_release_module(JceScript *sc, JceScriptModule mod)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm) return;
    g_bridge.release_module(s->vm, (uint32_t)mod);
}

/* ── The table ───────────────────────────────────────────────────────────
 *
 * POSITIONAL initialisers, deliberately.  Every slot must be non-NULL and the
 * registry checks that, but it cannot check that a slot holds the RIGHT
 * function — a designated initialiser that named the wrong field would leave
 * one slot NULL (caught) and one slot wrong (not caught).  Written
 * positionally, a slot inserted upstream shifts everything and the compiler
 * reports the type mismatch. */
static const JceScriptVM k_cs_vm = {
    sizeof(JceScriptVM),
    JCE_SCRIPT_VM_CSHARP_LANGUAGE,
    cs_create_sized,
    cs_destroy,
    cs_instantiate,
    cs_instantiate_source,
    cs_call_start,
    cs_call_update,
    cs_release,
    cs_call_collision,
    cs_call_message,
    cs_call_anim_event,
    cs_call_named,
    cs_call_named_num,
    cs_call_named_str,
    cs_instance_count,
    cs_update_coroutines,
    cs_compile_module,
    cs_rebind_instance,
    cs_release_module,
    cs_call_fixed_update,   /* APPENDED -- see jce_script_vm.h */
};

static const JceScriptVM *jce_script_vm_csharp_table(void) { return &k_cs_vm; }

JCE_API bool jce_script_vm_csharp_load_assembly(JceScript *sc, const char *path)
{
    JceCsScript *s = (JceCsScript *)sc;
    if (!s || !s->vm || !path || !path[0]) return false;
    /* Checked against OUR table and not merely non-NULL: a JceScript from a
     * different language would have a different layout at s->vm and this
     * would read whatever sits there. */
    if (s->hdr.vm != &k_cs_vm) {
        LOG_ERROR(LOG_TAG,
                  "jce_script_vm_csharp_load_assembly called with a handle "
                  "from another language");
        return false;
    }
    return g_bridge.load_assembly(s->vm, path) != 0;
}

JCE_API bool jce_script_vm_csharp_register(void)
{
    static bool registered;
    if (registered) return true;

    /* BOOT FIRST.  Registering a language whose runtime is absent would put
     * "csharp" in the registry and claim ".cs", so every .cs in a project
     * would resolve to a VM that refuses to create — a scene full of scripts
     * that silently do nothing.  Refusing here leaves .cs unclaimed, which is
     * the truthful state, and the log line says why. */
    if (!cs_boot()) return false;

    if (!jce_script_vm_register(&k_cs_vm)) return false;

    /* Claim the extension, or nothing authored in C# is ever SELECTED: the
     * runtime resolves each Script component's path to a language through
     * jce_script_vm_language_for_path(), and that answer comes from claims
     * like this one. */
    if (!jce_script_vm_register_extension(JCE_SCRIPT_VM_CSHARP_EXTENSION,
                                          JCE_SCRIPT_VM_CSHARP_LANGUAGE)) {
        LOG_ERROR(LOG_TAG, "could not claim ." JCE_SCRIPT_VM_CSHARP_EXTENSION);
        return false;
    }
    registered = true;
    return true;
}

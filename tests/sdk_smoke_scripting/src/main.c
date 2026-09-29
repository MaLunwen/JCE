/*
 * sdk_smoke_scripting main.c — the multi-language SDK consumer, in plain C99.
 *
 * tests/sdk_smoke proves a foreign project can link the engine and boot it.
 * This one proves the thing that had no proof at all until the SDK started
 * installing scripting/: that a project which is NOT this repository can run
 * a script in a language the engine does not contain.
 *
 * What each step is for, and what its absence used to look like:
 *
 *   1. lua claims ".lua" before anything registers.  THE POSITIVE CONTROL.
 *      Without it, step 2's "nothing claims .py" is indistinguishable from
 *      "the registry is empty", and this file would pass on an engine whose
 *      script layer never initialised.  Silence compares equal to silence.
 *   2. nothing claims ".py" yet.
 *   3. the shipped jce_script package goes on sys.path, and the backend
 *      registers.  An SDK that installed jce_script_vm_python.lib and not the
 *      package links, boots, and dies at the first script with an ImportError
 *      that names Python — never the SDK.
 *   4. ".py" now resolves to "python".
 *   5. a VM is created through the PUBLIC registry (jce_script_vm_create),
 *      not through the backend's own entry point.
 *   6. THE SCRIPT COMES OUT OF THE EMBEDDED PAK.  This host's read_file
 *      reads only the archive — never the source tree — because a
 *      packaged-build-only failure is the worst kind: the editor and every
 *      loose-file run keep working while the shipped game silently has no
 *      script.  `jce_cook --batch` used to rename smoke_py.py to
 *      smoke_py.jceasset unless --preserve-names was passed, which resolves
 *      to NO language; the cooker now refuses to rename a script whatever
 *      the flag says (tests/os/resource/test_jce_cook_script_names.c pins
 *      that with the real cooker).  This step still notices if the bytes
 *      stop arriving under the name the scene stores, for any reason.
 *   7. on_start ran exactly once and on_update exactly three times, counted
 *      in C from literal markers the script logs.  BOTH numbers are asserted
 *      to be nonzero independently before they are compared to anything.
 *
 * Success contract for the runner: exactly one "JCE_SMOKE_SCRIPTING: OK ..."
 * line and exit 0.  Any FAIL line, or a missing OK marker, fails the gate —
 * the exit code alone is not trusted, because an init failure leaves through
 * the engine's own error path.
 */

#include <jce/api.h>
#include <jce/application/jce_main.h>
#include <jce/jce_version.h>
#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/resource/jce_pak_loader.h>

/* Flat, with quotes, EXACTLY as an in-tree consumer spells it.  The SDK
 * installs this header at include/ rather than under include/jce/script_vm/
 * for that reason alone: one spelling, both worlds. */
#ifdef JCE_SMOKE_HAVE_PYTHON
#  include "jce_script_vm_python.h"
#endif

#ifdef JCE_SMOKE_HAVE_C
#  include <jce/script_vm/jce_script_vm_c.h>
#endif
#ifdef JCE_SMOKE_HAVE_JAVA
#  include <jce/script_vm/jce_script_vm_java.h>
#endif
#ifdef JCE_SMOKE_HAVE_CPP
#  include <jce/script_vm/jce_script_vm_cpp.h>
#endif
#ifdef JCE_SMOKE_HAVE_JS
#  include <jce/script_vm/jce_script_vm_js.h>
#endif
#ifdef JCE_SMOKE_HAVE_CSHARP
#  include <jce/script_vm/jce_script_vm_csharp.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_MAX_LOG 64

typedef struct SmokeHostState {
    const JcePakArchive *pak;
    int   starts;
    int   updates;
    int   log_lines;
    int   pak_reads;
    char  first_unexpected[256];
} SmokeHostState;

static SmokeHostState s_state;
static int   s_frames;
static float s_elapsed;
static bool  s_ok;

static void fail(const char *fmt, ...)
{
    va_list ap;
    printf("JCE_SMOKE_SCRIPTING: FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ *
 *  The host.  Two callbacks are load-bearing here and the rest stay
 *  NULL on purpose: a host that filled them in would be testing the
 *  engine's bindings, and what is under test is the SDK's packaging.
 * ------------------------------------------------------------------ */
static void smoke_log(void *user, const char *msg)
{
    SmokeHostState *st = (SmokeHostState *)user;
    if (!st || !msg) return;
    st->log_lines++;
    if (strcmp(msg, "SMOKE_PY_START") == 0) {
        st->starts++;
    } else if (strcmp(msg, "SMOKE_PY_UPDATE") == 0) {
        st->updates++;
    } else if (st->first_unexpected[0] == '\0') {
        /* Kept and reported rather than ignored: the VM routes a caught
         * script error to host.log, so an unexpected line here is usually
         * the real diagnosis of a zero count below. */
        snprintf(st->first_unexpected, sizeof st->first_unexpected, "%s", msg);
    }
}

/* PAK ONLY.  The engine's own runtime host tries the host filesystem first
 * and falls back to the archive; this one deliberately does not, so that
 * "the script was found" cannot mean "someone's source tree was lying
 * around next to the executable". */
static void *smoke_read_file(void *user, const char *path, uint64_t *out_size)
{
    SmokeHostState    *st = (SmokeHostState *)user;
    const JcePakAsset *a;
    void              *buf;
    size_t             n;

    if (out_size) *out_size = 0;
    if (!st || !st->pak || !path) return NULL;
    a = jce_pak_find(st->pak, path);
    if (!a || a->original_size == 0) return NULL;
    buf = jce_malloc((size_t)a->original_size);
    if (!buf) return NULL;
    n = jce_pak_decompress(a, buf, (size_t)a->original_size);
    if (n == 0) { jce_free(buf); return NULL; }
    st->pak_reads++;
    if (out_size) *out_size = (uint64_t)n;
    return buf;
}

static void smoke_host_init(JceScriptHost *h)
{
    memset(h, 0, sizeof *h);
    h->user      = &s_state;
    h->log       = smoke_log;
    h->read_file = smoke_read_file;
}

#ifdef JCE_SMOKE_HAVE_C
static bool smoke_c(const JceScriptHost *host)
{
    JceScript *cs;
    const char *lang;

    if (!jce_script_vm_c_register()) {
        fail("c: register returned false");
        return false;
    }
    lang = jce_script_vm_language_for_path("scripts/Thing.jcec");
    if (!lang || strcmp(lang, "c") != 0) {
        fail("c: '.jcec' resolves to '%s', expected 'c'",
             lang ? lang : "(nothing)");
        return false;
    }
    cs = jce_script_vm_create("c", host, sizeof *host);
    if (!cs) {
        fail("c: jce_script_vm_create returned NULL");
        return false;
    }
    if (jce_script_vm_cpp_module_count() != 0) {
        fail("c: shared native module_count=%d on an empty registry",
             jce_script_vm_cpp_module_count());
        jce_script_destroy(cs);
        return false;
    }
    jce_script_destroy(cs);
    printf("JCE_SMOKE_SCRIPTING: c OK (registry live, .jcec claimed)\n");
    return true;
}
#endif

/* ------------------------------------------------------------------ *
 *  The optional languages.  Each one REPORTS which branch it took —
 *  an SDK that shipped a backend and a smoke that quietly did not
 *  exercise it is the "150/151 dead" shape this repository keeps
 *  finding, and the cure is that skipping is never silent.
 * ------------------------------------------------------------------ */
#ifdef JCE_SMOKE_HAVE_JAVA
static bool smoke_java(const JceScriptHost *host)
{
    JceScriptVmJavaConfig cfg;
    const char *opts[1];
    JceScript  *js;
    const char *lang;

    memset(&cfg, 0, sizeof cfg);
    cfg.jvm_library = JCE_SMOKE_JAVA_JVM;
    cfg.class_path  = JCE_SMOKE_JAVA_CLASS_PATH;
    /* The JNI shim by ABSOLUTE path.  System.loadLibrary("jce_script_java")
     * is the fallback and it searches java.library.path, which an SDK
     * consumer has no reason to have set — the SDK knows where it put the
     * file, so it says so. */
    opts[0] = "-Djce.script.library=" JCE_SMOKE_JAVA_SCRIPT_LIB;
    cfg.options      = opts;
    cfg.option_count = 1;

    if (!jce_script_vm_java_configure(&cfg, sizeof cfg)) {
        fail("java: configure refused (jvm=%s)", JCE_SMOKE_JAVA_JVM);
        return false;
    }
    if (!jce_script_vm_java_register()) {
        fail("java: register returned false");
        return false;
    }
    lang = jce_script_vm_language_for_path("scripts/Thing.java");
    if (!lang || strcmp(lang, "java") != 0) {
        fail("java: '.java' resolves to '%s', expected 'java'",
             lang ? lang : "(nothing)");
        return false;
    }
    /* Creating the VM STARTS THE JVM, loads com.jce.script.JceScript off the
     * class path this SDK shipped, and that class loads the JNI shim this SDK
     * shipped.  All three installed pieces are checked by one call, and each
     * of them failing produces a different message from inside the JVM. */
    js = jce_script_vm_create("java", host, sizeof *host);
    if (!js) {
        fail("java: jce_script_vm_create returned NULL (classpath=%s)",
             JCE_SMOKE_JAVA_CLASS_PATH);
        return false;
    }
    if (jce_script_vm_java_jvm_creations() != 1) {
        fail("java: jvm_creations=%d, expected 1",
             jce_script_vm_java_jvm_creations());
        jce_script_destroy(js);
        return false;
    }
    jce_script_destroy(js);
    printf("JCE_SMOKE_SCRIPTING: java OK (jvm created once, classes+shim "
           "from the SDK)\n");
    return true;
}
#endif

#ifdef JCE_SMOKE_HAVE_CPP
static bool smoke_cpp(const JceScriptHost *host)
{
    JceScript *cs;

    if (!jce_script_vm_cpp_register()) {
        fail("cpp: register returned false");
        return false;
    }
    /* THE CPP BACKEND CLAIMS NO EXTENSION, and that is a real consequence
     * rather than an oversight: a C++ script is a class compiled into the
     * game, so there is no file for jce_script_vm_language_for_path() to
     * answer about.  Asserted here so the SDK's copy of that behaviour is
     * recorded rather than discovered by a project that names a .escpp. */
    if (jce_script_vm_language_for_path("scripts/Thing.cpp") != NULL) {
        fail("cpp: something now claims '.cpp'");
        return false;
    }
    cs = jce_script_vm_create("cpp", host, sizeof *host);
    if (!cs) {
        fail("cpp: jce_script_vm_create returned NULL");
        return false;
    }
    if (jce_script_vm_cpp_module_count() != 0) {
        fail("cpp: module_count=%d on a VM with no modules added",
             jce_script_vm_cpp_module_count());
        jce_script_destroy(cs);
        return false;
    }
    jce_script_destroy(cs);
    printf("JCE_SMOKE_SCRIPTING: cpp OK (registry live, 0 modules)\n");
    return true;
}
#endif

#ifdef JCE_SMOKE_HAVE_JS
/* The js backend links a THIRD-PARTY static library (quickjs-ng) that the SDK
 * installs beside its own.  That is the part only an SDK consumer can prove:
 * an install rule that shipped jce_script_vm_js.lib and forgot qjs.lib links
 * clean in THIS tree, where the conan graph is present, and fails in a user
 * project, where it is not.  So this runs JavaScript end to end.
 *
 * The observable is the exception barrier rather than a `jce` call, because
 * this smoke has no host members set and every binding would correctly
 * answer its absent value.  A throw that reaches host.log proves the whole
 * path anyway -- the VM was created, the source compiled and RAN inside
 * quickjs, on_start was dispatched, and the barrier caught it.  Nothing
 * short of a working qjs.lib gets there. */
static bool smoke_js(const JceScriptHost *host)
{
    JceScript        *js;
    JceScriptInstance inst;
    int               before = s_state.log_lines;

    if (!jce_script_vm_js_register()) {
        fail("js: register returned false");
        return false;
    }
    if (strcmp(jce_script_vm_language_for_path("a." JCE_SCRIPT_VM_JS_EXTENSION),
               JCE_SCRIPT_VM_JS_LANGUAGE) != 0) {
        fail("js: the SDK build did not claim ." JCE_SCRIPT_VM_JS_EXTENSION);
        return false;
    }
    js = jce_script_vm_create(JCE_SCRIPT_VM_JS_LANGUAGE, host, sizeof *host);
    if (!js) {
        fail("js: jce_script_vm_create returned NULL");
        return false;
    }
    inst = jce_script_instantiate_source(js, "@smoke",
        "const M = {};\n"
        "M.on_start = function () { throw new Error('reached'); };\n"
        "return M;\n", 1u);
    if (inst == 0u) {
        fail("js: instantiate_source refused a valid script");
        jce_script_destroy(js);
        return false;
    }
    jce_script_call_start(js, inst);
    if (s_state.log_lines == before) {
        fail("js: on_start never ran -- the script never reached quickjs");
        jce_script_release(js, inst);
        jce_script_destroy(js);
        return false;
    }
    jce_script_release(js, inst);
    jce_script_destroy(js);
    printf("JCE_SMOKE_SCRIPTING: js OK (quickjs from the SDK ran a script)\n");
    return true;
}
#endif

#ifdef JCE_SMOKE_HAVE_CSHARP
/* The C# backend hosts the MACHINE's .NET runtime and loads a managed
 * assembly the SDK shipped.  Three separate things can be missing and only a
 * consumer build can tell them apart: the nethost import library (a link
 * error here), the nethost runtime (a process that does not start), and the
 * managed assembly (a register() that refuses).  So this runs a real script.
 *
 * The observable is a DIAGNOSTIC TYPE that ships inside JceScript.dll, so no
 * second .NET project is needed -- and it drives the engine through the
 * generated Jce surface, which means a pass also proves the c_abi shared
 * library was found by the managed loader. */
static bool smoke_csharp(const JceScriptHost *host)
{
    JceScript        *cs;
    JceScriptInstance inst;
    int               before = s_state.log_lines;

    jce_script_vm_csharp_set_assembly(JCE_SMOKE_CSHARP_ASSEMBLY);
    if (!jce_script_vm_csharp_register()) {
        fail("csharp: register returned false (no .NET runtime, or the SDK's "
             "managed assembly is not at " JCE_SMOKE_CSHARP_ASSEMBLY ")");
        return false;
    }
    if (strcmp(jce_script_vm_language_for_path("a." JCE_SCRIPT_VM_CSHARP_EXTENSION),
               JCE_SCRIPT_VM_CSHARP_LANGUAGE) != 0) {
        fail("csharp: the SDK build did not claim ." JCE_SCRIPT_VM_CSHARP_EXTENSION);
        return false;
    }
    cs = jce_script_vm_create(JCE_SCRIPT_VM_CSHARP_LANGUAGE, host, sizeof *host);
    if (!cs) {
        fail("csharp: jce_script_vm_create returned NULL");
        return false;
    }
    inst = jce_script_instantiate(cs, "JceScript.Diagnostics.ThrowingScript", 1u);
    if (inst == 0u) {
        fail("csharp: the SDK's managed assembly has no diagnostic type");
        jce_script_destroy(cs);
        return false;
    }
    /* A deliberate throw: the barrier catches it on the MANAGED side and
     * reports through host.log.  Reaching this line at all proves the .NET
     * runtime started; the log line proves the managed half is the one this
     * engine was built against. */
    jce_script_call_start(cs, inst);
    if (s_state.log_lines == before) {
        fail("csharp: the script never ran -- .NET started but the managed "
             "bridge did not dispatch");
        jce_script_release(cs, inst);
        jce_script_destroy(cs);
        return false;
    }
    jce_script_release(cs, inst);
    jce_script_destroy(cs);
    printf("JCE_SMOKE_SCRIPTING: csharp OK (.NET from the SDK ran a script)\n");
    return true;
}
#endif

/* ------------------------------------------------------------------ */
static bool smoke_init(const JceServices *svc, void *ud)
{
    JceScriptHost     host;
#ifdef JCE_SMOKE_HAVE_PYTHON
    JceScript        *s;
    JceScriptInstance inst;
    const JcePakAsset *asset;
    int               i;
#endif
    const char       *lang;

    (void)ud;

    if ((jce_api_version() >> 24) != (uint32_t)JCE_VERSION_MAJOR) {
        fail("api major mismatch: lib=0x%08x hdr=%d",
             jce_api_version(), JCE_VERSION_MAJOR);
        return false;
    }
    if (!svc || !svc->pak) {
        fail("engine booted without the embedded pak");
        return false;
    }
    s_state.pak = svc->pak;

    /* 1. POSITIVE CONTROL — the registry is alive and lua is in it. */
    lang = jce_script_vm_language_for_path("scripts/anything.lua");
    if (!lang || strcmp(lang, "lua") != 0) {
        fail("the script registry does not even claim '.lua' (%s) — every "
             "assertion below would pass vacuously",
             lang ? lang : "(nothing)");
        return false;
    }

    smoke_host_init(&host);

#ifdef JCE_SMOKE_HAVE_PYTHON
    /* 2. and nothing claims '.py' yet. */
    lang = jce_script_vm_language_for_path("scripts/smoke_py.py");
    if (lang) {
        fail("'.py' already resolves to '%s' before registration", lang);
        return false;
    }

    /* 3. the SDK's own copy of the package, then the backend. */
    if (!jce_script_vm_python_add_path(JCE_SMOKE_PY_PACKAGE_DIR)) {
        fail("could not add the SDK's jce_script package dir to sys.path: %s",
             JCE_SMOKE_PY_PACKAGE_DIR);
        return false;
    }
    if (!jce_script_vm_python_register()) {
        fail("jce_script_vm_python_register() returned false");
        return false;
    }

    /* 4. */
    lang = jce_script_vm_language_for_path("scripts/smoke_py.py");
    if (!lang || strcmp(lang, "python") != 0) {
        fail("'.py' resolves to '%s' after registration, expected 'python'",
             lang ? lang : "(nothing)");
        return false;
    }

    /* 5. THE PUBLIC registry, not jce_script_vm_python(). */
    s = jce_script_vm_create("python", &host, sizeof host);
    if (!s) {
        fail("jce_script_vm_create(\"python\") returned NULL");
        return false;
    }

    /* 6a. the bytes really are in the archive, and they are the SCRIPT and
     *     not a cooked container: a rename or a wrapper here is the packaged-
     *     only death this file exists to catch, and it is worth naming
     *     separately from "instantiate failed". */
    asset = jce_pak_find(svc->pak, "scripts/smoke_py.py");
    if (!asset) {
        fail("'scripts/smoke_py.py' is not in the embedded pak — the cook or "
             "the pack step dropped it, or something renamed it. jce_cook is "
             "no longer a candidate: it refuses to rename a script even "
             "without --preserve-names.");
        jce_script_destroy(s);
        return false;
    }
    {
        char  *bytes = (char *)malloc((size_t)asset->original_size + 1u);
        size_t n = 0;
        bool   is_source;
        if (!bytes) { jce_script_destroy(s); return false; }
        n = jce_pak_decompress(asset, bytes, (size_t)asset->original_size);
        bytes[n] = '\0';
        is_source = (n > 0) &&
                    strstr(bytes, "JCE_SDK_SMOKE_SCRIPT_MARKER") != NULL;
        if (!is_source) {
            fail("the packed 'scripts/smoke_py.py' is %u bytes and does not "
                 "contain the script's own marker — it was transformed, not "
                 "packed", (unsigned)n);
            free(bytes);
            jce_script_destroy(s);
            return false;
        }
        free(bytes);
    }

    /* 6b. and the VM reads them through OUR host, from that archive. */
    inst = jce_script_instantiate(s, "scripts/smoke_py.py", 1u);
    if (inst == 0u) {
        fail("jce_script_instantiate failed%s%s",
             s_state.first_unexpected[0] ? "; host log said: " : "",
             s_state.first_unexpected);
        jce_script_destroy(s);
        return false;
    }
    if (s_state.pak_reads == 0) {
        fail("the script instantiated without a single read through our "
             "read_file — it did not come from the pak");
        jce_script_destroy(s);
        return false;
    }
    if (jce_script_instance_count(s) != 1) {
        fail("instance_count=%d, expected 1", jce_script_instance_count(s));
        jce_script_destroy(s);
        return false;
    }

    /* 7. lifecycle.  Each count is checked for BEING NONZERO before it is
     *    compared, because "0 == 0" is how a differential ships dead. */
    jce_script_call_start(s, inst);
    for (i = 0; i < 3; ++i)
        jce_script_call_update(s, inst, 1.0f / 60.0f);

    if (s_state.starts == 0) {
        fail("on_start never reached the host (log lines seen: %d%s%s)",
             s_state.log_lines,
             s_state.first_unexpected[0] ? "; first other line: " : "",
             s_state.first_unexpected);
        jce_script_destroy(s);
        return false;
    }
    if (s_state.updates == 0) {
        fail("on_update never reached the host (log lines seen: %d%s%s)",
             s_state.log_lines,
             s_state.first_unexpected[0] ? "; first other line: " : "",
             s_state.first_unexpected);
        jce_script_destroy(s);
        return false;
    }
    if (s_state.starts != 1 || s_state.updates != 3) {
        fail("lifecycle counts wrong: on_start=%d (want 1) on_update=%d "
             "(want 3)", s_state.starts, s_state.updates);
        jce_script_destroy(s);
        return false;
    }
    jce_script_destroy(s);

    printf("JCE_SMOKE_SCRIPTING: python OK (package %s, CPython %s, "
           "pak reads %d, on_start %d, on_update %d)\n",
           JCE_SMOKE_PY_PACKAGE_DIR, JCE_SMOKE_PY_SDK_VERSION,
           s_state.pak_reads, s_state.starts, s_state.updates);
#else
    printf("JCE_SMOKE_SCRIPTING: python SKIPPED - not in installed roster\n");
#endif

#ifdef JCE_SMOKE_HAVE_C
    if (!smoke_c(&host)) return false;
#else
    printf("JCE_SMOKE_SCRIPTING: c SKIPPED - not in installed roster\n");
#endif

#ifdef JCE_SMOKE_HAVE_CPP
    if (!smoke_cpp(&host)) return false;
#else
    printf("JCE_SMOKE_SCRIPTING: cpp SKIPPED — not in this SDK\n");
#endif
#ifdef JCE_SMOKE_HAVE_JS
    if (!smoke_js(&host)) return false;
#else
    printf("JCE_SMOKE_SCRIPTING: js SKIPPED - not in this SDK\n");
#endif
#ifdef JCE_SMOKE_HAVE_CSHARP
    if (!smoke_csharp(&host)) return false;
#else
    printf("JCE_SMOKE_SCRIPTING: csharp SKIPPED - not in this SDK\n");
#endif
#ifdef JCE_SMOKE_HAVE_JAVA
    if (!smoke_java(&host)) return false;
#else
    printf("JCE_SMOKE_SCRIPTING: java SKIPPED — not in this SDK, or no JVM "
           "on this machine\n");
#endif

    printf("JCE_SMOKE_SCRIPTING: OK version=%s languages=%d\n",
           jce_api_version_string(), jce_script_vm_count());
    fflush(stdout);
    s_ok = true;
    return true;
}

static void smoke_update(float dt, void *ud)
{
    (void)ud;
    s_frames++;
    s_elapsed += dt;
}

static bool smoke_should_quit(void *ud)
{
    (void)ud;
    /* !s_ok can only be reached if init returned true without setting it,
     * which is a bug in this file rather than in the SDK — quit at once
     * rather than spin, so the missing OK line is the whole report. */
    return !s_ok || (s_elapsed >= 0.25f && s_frames >= 10);
}

static JceAppDesc smoke_get_desc(void)
{
    JceAppDesc d;
    memset(&d, 0, sizeof d);
    d.name          = "JceSdkSmokeScripting";
    d.init          = smoke_init;
    d.update        = smoke_update;
    d.should_quit   = smoke_should_quit;
    d.window_width  = 320;
    d.window_height = 200;
    return d;
}

JCE_MAIN(smoke_get_desc)

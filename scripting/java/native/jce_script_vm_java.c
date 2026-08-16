/*
 * jce_script_vm_java.c — the Java JceScriptVM: the engine calling UP into a
 * JVM.
 *
 * The contract, the wiring example and every decision that is TRUE OF THE JVM
 * are in scripting/java/include/jce/script_vm/jce_script_vm_java.h.  The ABI
 * rules this file obeys — the clamped registration copy, the frozen handle
 * header, the owning-thread rule — are in
 * engine/include/jce/middleware/script/jce_script_vm.h.  Neither is repeated
 * here; what follows are the notes that belong next to the code.
 *
 * THE SHAPE OF EVERY SLOT, AND WHY IT IS THE SAME SHAPE EIGHTEEN TIMES:
 *
 *     tolerate bad input exactly as the Lua slot does   (NULL / 0 -> no-op)
 *     env = java_env(s, "<name>")     thread rule + JNIEnv, or refuse
 *     PushLocalFrame                  no local reference outlives the call
 *     one Call<T>Method               the Java runtime does the work
 *     java_take_exception             check, CLEAR, report through the host
 *     PopLocalFrame
 *
 * The uniformity is the point.  A slot that skipped the frame would leak
 * per dispatch, and a slot that skipped the check would leave a pending
 * exception that makes the NEXT slot misbehave in a way that looks unrelated —
 * both are invisible in the return value, so both are held by structure rather
 * than by review.  tests/scripting/java/vm/test_local_frames.py reads this
 * file and fails BY NAME for a slot missing either half.
 */

#include <jce/script_vm/jce_script_vm_java.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/platform/jce_library.h>

#include <jni.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv, for the JAVA_HOME fallback */
#include <string.h>

#define LOG_TAG "script.java"

/* Local-reference budget per dispatch.  The widest slot (call_anim_event,
 * call_message) creates one jstring plus, on a throw, the Throwable and its
 * toString — four.  Eight is that with room, and PopLocalFrame frees the frame
 * whatever was actually created inside it. */
#define JAVA_LOCAL_FRAME 8

/* Lua formats its error lines into a 512-byte buffer (jce_script.c); matching
 * it keeps a truncated Java exception truncated at the same place. */
#define JAVA_REPORT_MAX 512

/* Threads this backend attached itself.  A handful: the rule is one owning
 * thread per handle and games do not create hundreds of script VMs. */
#define JAVA_MAX_ATTACHED_THREADS 16

typedef jint(JNICALL *JavaCreateVmFn)(JavaVM **, void **, void *);
typedef jint(JNICALL *JavaGetCreatedVmsFn)(JavaVM **, jsize, jsize *);

/* ── Process-wide state ───────────────────────────────────────────────────
 *
 * A JVM is a process singleton whether we like it or not, so the things that
 * follow it are process-wide too: the library handle, the class and method ids
 * (one class, loaded by the application loader, identical for every handle),
 * and the attach bookkeeping.  Per-HANDLE state lives in JavaScript below and
 * shares nothing with another handle. */
static JceScriptVmJavaConfig g_cfg;
static char                 *g_cfg_jvm_library;
static char                 *g_cfg_class_path;
static char                **g_cfg_options;
static int                   g_cfg_option_count;

static JavaVM   *g_jvm;
static JceLibrary g_jvm_lib;
static int        g_creations;
static int        g_adoptions;
static int        g_live_handles;

static jclass    g_runtime_cls;      /* global ref, never released */
static jmethodID g_m_ctor;
static jmethodID g_m_instantiate;
static jmethodID g_m_instantiate_source;
static jmethodID g_m_call_start;
static jmethodID g_m_call_update;
static jmethodID g_m_release;
static jmethodID g_m_call_collision;
static jmethodID g_m_call_message;
static jmethodID g_m_call_anim_event;
static jmethodID g_m_call_named;
static jmethodID g_m_call_named_num;
static jmethodID g_m_call_named_str;
static jmethodID g_m_instance_count;
static jmethodID g_m_update_coroutines;
static jmethodID g_m_compile_module;
static jmethodID g_m_disable_handler;
static jmethodID g_m_rebind_instance;
static jmethodID g_m_release_module;
static jmethodID g_m_shutdown;
static jmethodID g_m_to_string;      /* java.lang.Object::toString */

static struct {
    uint64_t thread;
    int      handles;
} g_attached[JAVA_MAX_ATTACHED_THREADS];

/* One JceScript handle. */
typedef struct JavaScript {
    JceScriptVMHeader hdr;          /* MUST be first; jce_script_vm.h */
    JavaVM           *jvm;
    jobject           runtime;      /* global ref to JceScriptRuntime */
    JceScriptHost     host;
    bool              have_host;
    uint64_t          owner_thread;
    bool              attached_here;
    bool              warned_thread;
} JavaScript;

/* ── Reporting ────────────────────────────────────────────────────────────
 *
 * Same two destinations and same order as jce_script.c: the host's log (so a
 * game surfaces a script error where it surfaces every other script error) and
 * the engine log.  A NULL host.log is a no-op, never a fallback to something
 * else. */
static void java_report(JavaScript *s, const char *fmt, ...)
{
    char    buf[JAVA_REPORT_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (s && s->have_host && s->host.log)
        s->host.log(s->host.user, buf);
    LOG_ERROR(LOG_TAG, "%s", buf);
}

/* ── Configuration ──────────────────────────────────────────────────────── */

static char *java_dup(const char *src)
{
    size_t n;
    char  *out;
    if (!src) return NULL;
    n = strlen(src) + 1u;
    out = (char *)jce_malloc(n);
    if (out) memcpy(out, src, n);
    return out;
}

static void java_free_config(void)
{
    int i;
    jce_free(g_cfg_jvm_library);
    jce_free(g_cfg_class_path);
    for (i = 0; i < g_cfg_option_count; ++i)
        jce_free(g_cfg_options[i]);
    jce_free(g_cfg_options);
    g_cfg_jvm_library = NULL;
    g_cfg_class_path  = NULL;
    g_cfg_options     = NULL;
    g_cfg_option_count = 0;
    memset(&g_cfg, 0, sizeof(g_cfg));
}

bool JCE_CALL jce_script_vm_java_configure(const JceScriptVmJavaConfig *cfg,
                                           size_t cfg_size)
{
    size_t copy;
    int    i;

    if (g_jvm) {
        /* Refused, not accepted-and-ignored.  A JVM's options are fixed at
         * creation, so a configure() that appeared to succeed here would be a
         * setting that exists, reads back, and does nothing. */
        LOG_ERROR(LOG_TAG,
                  "jce_script_vm_java_configure refused: the JVM is already "
                  "running and its options cannot be changed. Configure "
                  "before the first jce_script_vm_create(\"java\", ...).");
        return false;
    }
    if (!cfg || cfg_size == 0u) {
        LOG_ERROR(LOG_TAG, "jce_script_vm_java_configure refused: NULL config");
        return false;
    }

    java_free_config();

    /* min(caller, ours) over a zeroed destination — the JceScriptHost rule,
     * applied to our own caller-allocated struct for the same reason. */
    copy = cfg_size < sizeof(g_cfg) ? cfg_size : sizeof(g_cfg);
    memcpy(&g_cfg, cfg, copy);

    g_cfg_jvm_library = java_dup(g_cfg.jvm_library);
    g_cfg_class_path  = java_dup(g_cfg.class_path);
    if (g_cfg.options && g_cfg.option_count > 0) {
        g_cfg_options = (char **)jce_malloc(
            sizeof(char *) * (size_t)g_cfg.option_count);
        if (!g_cfg_options) {
            java_free_config();
            LOG_ERROR(LOG_TAG, "jce_script_vm_java_configure: out of memory");
            return false;
        }
        for (i = 0; i < g_cfg.option_count; ++i)
            g_cfg_options[i] = java_dup(g_cfg.options[i]);
        g_cfg_option_count = g_cfg.option_count;
    }
    /* The copies are what everything below reads; the caller's pointers are
     * not kept, so the caller may free them the moment this returns. */
    g_cfg.jvm_library = NULL;
    g_cfg.class_path  = NULL;
    g_cfg.options     = NULL;
    return true;
}

/* Where the JVM library is, as an ABSOLUTE path.  A bare name is refused
 * rather than resolved: jce_library_open() attaches to an already-resident
 * module of the same name on Windows, so "jvm.dll" silently binds to whichever
 * JVM some other component in the process loaded first. */
static bool java_resolve_jvm_library(char *out, size_t cap)
{
    const char *home;

    if (g_cfg_jvm_library && g_cfg_jvm_library[0]) {
        if (!strchr(g_cfg_jvm_library, '/') && !strchr(g_cfg_jvm_library, '\\')) {
            LOG_ERROR(LOG_TAG,
                      "jvm_library '%s' is a bare name. Pass an ABSOLUTE path: "
                      "jce_library_open attaches to an already-resident module "
                      "of the same name, so a bare name binds to whichever JVM "
                      "got loaded first rather than the one you named.",
                      g_cfg_jvm_library);
            return false;
        }
        snprintf(out, cap, "%s", g_cfg_jvm_library);
        LOG_INFO(LOG_TAG, "JVM library: %s (configured)", out);
        return true;
    }

    home = getenv("JAVA_HOME");
    if (!home || !home[0]) {
        LOG_ERROR(LOG_TAG,
                  "no JVM library: config.jvm_library is unset and JAVA_HOME "
                  "is not set. There is no third place to look.");
        return false;
    }
#if defined(_WIN32)
    snprintf(out, cap, "%s/bin/server/jvm.dll", home);
#elif defined(__APPLE__)
    snprintf(out, cap, "%s/lib/server/libjvm.dylib", home);
#else
    snprintf(out, cap, "%s/lib/server/libjvm.so", home);
#endif
    LOG_INFO(LOG_TAG, "JVM library: %s (derived from JAVA_HOME)", out);
    return true;
}

/* ── JVM bootstrap ────────────────────────────────────────────────────────
 *
 * Called from create_sized.  Idempotent: the second handle takes the same JVM.
 * See the header on why destroy() never tears it down. */
static bool java_cache_ids(JNIEnv *env);

static JNIEnv *java_bootstrap(void)
{
    char                path[1024];
    JavaCreateVmFn      create_vm;
    JavaGetCreatedVmsFn get_vms;
    JavaVMInitArgs      args;
    JavaVMOption        opts[16];
    char                cp_opt[2048];
    JNIEnv             *env = NULL;
    jsize               found = 0;
    int                 n = 0;
    int                 i;

    if (g_jvm) {
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_8) == JNI_OK)
            return env;
        return NULL;          /* caller attaches */
    }

    if (!java_resolve_jvm_library(path, sizeof(path)))
        return NULL;

    g_jvm_lib = jce_library_open(path);
    if (!g_jvm_lib) {
        LOG_ERROR(LOG_TAG,
                  "cannot load the JVM library '%s'. On Windows the JDK's bin "
                  "directory must also be reachable by the OS loader, because "
                  "jvm.dll's own dependencies live beside it.", path);
        return NULL;
    }
    create_vm = (JavaCreateVmFn)jce_library_symbol(g_jvm_lib, "JNI_CreateJavaVM");
    get_vms   = (JavaGetCreatedVmsFn)jce_library_symbol(g_jvm_lib,
                                                        "JNI_GetCreatedJavaVMs");
    if (!create_vm || !get_vms) {
        LOG_ERROR(LOG_TAG,
                  "'%s' loaded but does not export JNI_CreateJavaVM / "
                  "JNI_GetCreatedJavaVMs — that is not a JVM.", path);
        jce_library_close(g_jvm_lib);
        g_jvm_lib = NULL;
        return NULL;
    }

    /* ADOPT rather than fight.  JNI_CreateJavaVM may be called once per
     * process; if something already did (this engine loaded INTO a JVM, or a
     * plugin got there first) the only correct answer is to use theirs. */
    if (get_vms(&g_jvm, 1, &found) == JNI_OK && found > 0 && g_jvm) {
        ++g_adoptions;
        LOG_INFO(LOG_TAG, "adopted the JVM already running in this process");
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_8) == JNI_OK)
            return java_cache_ids(env) ? env : NULL;
        return NULL;          /* caller attaches, then caches */
    }
    g_jvm = NULL;

    if (g_cfg_class_path && g_cfg_class_path[0]) {
        snprintf(cp_opt, sizeof(cp_opt), "-Djava.class.path=%s",
                 g_cfg_class_path);
        opts[n].optionString = cp_opt;
        opts[n].extraInfo = NULL;
        ++n;
    }
    for (i = 0; i < g_cfg_option_count && n < (int)(sizeof(opts) / sizeof(opts[0])); ++i) {
        if (!g_cfg_options[i]) continue;
        opts[n].optionString = g_cfg_options[i];
        opts[n].extraInfo = NULL;
        ++n;
    }

    args.version = JNI_VERSION_1_8;
    args.nOptions = n;
    args.options = n > 0 ? opts : NULL;
    /* JNI_FALSE: an option the JVM does not recognise is a loud failure here
     * rather than a setting the game believes it applied. */
    args.ignoreUnrecognized = JNI_FALSE;

    if (create_vm(&g_jvm, (void **)&env, &args) != JNI_OK || !g_jvm) {
        LOG_ERROR(LOG_TAG,
                  "JNI_CreateJavaVM failed (%d option(s), class path %s)",
                  n, g_cfg_class_path ? g_cfg_class_path : "(default)");
        g_jvm = NULL;
        jce_library_close(g_jvm_lib);
        g_jvm_lib = NULL;
        return NULL;
    }
    ++g_creations;
    LOG_SUCCESS(LOG_TAG, "JVM created (%d option(s))", n);
    return java_cache_ids(env) ? env : NULL;
}

/* Everything the slots dispatch through, resolved once.  A missing method here
 * is a mismatch between this file and JceScriptRuntime.java, and it is fatal
 * at create rather than at the first dispatch that needs it. */
static bool java_cache_id(JNIEnv *env, jmethodID *out, const char *name,
                          const char *sig)
{
    *out = (*env)->GetMethodID(env, g_runtime_cls, name, sig);
    if (*out) return true;
    (*env)->ExceptionClear(env);
    LOG_ERROR(LOG_TAG,
              "com.jce.script.vm.JceScriptRuntime has no %s%s — the JNI "
              "backend and the classes on the class path are different "
              "versions.", name, sig);
    return false;
}

static bool java_register_natives(JNIEnv *env);

static bool java_cache_ids(JNIEnv *env)
{
    jclass local;
    jclass object_cls;
    bool   ok = true;

    if (g_runtime_cls) return true;

    local = (*env)->FindClass(env, "com/jce/script/vm/JceScriptRuntime");
    if (!local) {
        (*env)->ExceptionClear(env);
        LOG_ERROR(LOG_TAG,
                  "com.jce.script.vm.JceScriptRuntime is not on the class "
                  "path. Build the scripting classes with "
                  "scripting/java/build_java.py and put them on "
                  "JceScriptVmJavaConfig::class_path.");
        return false;
    }
    g_runtime_cls = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!g_runtime_cls) return false;

    object_cls = (*env)->FindClass(env, "java/lang/Object");
    if (object_cls) {
        g_m_to_string = (*env)->GetMethodID(env, object_cls, "toString",
                                            "()Ljava/lang/String;");
        (*env)->DeleteLocalRef(env, object_cls);
    }
    if (!g_m_to_string) (*env)->ExceptionClear(env);

    ok = ok && java_cache_id(env, &g_m_ctor, "<init>", "(JJJ)V");
    ok = ok && java_cache_id(env, &g_m_instantiate, "instantiate",
                             "(Ljava/lang/String;[BJ)I");
    ok = ok && java_cache_id(env, &g_m_instantiate_source, "instantiateSource",
                             "(Ljava/lang/String;Ljava/lang/String;J)I");
    ok = ok && java_cache_id(env, &g_m_call_start, "callStart", "(I)V");
    ok = ok && java_cache_id(env, &g_m_call_update, "callUpdate", "(IF)V");
    ok = ok && java_cache_id(env, &g_m_release, "release", "(I)V");
    ok = ok && java_cache_id(env, &g_m_call_collision, "callCollision", "(IJ)V");
    ok = ok && java_cache_id(env, &g_m_call_message, "callMessage",
                             "(ILjava/lang/String;DLjava/lang/String;)V");
    ok = ok && java_cache_id(env, &g_m_call_anim_event, "callAnimEvent",
                             "(IILjava/lang/String;FFI)V");
    ok = ok && java_cache_id(env, &g_m_call_named, "callNamed",
                             "(Ljava/lang/String;J)Z");
    ok = ok && java_cache_id(env, &g_m_call_named_num, "callNamedNum",
                             "(Ljava/lang/String;JD)Z");
    ok = ok && java_cache_id(env, &g_m_call_named_str, "callNamedStr",
                             "(Ljava/lang/String;JLjava/lang/String;)Z");
    ok = ok && java_cache_id(env, &g_m_instance_count, "instanceCount", "()I");
    ok = ok && java_cache_id(env, &g_m_update_coroutines, "updateCoroutines",
                             "(F)V");
    ok = ok && java_cache_id(env, &g_m_compile_module, "compileModule",
                             "(Ljava/lang/String;Ljava/lang/String;)I");
    ok = ok && java_cache_id(env, &g_m_disable_handler, "disableHandler",
                             "(ILjava/lang/String;)V");
    ok = ok && java_cache_id(env, &g_m_rebind_instance, "rebindInstance",
                             "(II)V");
    ok = ok && java_cache_id(env, &g_m_release_module, "releaseModule", "(I)V");
    ok = ok && java_cache_id(env, &g_m_shutdown, "shutdown", "()V");
    ok = ok && java_register_natives(env);
    if (!ok) {
        /* Do not leave a half-resolved class behind: the next create would
         * take the `if (g_runtime_cls) return true` fast path above and
         * dispatch through NULL method ids. */
        (*env)->DeleteGlobalRef(env, g_runtime_cls);
        g_runtime_cls = NULL;
    }
    return ok;
}

/* ── Thread attachment ────────────────────────────────────────────────────
 *
 * The rule is the engine's: a handle belongs to the thread that created it.
 * Attachment therefore happens once, at create, for that thread, and detaches
 * at destroy — but only if THIS handle did the attaching and no other handle
 * on that thread is still live.  A thread the JVM already knew is never
 * detached by us: it is not ours to end. */
static int java_attach_slot(uint64_t thread)
{
    int i, free_slot = -1;
    for (i = 0; i < JAVA_MAX_ATTACHED_THREADS; ++i) {
        if (g_attached[i].handles > 0 && g_attached[i].thread == thread)
            return i;
        if (g_attached[i].handles == 0 && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

static JNIEnv *java_attach_current(JavaScript *s)
{
    JNIEnv *env = NULL;
    jint    st;
    int     slot;

    st = (*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_8);
    if (st == JNI_OK) return env;      /* already attached; not ours to detach */

    if ((*g_jvm)->AttachCurrentThread(g_jvm, (void **)&env, NULL) != JNI_OK) {
        LOG_ERROR(LOG_TAG, "AttachCurrentThread failed");
        return NULL;
    }
    slot = java_attach_slot(s->owner_thread);
    if (slot < 0) {
        /* Out of bookkeeping room: stay attached rather than detach a thread
         * we can no longer account for.  Leaking an attachment is recoverable;
         * detaching a thread another handle is using is not. */
        LOG_ERROR(LOG_TAG,
                  "more than %d threads own Java script VMs; thread %llu will "
                  "stay attached for the life of the process",
                  JAVA_MAX_ATTACHED_THREADS,
                  (unsigned long long)s->owner_thread);
        return env;
    }
    g_attached[slot].thread = s->owner_thread;
    g_attached[slot].handles += 1;
    s->attached_here = true;
    return env;
}

static void java_release_thread(JavaScript *s)
{
    int i;
    if (!s->attached_here) return;
    for (i = 0; i < JAVA_MAX_ATTACHED_THREADS; ++i) {
        if (g_attached[i].handles <= 0) continue;
        if (g_attached[i].thread != s->owner_thread) continue;
        g_attached[i].handles -= 1;
        if (g_attached[i].handles == 0)
            (*g_jvm)->DetachCurrentThread(g_jvm);
        return;
    }
}

/* ── The per-dispatch preamble ────────────────────────────────────────────
 *
 * The thread rule, enforced HERE and not only in the engine's forwarders: the
 * forwarder check is disarmed while hdr.owner_thread is 0, which is every call
 * made through the vtable directly.  A JNIEnv is per-thread and a
 * JceScriptRuntime is not synchronised, so the answer to "another thread" is
 * refusal, never AttachCurrentThread. */
static JNIEnv *java_env(JavaScript *s, const char *what)
{
    JNIEnv *env = NULL;

    if (!s || !s->jvm || !s->runtime) return NULL;
    if (jce_thread_current_id() != s->owner_thread) {
        if (!s->warned_thread) {
            s->warned_thread = true;
            LOG_ERROR(LOG_TAG,
                      "%s called from thread %llu but this Java VM is owned by "
                      "thread %llu — refused. Attaching the caller instead "
                      "would hand two threads one unsynchronised runtime.",
                      what, (unsigned long long)jce_thread_current_id(),
                      (unsigned long long)s->owner_thread);
        }
        return NULL;
    }
    if ((*s->jvm)->GetEnv(s->jvm, (void **)&env, JNI_VERSION_1_8) != JNI_OK)
        return NULL;
    return env;
}

/* Check, CLEAR, report.  Returns true when an exception was pending — which
 * the named dispatchers use as "a handler existed and ran", so this is not
 * hygiene: forget it and jce_script_call_named returns the wrong answer.
 *
 * Every reference created here lives in the caller's local frame. */
static bool java_take_exception(JavaScript *s, JNIEnv *env, const char *what)
{
    jthrowable  err;
    jstring     text = NULL;
    const char *utf = NULL;

    if (!(*env)->ExceptionCheck(env)) return false;

    err = (*env)->ExceptionOccurred(env);
    /* Cleared BEFORE anything else is called on this env: with an exception
     * pending, every other JNI function is undefined. */
    (*env)->ExceptionClear(env);

    if (err && g_m_to_string) {
        text = (jstring)(*env)->CallObjectMethod(env, err, g_m_to_string);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);      /* toString threw (OOM): give up
                                               * on the detail, not on the
                                               * report */
            text = NULL;
        }
        if (text) utf = (*env)->GetStringUTFChars(env, text, NULL);
    }
    java_report(s, "%s error: %s", what,
                utf ? utf : "(exception with no readable message)");
    if (utf) (*env)->ReleaseStringUTFChars(env, text, utf);
    return true;
}

/* THE FAILING-CALLBACK RULE (jce_script.h), Java side.
 *
 * Java script code does not catch its own exceptions — the Throwable travels
 * out through JNI and the slot takes it — so the DECISION lives here even
 * though the STATE lives in JceScriptRuntime.Instance.  Three steps, in the
 * order the reference implementation performs them: report the error, mark the
 * hook disabled on that instance, then write the published notice.
 *
 * `hook` must be one of the four fixed names the rule lists; the caller states
 * it, exactly as jce_script.c's dispatchers state their slot rather than
 * deriving one from a method name.  The dispatchers that do NOT participate
 * (release/on_destroy, call_message, and the three named-global slots) keep
 * calling java_take_exception directly, which is what makes their
 * non-participation visible at the call site instead of hidden in a condition
 * here.
 *
 * Returns what java_take_exception returned, so the named dispatchers' use of
 * that value as "a handler existed and ran" is untouched. */
static bool java_handler_threw(JavaScript *s, JNIEnv *env,
                               JceScriptInstance inst, const char *hook)
{
    jstring jhook;
    char    buf[JAVA_REPORT_MAX];

    if (!java_take_exception(s, env, hook)) return false;

    jhook = (*env)->NewStringUTF(env, hook);
    if (jhook) {
        (*env)->CallVoidMethod(env, s->runtime, g_m_disable_handler,
                               (jint)inst, jhook);
        /* disableHandler is a map lookup and an |=, so nothing in it throws
         * except OOM — but a pending exception makes every later JNI call in
         * this frame undefined, and "it cannot throw" is not something this
         * frame is allowed to assume about a method it does not compile. */
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    snprintf(buf, sizeof(buf), JCE_SCRIPT_DISABLED_NOTICE_FMT, hook);
    java_report(s, "%s", buf);
    return true;
}

/* ── The eighteen slots ─────────────────────────────────────────────────── */

static JceScript *java_create_sized(const JceScriptHost *host, size_t host_size)
{
    JavaScript *s;
    JNIEnv     *env;
    jobject     local;

    s = (JavaScript *)jce_malloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    /* The handle's first word is the vtable the forwarders dispatch through.
     * jce_script_vm_create() refuses a handle where this is not the table it
     * called, so forgetting it is a clean failure. */
    s->hdr.vm = jce_script_vm_java();
    s->owner_thread = jce_thread_current_id();

    if (host && host_size > 0) {
        /* min(caller, ours) over a zeroed destination.  JceScriptHost is
         * CALLER-allocated and grows; `*host` at our own sizeof reads past the
         * end of a host built against an older header and files the bytes that
         * followed under a callback we then invoke. */
        const size_t n = host_size < sizeof(s->host) ? host_size
                                                     : sizeof(s->host);
        memcpy(&s->host, host, n);
        s->have_host = true;
    }

    env = java_bootstrap();
    if (!g_jvm) { jce_free(s); return NULL; }
    s->jvm = g_jvm;
    if (!env) {
        env = java_attach_current(s);
        if (!env) { jce_free(s); return NULL; }
        if (!java_cache_ids(env)) { java_release_thread(s); jce_free(s); return NULL; }
    } else if (!java_cache_ids(env)) {
        jce_free(s);
        return NULL;
    }

    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        java_release_thread(s);
        jce_free(s);
        return NULL;
    }
    local = (*env)->NewObject(env, g_runtime_cls, g_m_ctor,
                              (jlong)(intptr_t)s,
                              s->have_host ? (jlong)(intptr_t)&s->host : (jlong)0,
                              (jlong)sizeof(s->host));
    if (java_take_exception(s, env, "JceScriptRuntime") || !local) {
        (*env)->PopLocalFrame(env, NULL);
        java_release_thread(s);
        jce_free(s);
        return NULL;
    }
    s->runtime = (*env)->NewGlobalRef(env, local);
    (*env)->PopLocalFrame(env, NULL);
    if (!s->runtime) {
        java_release_thread(s);
        jce_free(s);
        return NULL;
    }

    ++g_live_handles;
    LOG_SUCCESS(LOG_TAG, "Java script VM created%s",
                s->have_host ? " (host bridged)" : "");
    return (JceScript *)s;
}

static void java_destroy(JceScript *sc)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;

    if (!s) return;
    env = java_env(s, "destroy");
    if (env) {
        if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) == 0) {
            (*env)->CallVoidMethod(env, s->runtime, g_m_shutdown);
            java_take_exception(s, env, "shutdown");
            (*env)->PopLocalFrame(env, NULL);
        } else {
            (*env)->ExceptionClear(env);
        }
        (*env)->DeleteGlobalRef(env, s->runtime);
    }
    s->runtime = NULL;
    java_release_thread(s);
    if (g_live_handles > 0) --g_live_handles;
    /* DestroyJavaVM is deliberately NOT called; see the header. */
    jce_free(s);
}

static JceScriptInstance java_instantiate(JceScript *sc, const char *path,
                                          JceScriptEntity owner)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;
    uint64_t    size = 0;
    void       *buf;
    jstring     jpath;
    jbyteArray  jbytes;
    jint        result = 0;

    if (!s || !path) return 0;
    if (!s->have_host || !s->host.read_file) {
        java_report(s, "no read_file host callback; cannot load '%s'", path);
        return 0;
    }
    env = java_env(s, "instantiate");
    if (!env) return 0;

    buf = s->host.read_file(s->host.user, path, &size);
    if (!buf || size == 0) {
        if (buf) jce_free(buf);
        java_report(s, "cannot read script '%s'", path);
        return 0;
    }

    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        jce_free(buf);
        return 0;
    }
    jpath = (*env)->NewStringUTF(env, path);
    jbytes = (*env)->NewByteArray(env, (jsize)size);
    if (jpath && jbytes) {
        (*env)->SetByteArrayRegion(env, jbytes, 0, (jsize)size,
                                   (const jbyte *)buf);
        result = (*env)->CallIntMethod(env, s->runtime, g_m_instantiate,
                                       jpath, jbytes, (jlong)owner);
        java_take_exception(s, env, "instantiate");
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
    jce_free(buf);
    return (JceScriptInstance)result;
}

static JceScriptInstance java_instantiate_source(JceScript *sc, const char *name,
                                                 const char *source,
                                                 JceScriptEntity owner)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;
    jstring     jname;
    jstring     jsource;
    jint        result = 0;

    if (!s || !source) return 0;
    env = java_env(s, "instantiate_source");
    if (!env) return 0;

    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    jname = (*env)->NewStringUTF(env, name ? name : "chunk");
    jsource = (*env)->NewStringUTF(env, source);
    if (jname && jsource) {
        result = (*env)->CallIntMethod(env, s->runtime, g_m_instantiate_source,
                                       jname, jsource, (jlong)owner);
        java_take_exception(s, env, "instantiate_source");
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
    return (JceScriptInstance)result;
}

static void java_call_start(JceScript *sc, JceScriptInstance inst)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;

    if (!s || inst == 0) return;
    env = java_env(s, "call_start");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallVoidMethod(env, s->runtime, g_m_call_start, (jint)inst);
    java_handler_threw(s, env, inst, "on_start");
    (*env)->PopLocalFrame(env, NULL);
}

static void java_call_update(JceScript *sc, JceScriptInstance inst, float dt)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;

    if (!s || inst == 0) return;
    env = java_env(s, "call_update");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallVoidMethod(env, s->runtime, g_m_call_update, (jint)inst,
                           (jfloat)dt);
    java_handler_threw(s, env, inst, "on_update");
    (*env)->PopLocalFrame(env, NULL);
}

static void java_release(JceScript *sc, JceScriptInstance inst)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;

    if (!s || inst == 0) return;
    env = java_env(s, "release");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallVoidMethod(env, s->runtime, g_m_release, (jint)inst);
    /* java_take_exception and not java_handler_threw: on_destroy does not
     * participate in THE FAILING-CALLBACK RULE — see jce_script.h. */
    java_take_exception(s, env, "on_destroy");
    (*env)->PopLocalFrame(env, NULL);
}

static void java_call_collision(JceScript *sc, JceScriptInstance inst,
                                JceScriptEntity other_entity)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;

    if (!s || inst == 0) return;
    env = java_env(s, "call_collision");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallVoidMethod(env, s->runtime, g_m_call_collision, (jint)inst,
                           (jlong)other_entity);
    java_handler_threw(s, env, inst, "on_collision");
    (*env)->PopLocalFrame(env, NULL);
}

static void java_call_message(JceScript *sc, JceScriptInstance inst,
                              const char *msg_name, double number_arg,
                              const char *str_arg)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;
    jstring     jname;
    jstring     jstr = NULL;

    if (!s || inst == 0 || !msg_name || !msg_name[0]) return;
    env = java_env(s, "call_message");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    jname = (*env)->NewStringUTF(env, msg_name);
    /* NULL stays NULL: Lua pushes nil for a NULL str_arg, and a Java handler
     * must be able to tell that from the empty string. */
    if (str_arg) jstr = (*env)->NewStringUTF(env, str_arg);
    if (jname) {
        (*env)->CallVoidMethod(env, s->runtime, g_m_call_message, (jint)inst,
                               jname, (jdouble)number_arg, jstr);
        java_take_exception(s, env, msg_name);
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
}

static void java_call_anim_event(JceScript *sc, JceScriptInstance inst,
                                 uint32_t id, const char *name,
                                 float f0, float f1, int i0)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;
    jstring     jname = NULL;

    if (!s || inst == 0) return;
    env = java_env(s, "call_anim_event");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    /* NULL *or empty* is absence, which is the exact test jce_script.c makes
     * before pushing nil. */
    if (name && name[0]) jname = (*env)->NewStringUTF(env, name);
    (*env)->CallVoidMethod(env, s->runtime, g_m_call_anim_event, (jint)inst,
                           (jint)id, jname, (jfloat)f0, (jfloat)f1, (jint)i0);
    java_handler_threw(s, env, inst, "on_anim_event");
    (*env)->PopLocalFrame(env, NULL);
}

/* The three named dispatchers return "a handler of that name EXISTED", not
 * "the call succeeded" — jce_script.h states it and Lua implements it by
 * returning true after catching the error.  A Java handler that throws leaves
 * the JNI default (false) in `answer` with an exception pending, so the
 * pending exception IS the answer. */
static bool java_call_named(JceScript *sc, const char *fn_name,
                            JceScriptEntity arg_entity)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;
    jstring     jname;
    jboolean    answer = JNI_FALSE;
    bool        threw = false;

    if (!s || !fn_name || !fn_name[0]) return false;
    env = java_env(s, "call_named");
    if (!env) return false;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return false;
    }
    jname = (*env)->NewStringUTF(env, fn_name);
    if (jname) {
        answer = (*env)->CallBooleanMethod(env, s->runtime, g_m_call_named,
                                           jname, (jlong)arg_entity);
        threw = java_take_exception(s, env, fn_name);
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
    return threw || answer == JNI_TRUE;
}

static bool java_call_named_num(JceScript *sc, const char *fn_name,
                                JceScriptEntity arg_entity, double value)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;
    jstring     jname;
    jboolean    answer = JNI_FALSE;
    bool        threw = false;

    if (!s || !fn_name || !fn_name[0]) return false;
    env = java_env(s, "call_named_num");
    if (!env) return false;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return false;
    }
    jname = (*env)->NewStringUTF(env, fn_name);
    if (jname) {
        answer = (*env)->CallBooleanMethod(env, s->runtime, g_m_call_named_num,
                                           jname, (jlong)arg_entity,
                                           (jdouble)value);
        threw = java_take_exception(s, env, fn_name);
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
    return threw || answer == JNI_TRUE;
}

static bool java_call_named_str(JceScript *sc, const char *fn_name,
                                JceScriptEntity arg_entity, const char *str)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;
    jstring     jname;
    jstring     jstr = NULL;
    jboolean    answer = JNI_FALSE;
    bool        threw = false;

    if (!s || !fn_name || !fn_name[0]) return false;
    env = java_env(s, "call_named_str");
    if (!env) return false;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return false;
    }
    jname = (*env)->NewStringUTF(env, fn_name);
    if (str) jstr = (*env)->NewStringUTF(env, str);
    if (jname) {
        answer = (*env)->CallBooleanMethod(env, s->runtime, g_m_call_named_str,
                                           jname, (jlong)arg_entity, jstr);
        threw = java_take_exception(s, env, fn_name);
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
    return threw || answer == JNI_TRUE;
}

static int java_instance_count(const JceScript *sc)
{
    /* const in, engine-owned scratch written on the way: the same cast
     * jce_script_vm.c makes for exactly the same reason. */
    union { const JceScript *in; JavaScript *out; } u;
    JavaScript *s;
    JNIEnv     *env;
    jint        n = 0;

    u.in = sc;
    s = u.out;
    if (!s) return 0;
    env = java_env(s, "instance_count");
    if (!env) return 0;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    n = (*env)->CallIntMethod(env, s->runtime, g_m_instance_count);
    java_take_exception(s, env, "instance_count");
    (*env)->PopLocalFrame(env, NULL);
    return (int)n;
}

static void java_update_coroutines(JceScript *sc, float dt)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;

    if (!s) return;
    env = java_env(s, "update_coroutines");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallVoidMethod(env, s->runtime, g_m_update_coroutines, (jfloat)dt);
    java_take_exception(s, env, "coroutine");
    (*env)->PopLocalFrame(env, NULL);
}

static JceScriptModule java_compile_module(JceScript *sc, const char *name,
                                           const char *source, size_t len)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;
    jstring     jname;
    jstring     jsource;
    char       *bounded;
    jint        result = 0;

    if (!s || !source) return 0;
    env = java_env(s, "compile_module");
    if (!env) return 0;

    /* NewStringUTF wants NUL termination and `len` is the authority — a source
     * buffer may be a slice of a larger file, and compiling past `len` would
     * compile bytes the caller did not offer. */
    bounded = (char *)jce_malloc(len + 1u);
    if (!bounded) return 0;
    memcpy(bounded, source, len);
    bounded[len] = '\0';

    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        jce_free(bounded);
        return 0;
    }
    jname = (*env)->NewStringUTF(env, name ? name : "reload");
    jsource = (*env)->NewStringUTF(env, bounded);
    if (jname && jsource) {
        result = (*env)->CallIntMethod(env, s->runtime, g_m_compile_module,
                                       jname, jsource);
        java_take_exception(s, env, "reload");
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
    jce_free(bounded);
    return (JceScriptModule)result;
}

static void java_rebind_instance(JceScript *sc, JceScriptInstance inst,
                                 JceScriptModule mod)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;

    if (!s || inst == 0 || mod == 0) return;
    env = java_env(s, "rebind_instance");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallVoidMethod(env, s->runtime, g_m_rebind_instance, (jint)inst,
                           (jint)mod);
    java_take_exception(s, env, "rebind");
    (*env)->PopLocalFrame(env, NULL);
}

static void java_release_module(JceScript *sc, JceScriptModule mod)
{
    JavaScript *s = (JavaScript *)sc;
    JNIEnv     *env;

    if (!s || mod == 0) return;
    env = java_env(s, "release_module");
    if (!env) return;
    if ((*env)->PushLocalFrame(env, JAVA_LOCAL_FRAME) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallVoidMethod(env, s->runtime, g_m_release_module, (jint)mod);
    java_take_exception(s, env, "release_module");
    (*env)->PopLocalFrame(env, NULL);
}

/* ── The natives JceScriptRuntime declares ────────────────────────────────
 *
 * Registered with RegisterNatives rather than exported by name: this backend
 * is a STATIC library linked into the game, not a shared object the JVM can
 * System.loadLibrary().  There is no DLL to look symbols up in, and
 * RegisterNatives is the JNI answer for exactly that shape. */
static JavaScript *java_from_handle(jlong handle)
{
    return (JavaScript *)(intptr_t)handle;
}

static void JNICALL java_native_log(JNIEnv *env, jclass cls, jlong handle,
                                    jstring message)
{
    JavaScript *s = java_from_handle(handle);
    const char *utf;

    (void)cls;
    if (!s || !s->have_host || !s->host.log || !message) return;
    utf = (*env)->GetStringUTFChars(env, message, NULL);
    if (!utf) return;
    s->host.log(s->host.user, utf);
    (*env)->ReleaseStringUTFChars(env, message, utf);
}

static jboolean JNICALL java_native_get_position(JNIEnv *env, jclass cls,
                                                 jlong handle, jlong entity,
                                                 jfloatArray out)
{
    JavaScript *s = java_from_handle(handle);
    float       xyz[3];

    (void)cls;
    if (!s || !s->have_host || !s->host.get_position || !out) return JNI_FALSE;
    if ((*env)->GetArrayLength(env, out) < 3) return JNI_FALSE;
    if (!s->host.get_position(s->host.user, (JceScriptEntity)entity, xyz))
        return JNI_FALSE;
    (*env)->SetFloatArrayRegion(env, out, 0, 3, xyz);
    return JNI_TRUE;
}

static void JNICALL java_native_set_position(JNIEnv *env, jclass cls,
                                             jlong handle, jlong entity,
                                             jfloat x, jfloat y, jfloat z)
{
    JavaScript *s = java_from_handle(handle);

    (void)env;
    (void)cls;
    if (!s || !s->have_host || !s->host.set_position) return;
    s->host.set_position(s->host.user, (JceScriptEntity)entity,
                         (float)x, (float)y, (float)z);
}

static bool java_register_natives(JNIEnv *env)
{
    JNINativeMethod natives[3];

    natives[0].name = (char *)"nativeLog";
    natives[0].signature = (char *)"(JLjava/lang/String;)V";
    natives[0].fnPtr = (void *)java_native_log;
    natives[1].name = (char *)"nativeGetPosition";
    natives[1].signature = (char *)"(JJ[F)Z";
    natives[1].fnPtr = (void *)java_native_get_position;
    natives[2].name = (char *)"nativeSetPosition";
    natives[2].signature = (char *)"(JJFFF)V";
    natives[2].fnPtr = (void *)java_native_set_position;

    if ((*env)->RegisterNatives(env, g_runtime_cls, natives, 3) == JNI_OK)
        return true;
    (*env)->ExceptionClear(env);
    LOG_ERROR(LOG_TAG, "RegisterNatives failed for JceScriptRuntime");
    return false;
}

/* ── The table ────────────────────────────────────────────────────────────
 *
 * POSITIONAL initialisers, as jce_script_vm.h requires: designated ones would
 * survive a reorder of JceScriptVM, and this table plus the engine's signature
 * pin are what turn a reorder or a retype into a compile error at two sites.
 * jce_script_vm.h makes C4113/C4133 fatal for every translation unit that
 * includes it, which is why a mistyped slot here stops the build instead of
 * warning. */
static const JceScriptVM k_java_vm = {
    sizeof(JceScriptVM),
    "java",
    java_create_sized,
    java_destroy,
    java_instantiate,
    java_instantiate_source,
    java_call_start,
    java_call_update,
    java_release,
    java_call_collision,
    java_call_message,
    java_call_anim_event,
    java_call_named,
    java_call_named_num,
    java_call_named_str,
    java_instance_count,
    java_update_coroutines,
    java_compile_module,
    java_rebind_instance,
    java_release_module,
};

const JceScriptVM *JCE_CALL jce_script_vm_java(void)
{
    return &k_java_vm;
}

bool JCE_CALL jce_script_vm_java_register(void)
{
    if (!jce_script_vm_register(&k_java_vm)) return false;
    /* Claim the extensions, or nothing authored in Java is ever SELECTED: the
     * runtime resolves each Script component's path to a language through
     * jce_script_vm_language_for_path(), and that answer comes from claims
     * like these.  BOTH extensions, because instantiate dispatches on the
     * class-file magic and accepts either — .java source (compiled in memory,
     * needs a JDK) or a pre-compiled .class (loads on a plain JRE).  Claiming
     * only .java would make every pre-compiled deployment unreachable, which
     * is the configuration a shipped game is most likely to use.
     * A claim naming an unregistered language is refused, so these must
     * follow the registration above. */
    if (!jce_script_vm_register_extension("java", "java"))  return false;
    return jce_script_vm_register_extension("class", "java");
}

int JCE_CALL jce_script_vm_java_jvm_creations(void) { return g_creations; }
int JCE_CALL jce_script_vm_java_jvm_adoptions(void) { return g_adoptions; }
int JCE_CALL jce_script_vm_java_live_handles(void) { return g_live_handles; }

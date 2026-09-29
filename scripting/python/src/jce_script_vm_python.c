/*
 * jce_script_vm_python.c — CPython as a JceScriptVM.  The engine calls UP.
 *
 * Read engine/include/jce/middleware/script/jce_script_vm.h first: the ABI
 * rules, the two silently-failing slots, the frozen handle header and the
 * owning-thread rule are all stated there and none of them is restated here.
 * Read scripting/python/jce_script/vm.py second: it owns the instance model,
 * the error policy and the coroutine scheduler, and its docstring is where the
 * split between these two files is argued.
 *
 * WHAT THIS FILE OWNS, AND WHAT IT DELIBERATELY DOES NOT
 * ------------------------------------------------------
 * Owns: interpreter lifetime, the GIL, marshalling for the 18 slots, and the
 * routing of a script's errors into `host.log` / the engine log.
 *
 * Does NOT own: any part of the scripting surface.  There is no `jce.` binding
 * in this file and there must never be one.  The `jce` a Python script sees is
 * `jce_script.open_host(&s->host, sizeof s->host)` — the SAME ctypes binding a
 * cross-language differential already accepted against Lua, opened over the
 * SAME JceScriptHost the engine handed to create_sized.  A `jce` written here
 * in C would be a second definition of a 71-entry surface whose first
 * definition is generated from a manifest, and two definitions drift.
 * *Enforced by:* test_emit_python.py :: test_the_c_shim_owns_no_scripting_surface,
 * which fails naming any manifest entry that appears in this file.
 *
 * INTERPRETER LIFETIME: INITIALISE ONCE, NEVER FINALISE
 * -----------------------------------------------------
 * A second JceScript handle does NOT get a second interpreter: `g_live`
 * counts handles, `Py_InitializeFromConfig` runs for the first one only, and
 * each handle's state is one `ScriptVM` object — separate namespaces, separate
 * instances, separate coroutine scheduler.  Isolation is per-handle without
 * sub-interpreters, whose PyGILState_* interaction is documented as unsupported.
 *
 * Nothing here ever calls Py_FinalizeEx, and that is a decision with a
 * measurement behind it and a cost stated:
 *
 *   - MEASURED, on this toolchain (CPython 3.12.10, MSVC 14.43): a BARE
 *     interpreter survives two full Py_Initialize / Py_FinalizeEx cycles in
 *     one process.  So the cycle is not categorically broken, and "it would
 *     crash" would have been a claim I could not support.
 *   - What that measurement does NOT cover is the case that matters: CPython
 *     documents that an extension module's initialisation routine may not
 *     survive being called twice, and the VM cannot know what a game's scripts
 *     import.  A script VM is created and destroyed on EVERY scene load, so
 *     the second cycle is the normal path, not an edge case.
 *   - Finalising would also invalidate every PyObject an unrelated embedder in
 *     the same process holds.  We may not be the only tenant; `g_owns_interp`
 *     records whether we were even the one who started it.
 *
 * The cost is one interpreter's memory held until process exit.  It is BOUNDED
 * — one, not one per handle: everything a handle allocates is dropped in
 * destroy, so create/destroy in a loop does not grow.
 * *Enforced by:* test_jce_script_vm_python.c ::
 * test_a_second_vm_after_the_first_was_destroyed_still_runs, which is the
 * shape that a Py_Finalize on last-destroy would turn into a shutdown crash
 * that no lifecycle test would notice.
 *
 * THE GIL, AND HOW A TEST SEES IT
 * --------------------------------
 * Every slot runs on the engine's thread, which is not a Python thread and
 * does not hold the GIL between dispatches: `Py_InitializeFromConfig` leaves
 * the initialising thread holding it, and this file gives it up immediately
 * with PyEval_SaveThread so that PyGILState_Ensure is the ONE way in.  Each
 * slot wraps its work in PyGILState_Ensure / PyGILState_Release.
 *
 * A slot that forgot that pair would call into CPython without the GIL, which
 * is a crash or a hang — and "a crash reports less than a red test" is a
 * hazard this repository measured four times in a single day.  So the failure
 * is converted into a number: `py_call` is the single door every slot's Python
 * work goes through, and it REFUSES to proceed when PyGILState_Check() says
 * the GIL is not held, counting the refusal in `body_unheld`.  Balance is
 * counted too.  Both are exported through jce_script_vm_python_gil_stats.
 * *Enforced by:* test_jce_script_vm_python.c :: test_every_slot_balances_the_gil
 * and test_no_slot_body_ran_without_the_gil.
 */

/* PY_SSIZE_T_CLEAN is MANDATORY since CPython 3.10 for any `#` format unit,
 * and py_compile_module uses "s#" because its contract is a pointer AND a
 * length.  Without it the build fails with SystemError at the format, not with
 * a wrong length — but only in the one translation unit that uses `#`, which
 * is why it is stated here rather than assumed from the include order. */
#define PY_SSIZE_T_CLEAN

/* Python.h must precede every other header (CPython requirement), and on MSVC
 * it must not be included with _DEBUG defined: pyconfig.h's
 * `pragma comment(lib, "python3xx_d.lib")` is guarded on it, and a normal
 * CPython install ships no debug import library — a Debug engine build would
 * fail to LINK for a reason that names a file nobody asked for.  Undefining it
 * around the include is the documented workaround and selects the release
 * CPython in every configuration. */
#if defined(_MSC_VER) && defined(_DEBUG)
#  define JCE_PY_VM_RESTORE_DEBUG
#  undef _DEBUG
#  include <Python.h>
#  define _DEBUG
#else
#  include <Python.h>
#endif

#include "jce_script_vm_python.h"

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "script"

/* JCE_PY_VM_PROTOCOL, JCE_PY_VM_MAX_PATHS, JCE_PY_VM_PATH_MAX and
 * JCE_PY_VM_MSG_MAX all live in the header: the protocol number is part of
 * this backend's published contract with jce_script/vm.py and a test reads it
 * from there, and the sizes are what an embedder needs to know before calling
 * add_path. */

/* The CPython this was compiled against, injected by CMake from
 * Python3_VERSION.  Reported at create because "which Python" is the first
 * question every embedding bug report needs answered, and Py_GetVersion() is
 * the RUNTIME one — the two differing IS the bug in the common case. */
#ifndef JCE_PY_VM_PYTHON_VERSION
#  define JCE_PY_VM_PYTHON_VERSION "unknown"
#endif

/* ── The handle ──────────────────────────────────────────────────────────
 *
 * JceScriptVMHeader FIRST.  jce_script_vm_create() refuses a handle whose
 * first word is not the table it dispatched through, because every forwarder
 * reads the vtable from offset 0. */
typedef struct JcePyScript {
    JceScriptVMHeader hdr;
    JceScriptHost     host;
    bool              have_host;
    PyObject         *vm;        /* jce_script.vm.ScriptVM, one per handle */
} JcePyScript;

/* ── Process-wide state ──────────────────────────────────────────────────
 *
 * Not guarded by a mutex, and that is the same rule the registry states: a
 * JceScript handle belongs to the thread that created it, and creating
 * handles is a startup-time operation. */
static int      g_live;
static bool     g_py_ready;
static bool     g_owns_interp;
static char     g_paths[JCE_PY_VM_MAX_PATHS][JCE_PY_VM_PATH_MAX];
static int      g_path_count;
static int      g_paths_applied;
static JceScriptVmPythonGilStats g_gil;

void jce_script_vm_python_gil_stats(JceScriptVmPythonGilStats *out)
{
    if (out) *out = g_gil;
}

int  jce_script_vm_python_live_handles(void)     { return g_live; }
bool jce_script_vm_python_owns_interpreter(void) { return g_owns_interp; }

/* ── The GIL ─────────────────────────────────────────────────────────────
 *
 * `was` is recorded rather than assumed zero: when an EMBEDDER initialised
 * CPython and calls us from a thread that already holds the GIL, Release
 * drops our nesting level and not theirs, so "held afterwards" is correct
 * there and a defect here.  Comparing against the value before Ensure is true
 * in both processes. */
static void py_enter(PyGILState_STATE *g, int *was)
{
    *was = PyGILState_Check();
    ++g_gil.enters;
    *g = PyGILState_Ensure();
}

static void py_leave(PyGILState_STATE g, int was)
{
    ++g_gil.leaves;
    /* PyGILState_Release WITHOUT the GIL is a Py_FatalError — the process
     * aborts inside CPython, printing nothing about the named failures the
     * run had already produced.  "A crash reports less than a red test" is a
     * hazard this worktree measured four times in one day, so the only way to
     * get here without the GIL — a missing Ensure — is counted and returned
     * from instead of executed.  In correct code this branch is dead, which is
     * why it costs one predictable comparison per dispatch and is worth it. */
    if (!PyGILState_Check()) { ++g_gil.still_held; return; }
    PyGILState_Release(g);
    if (PyGILState_Check() != was) ++g_gil.still_held;
}

/* ── Error formatting ────────────────────────────────────────────────────
 *
 * One line, because host.log takes one line in Lua and the lifecycle
 * differential compares those lines.  The full traceback is printed to stderr
 * by vm.py, which is where a developer looks. */
static void py_format_error(char *buf, size_t cap)
{
    PyObject   *type = NULL, *value = NULL, *tb = NULL, *str = NULL;
    const char *msg = "?";
    const char *tname = "?";

    if (cap == 0u) return;
    buf[0] = '\0';
    if (!PyErr_Occurred()) return;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    if (type) {
        PyObject *n = PyObject_GetAttrString(type, "__name__");
        if (n) {
            const char *s = PyUnicode_AsUTF8(n);
            if (s) tname = s;
            if (value) {
                str = PyObject_Str(value);
                if (str) {
                    const char *m = PyUnicode_AsUTF8(str);
                    if (m) msg = m;
                }
            }
            snprintf(buf, cap, "%s: %s", tname, msg);
            Py_DECREF(n);
        }
    }
    if (buf[0] == '\0') snprintf(buf, cap, "%s: %s", tname, msg);
    Py_XDECREF(str);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    PyErr_Clear();
}

/* Emit one line the way jce_script.c does: host.log when the host is meant to
 * see it, and the engine log always. */
static void py_emit(JcePyScript *s, bool host_visible, const char *text)
{
    if (host_visible && s && s->have_host && s->host.log)
        s->host.log(s->host.user, text);
    LOG_ERROR(LOG_TAG, "%s", text);
}

/* A failure in OUR marshalling, not in the script.  Reported host-visibly on
 * purpose: it is a defect in this file or a broken jce_script package, and
 * both are worse than a script error, so hiding it in the engine log would be
 * the wrong asymmetry. */
static void py_report_internal(JcePyScript *s, const char *what)
{
    char detail[JCE_PY_VM_MSG_MAX];
    char line[JCE_PY_VM_MSG_MAX];
    py_format_error(detail, sizeof detail);
    snprintf(line, sizeof line, "python VM internal error in %s: %s",
             what ? what : "?", detail[0] ? detail : "(no exception set)");
    py_emit(s, true, line);
}

/* ── The one door into Python ────────────────────────────────────────────
 *
 * THE GIL GUARD IS HERE AND NOWHERE ELSE.  Putting it in every slot would
 * make it 18 things to get right; putting it here makes a slot that skipped
 * PyGILState_Ensure a counted refusal and a clean no-op instead of an
 * undebuggable crash inside CPython.
 *
 * `fmt` is a Py_BuildValue format and MUST be parenthesised, so the result is
 * always a tuple — "K" alone builds a bare int, and passing that to
 * PyObject_Call would be a TypeError at every dispatch. */
static PyObject *py_call(JcePyScript *s, const char *meth,
                         const char *fmt, ...)
{
    PyObject *fn, *args, *res;
    va_list   ap;

    if (!PyGILState_Check()) {
        ++g_gil.body_unheld;
        return NULL;
    }
    if (!s || !s->vm) return NULL;

    fn = PyObject_GetAttrString(s->vm, meth);
    if (!fn) { py_report_internal(s, meth); return NULL; }

    va_start(ap, fmt);
    args = Py_VaBuildValue(fmt, ap);
    va_end(ap);
    if (!args || !PyTuple_Check(args)) {
        Py_XDECREF(args);
        Py_DECREF(fn);
        py_report_internal(s, meth);
        return NULL;
    }

    res = PyObject_Call(fn, args, NULL);
    Py_DECREF(args);
    Py_DECREF(fn);
    if (!res) py_report_internal(s, meth);
    return res;
}

/* Everything vm.py wants logged since the last dispatch, in order.
 *
 * Called after EVERY slot.  Doing the writes here rather than in Python is
 * what makes "which lines the host sees" one decision for both languages: the
 * visibility flag is vm.py's, the two calls are jce_script.c's. */
static void py_drain(JcePyScript *s)
{
    PyObject  *seq;
    Py_ssize_t i, n;

    seq = py_call(s, "drain", "()");
    if (!seq) return;
    if (!PyTuple_Check(seq)) { Py_DECREF(seq); return; }
    n = PyTuple_GET_SIZE(seq);
    for (i = 0; i < n; ++i) {
        PyObject   *pair = PyTuple_GET_ITEM(seq, i);   /* borrowed */
        PyObject   *vis, *txt;
        const char *text;
        if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) continue;
        vis = PyTuple_GET_ITEM(pair, 0);
        txt = PyTuple_GET_ITEM(pair, 1);
        text = PyUnicode_Check(txt) ? PyUnicode_AsUTF8(txt) : NULL;
        if (!text) continue;
        py_emit(s, PyObject_IsTrue(vis) == 1, text);
    }
    Py_DECREF(seq);
}

/* DECREF a slot's return value and flush the log.  Every slot ends here, so
 * "the errors were reported" is not something 18 functions each remember. */
static void py_finish(JcePyScript *s, PyObject *res)
{
    Py_XDECREF(res);
    py_drain(s);
}

/* ── sys.path ────────────────────────────────────────────────────────────  */

bool jce_script_vm_python_add_path(const char *dir)
{
    size_t len;
    if (!dir || !dir[0]) return false;
    len = strlen(dir);
    if (len >= (size_t)JCE_PY_VM_PATH_MAX) {
        LOG_ERROR(LOG_TAG, "python VM: sys.path entry is longer than %d bytes",
                  JCE_PY_VM_PATH_MAX - 1);
        return false;
    }
    if (g_path_count >= JCE_PY_VM_MAX_PATHS) {
        LOG_ERROR(LOG_TAG, "python VM: no room for another sys.path entry "
                           "(%d already)", g_path_count);
        return false;
    }
    memcpy(g_paths[g_path_count], dir, len + 1u);
    ++g_path_count;
    return true;
}

/* Applied under the GIL.  PySys_GetObject/PyList_Insert rather than running
 * `import sys; sys.path.insert(...)` as source: a directory is arbitrary text
 * and pasting it into a source string is a quoting bug waiting for the first
 * Windows path with a backslash in it. */
static void py_apply_paths(void)
{
    PyObject *path = PySys_GetObject("path");    /* borrowed */
    int i;
    if (!path || !PyList_Check(path)) return;
    for (i = g_paths_applied; i < g_path_count; ++i) {
        PyObject *entry = PyUnicode_FromString(g_paths[i]);
        if (!entry) { PyErr_Clear(); continue; }
        if (PyList_Insert(path, 0, entry) != 0) PyErr_Clear();
        Py_DECREF(entry);
    }
    g_paths_applied = g_path_count;
}

/* ── Interpreter lifetime ────────────────────────────────────────────────  */

static bool py_runtime_init(void)
{
    PyStatus st;
    PyConfig cfg;

    if (g_py_ready) return true;

    if (Py_IsInitialized()) {
        /* Somebody else's interpreter.  We are a tenant, not the landlord:
         * no configuration, no finalisation, and the GIL is whatever their
         * thread state says — PyGILState_Ensure copes with both. */
        g_owns_interp = false;
        g_py_ready = true;
        return true;
    }

    PyConfig_InitPythonConfig(&cfg);
    /* An engine owns SIGINT.  CPython installs its own handler by default,
     * which would make Ctrl-C raise KeyboardInterrupt inside whatever script
     * happened to be running instead of reaching the engine's shutdown path. */
    cfg.install_signal_handlers = 0;
    /* Py_InitializeFromConfig rather than Py_Initialize because it RETURNS a
     * status: Py_Initialize calls Py_FatalError and aborts the process when
     * it cannot find the standard library, and a misconfigured PYTHONHOME
     * must not take the game down with no log line. */
    st = Py_InitializeFromConfig(&cfg);
    PyConfig_Clear(&cfg);
    if (PyStatus_Exception(st)) {
        LOG_ERROR(LOG_TAG,
                  "python VM: interpreter initialisation failed (%s: %s). "
                  "Check PYTHONHOME/PYTHONPATH for the embedded CPython.",
                  st.func ? st.func : "?", st.err_msg ? st.err_msg : "?");
        return false;
    }
    g_owns_interp = true;
    g_py_ready = true;

    /* Py_InitializeFromConfig leaves THIS thread holding the GIL.  Give it up
     * at once so PyGILState_Ensure is the only way in: keeping it would make
     * every later Ensure a nested no-op whose Release never actually frees the
     * GIL, and no other thread in the process could ever run Python. */
    (void)PyEval_SaveThread();
    return true;
}

/* ── create / destroy ────────────────────────────────────────────────────  */

/* Build the ScriptVM object for this handle.  Returns false with the reason
 * already logged. */
static bool py_open_vm(JcePyScript *s)
{
    PyObject *mod = NULL, *proto = NULL, *cls = NULL, *vm = NULL;
    long      have;
    char      detail[JCE_PY_VM_MSG_MAX];

    /* THE ONE PLACE THAT TOUCHES PYTHON WITHOUT GOING THROUGH py_call, so it
     * needs py_call's guard of its own.  Without it, a missing
     * PyGILState_Ensure would not be a counted refusal here — it would be an
     * access violation in PyImport_ImportModule, i.e. the whole mutation
     * battery reporting nothing at all. */
    if (!PyGILState_Check()) {
        ++g_gil.body_unheld;
        LOG_ERROR(LOG_TAG,
                  "python VM: create reached CPython without the GIL — "
                  "refused. jce_script_vm_python_gil_stats().body_unheld "
                  "names this.");
        return false;
    }

    py_apply_paths();

    mod = PyImport_ImportModule("jce_script.vm");
    if (!mod) {
        py_format_error(detail, sizeof detail);
        LOG_ERROR(LOG_TAG,
                  "python VM: cannot import jce_script.vm (%s). Install the "
                  "package (pip install jce-script), put scripting/python on "
                  "PYTHONPATH, or call jce_script_vm_python_add_path().",
                  detail);
        return false;
    }

    proto = PyObject_GetAttrString(mod, "PROTOCOL");
    have = proto ? PyLong_AsLong(proto) : -1;
    Py_XDECREF(proto);
    if (have != (long)JCE_PY_VM_PROTOCOL) {
        PyErr_Clear();
        LOG_ERROR(LOG_TAG,
                  "python VM: jce_script.vm speaks protocol %ld, this engine "
                  "speaks %d. The package and the engine are shipped "
                  "separately; upgrade whichever is behind.",
                  have, JCE_PY_VM_PROTOCOL);
        Py_DECREF(mod);
        return false;
    }

    cls = PyObject_GetAttrString(mod, "ScriptVM");
    Py_DECREF(mod);
    if (!cls) {
        py_format_error(detail, sizeof detail);
        LOG_ERROR(LOG_TAG, "python VM: jce_script.vm has no ScriptVM (%s)",
                  detail);
        return false;
    }

    /* The host pointer is `&s->host` — OUR heap copy, clamped at create — and
     * never the caller's, which may be a stack temporary the caller reuses.
     * The size is sizeof OURS for the same reason jce_script_create passes
     * sizeof(JceScriptHost): it is the size of the object being handed over,
     * and a summed size is the bug the clamp exists to prevent. */
    vm = PyObject_CallMethod(cls, "open", "(KKKK)",
                             (unsigned long long)(uintptr_t)
                                 (s->have_host ? &s->host : NULL),
                             (unsigned long long)(s->have_host
                                 ? sizeof s->host : 0u),
                             (unsigned long long)(uintptr_t)
                                 (s->have_host ? (void *)s->host.log : NULL),
                             (unsigned long long)(uintptr_t)
                                 (s->have_host ? s->host.user : NULL));
    Py_DECREF(cls);
    if (!vm) {
        py_format_error(detail, sizeof detail);
        LOG_ERROR(LOG_TAG,
                  "python VM: ScriptVM.open failed (%s). The scripting "
                  "surface is jce_script_api; set $JCE_SCRIPT_API to the "
                  "built shared library if it is not beside the executable.",
                  detail);
        return false;
    }
    s->vm = vm;
    return true;
}

static JceScript *py_create_sized(const JceScriptHost *host, size_t host_size)
{
    JcePyScript     *s;
    PyGILState_STATE gil;
    int              was;
    bool             ok;

    s = (JcePyScript *)jce_malloc(sizeof *s);
    if (!s) return NULL;
    memset(s, 0, sizeof *s);

    /* MUST point at OUR table, before returning.  jce_script_vm_create()
     * verifies it and then repoints it at the registry's clamped copy. */
    s->hdr.vm = jce_script_vm_python();

    if (host && host_size > 0u) {
        /* min(caller, ours) over a ZEROED destination.  JceScriptHost is
         * caller-allocated and grows; `*host` at our own sizeof over-reads a
         * consumer built against an older header and then calls whatever
         * followed it. */
        const size_t n = host_size < sizeof s->host ? host_size
                                                    : sizeof s->host;
        memcpy(&s->host, host, n);
        s->have_host = true;
    }

    if (!py_runtime_init()) {
        jce_free(s);
        return NULL;
    }

    py_enter(&gil, &was);
    ok = py_open_vm(s);
    py_leave(gil, was);

    if (!ok) {
        /* REFUSED, not degraded.  A VM whose `jce` is missing runs every
         * handler happily and reaches the engine with nothing, which is
         * indistinguishable from a script that does nothing — the silent
         * failure this layer exists to refuse. */
        jce_free(s);
        return NULL;
    }

    ++g_live;
    LOG_SUCCESS(LOG_TAG,
                "Python VM created%s (built against CPython %s, %d live, "
                "interpreter %s)",
                s->have_host ? " (host bridged)" : "",
                JCE_PY_VM_PYTHON_VERSION, g_live,
                g_owns_interp ? "ours" : "the host process's");
    return (JceScript *)s;
}

static void py_destroy(JceScript *sc)
{
    JcePyScript     *s = (JcePyScript *)sc;
    PyGILState_STATE gil;
    int              was;

    if (!s) return;
    if (s->vm) {
        py_enter(&gil, &was);
        py_finish(s, py_call(s, "close", "()"));
        Py_CLEAR(s->vm);
        py_leave(gil, was);
    }
    if (g_live > 0) --g_live;
    /* No Py_FinalizeEx here even at g_live == 0.  See INTERPRETER LIFETIME. */
    jce_free(s);
}

/* ── Slot scaffolding ────────────────────────────────────────────────────  */

#define PY_SLOT_VOID(sc)                                    \
    JcePyScript     *s = (JcePyScript *)(sc);               \
    PyGILState_STATE _gil;                                  \
    int              _was;                                  \
    if (!s || !s->vm) return;                               \
    py_enter(&_gil, &_was)

#define PY_SLOT_RET(sc, absent)                             \
    JcePyScript     *s = (JcePyScript *)(sc);               \
    PyGILState_STATE _gil;                                  \
    int              _was;                                  \
    if (!s || !s->vm) return (absent);                      \
    py_enter(&_gil, &_was)

#define PY_SLOT_END() py_leave(_gil, _was)

/* ── Instantiation ───────────────────────────────────────────────────────  */

static JceScriptInstance py_instantiate_source(JceScript *sc, const char *name,
                                               const char *source,
                                               JceScriptEntity owner)
{
    PyObject         *res;
    JceScriptInstance inst = 0u;
    PY_SLOT_RET(sc, 0u);

    if (source) {
        res = py_call(s, "instantiate_source", "(zsK)",
                      name, source, (unsigned long long)owner);
        if (res) inst = (JceScriptInstance)PyLong_AsUnsignedLongLongMask(res);
        py_finish(s, res);
    }
    PY_SLOT_END();
    return inst;
}

/* `path` goes through the host's read_file, exactly as script_lua_instantiate
 * does, so the two languages load a script through the same door and a host
 * with no read_file refuses both identically. */
static JceScriptInstance py_instantiate(JceScript *sc, const char *path,
                                        JceScriptEntity owner)
{
    JcePyScript      *s = (JcePyScript *)sc;
    void             *buf;
    uint64_t          size = 0u;
    char             *src;
    JceScriptInstance inst;

    if (!s || !s->vm || !path) return 0u;
    if (!s->have_host || !s->host.read_file) {
        LOG_ERROR(LOG_TAG, "no read_file host callback; cannot load '%s'",
                  path);
        return 0u;
    }
    buf = s->host.read_file(s->host.user, path, &size);
    if (!buf || size == 0u) {
        if (buf) jce_free(buf);
        LOG_ERROR(LOG_TAG, "cannot read script '%s'", path);
        return 0u;
    }
    /* read_file need not NUL-terminate; compile() takes a C string. */
    src = (char *)jce_malloc((size_t)size + 1u);
    if (!src) { jce_free(buf); return 0u; }
    memcpy(src, buf, (size_t)size);
    src[(size_t)size] = '\0';
    jce_free(buf);

    inst = py_instantiate_source(sc, path, src, owner);
    jce_free(src);
    return inst;
}

static void py_release(JceScript *sc, JceScriptInstance inst)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "release", "(K)", (unsigned long long)inst));
    PY_SLOT_END();
}

/* ── Dispatch ────────────────────────────────────────────────────────────  */

static void py_call_start(JceScript *sc, JceScriptInstance inst)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "call_start", "(K)", (unsigned long long)inst));
    PY_SLOT_END();
}

static void py_call_update(JceScript *sc, JceScriptInstance inst, float dt)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "call_update", "(Kd)",
                         (unsigned long long)inst, (double)dt));
    PY_SLOT_END();
}

/* The FIXED-step half: same marshalling, a different method on the Python
 * runtime object, and a dt that is the physics step rather than the frame. */
static void py_call_fixed_update(JceScript *sc, JceScriptInstance inst,
                                 float dt)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "call_fixed_update", "(Kd)",
                         (unsigned long long)inst, (double)dt));
    PY_SLOT_END();
}

static void py_call_collision(JceScript *sc, JceScriptInstance inst,
                              JceScriptEntity other_entity)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "call_collision", "(KK)",
                         (unsigned long long)inst,
                         (unsigned long long)other_entity));
    PY_SLOT_END();
}

static void py_call_message(JceScript *sc, JceScriptInstance inst,
                            const char *msg_name, double number_arg,
                            const char *str_arg)
{
    PY_SLOT_VOID(sc);
    /* "z" for str_arg: NULL becomes None, which is Python's nil — the same
     * thing lua_pushnil does on the other side of this comparison. */
    py_finish(s, py_call(s, "call_message", "(Kzdz)",
                         (unsigned long long)inst, msg_name, number_arg,
                         str_arg));
    PY_SLOT_END();
}

static void py_call_anim_event(JceScript *sc, JceScriptInstance inst,
                               uint32_t id, const char *name,
                               float f0, float f1, int i0)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "call_anim_event", "(Kkzddi)",
                         (unsigned long long)inst, (unsigned long)id, name,
                         (double)f0, (double)f1, i0));
    PY_SLOT_END();
}

/* ── The three global-handler dispatchers ────────────────────────────────
 *
 * call_named_num and call_named_str are the two whose absence is invisible:
 * every UISlider / UIToggle / UIDropdown / UIInputField handler goes through
 * them and `false` means "no such global", which is what a correctly absent
 * handler also returns.  They are here, filled, and exercised by the
 * lifecycle differential's ui_handler cases. */
static bool py_named_result(PyObject *res)
{
    return res && PyObject_IsTrue(res) == 1;
}

static bool py_call_named(JceScript *sc, const char *fn_name,
                          JceScriptEntity arg_entity)
{
    PyObject *res;
    bool      hit;
    PY_SLOT_RET(sc, false);
    res = py_call(s, "call_named", "(zK)", fn_name,
                  (unsigned long long)arg_entity);
    hit = py_named_result(res);
    py_finish(s, res);
    PY_SLOT_END();
    return hit;
}

static bool py_call_named_num(JceScript *sc, const char *fn_name,
                              JceScriptEntity arg_entity, double value)
{
    PyObject *res;
    bool      hit;
    PY_SLOT_RET(sc, false);
    res = py_call(s, "call_named_num", "(zKd)", fn_name,
                  (unsigned long long)arg_entity, value);
    hit = py_named_result(res);
    py_finish(s, res);
    PY_SLOT_END();
    return hit;
}

static bool py_call_named_str(JceScript *sc, const char *fn_name,
                              JceScriptEntity arg_entity, const char *str)
{
    PyObject *res;
    bool      hit;
    PY_SLOT_RET(sc, false);
    res = py_call(s, "call_named_str", "(zKz)", fn_name,
                  (unsigned long long)arg_entity, str);
    hit = py_named_result(res);
    py_finish(s, res);
    PY_SLOT_END();
    return hit;
}

/* ── Bookkeeping ─────────────────────────────────────────────────────────  */

static int py_instance_count(const JceScript *sc)
{
    PyObject *res;
    int       n = 0;
    /* The handle is const to the public surface and not to us: the count is
     * read by calling into Python, which is a mutation of nothing the caller
     * can observe.  jce_script_instance_count is const for the same reason on
     * the Lua side, where it reads a plain int. */
    union { const JceScript *in; JceScript *out; } u;
    u.in = sc;
    {
        PY_SLOT_RET(u.out, 0);
        res = py_call(s, "instance_count", "()");
        if (res) n = (int)PyLong_AsLong(res);
        if (PyErr_Occurred()) { PyErr_Clear(); n = 0; }
        py_finish(s, res);
        PY_SLOT_END();
    }
    return n;
}

static void py_update_coroutines(JceScript *sc, float dt)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "update_coroutines", "(d)", (double)dt));
    PY_SLOT_END();
}

/* ── Hot reload ──────────────────────────────────────────────────────────  */

static JceScriptModule py_compile_module(JceScript *sc, const char *name,
                                         const char *source, size_t len)
{
    PyObject       *res;
    JceScriptModule mod = 0u;
    PY_SLOT_RET(sc, 0u);

    if (source) {
        /* "s#" takes an explicit length, because compile_module's contract is
         * a pointer AND a length: the caller's buffer is not required to be
         * NUL-terminated and reading to a NUL would run off it. */
        res = py_call(s, "compile_module", "(zs#)", name, source,
                      (Py_ssize_t)len);
        if (res) mod = (JceScriptModule)PyLong_AsUnsignedLongLongMask(res);
        py_finish(s, res);
    }
    PY_SLOT_END();
    return mod;
}

static void py_rebind_instance(JceScript *sc, JceScriptInstance inst,
                               JceScriptModule mod)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "rebind_instance", "(KK)",
                         (unsigned long long)inst, (unsigned long long)mod));
    PY_SLOT_END();
}

static void py_release_module(JceScript *sc, JceScriptModule mod)
{
    PY_SLOT_VOID(sc);
    py_finish(s, py_call(s, "release_module", "(K)",
                         (unsigned long long)mod));
    PY_SLOT_END();
}

/* ── The table ───────────────────────────────────────────────────────────
 *
 * POSITIONAL initialisers, for the reason jce_script.c's k_lua_vm states:
 * designated initialisers would survive a reordering of JceScriptVM and these
 * do not.  struct_size comes from sizeof and is never summed by hand.
 * Every slot is filled; registration refuses a NULL one. */
static const JceScriptVM k_python_vm = {
    sizeof(JceScriptVM),
    "python",
    py_create_sized,
    py_destroy,
    py_instantiate,
    py_instantiate_source,
    py_call_start,
    py_call_update,
    py_release,
    py_call_collision,
    py_call_message,
    py_call_anim_event,
    py_call_named,
    py_call_named_num,
    py_call_named_str,
    py_instance_count,
    py_update_coroutines,
    py_compile_module,
    py_rebind_instance,
    py_release_module,
    py_call_fixed_update,   /* APPENDED -- see jce_script_vm.h */
};

const JceScriptVM *jce_script_vm_python(void)
{
    return &k_python_vm;
}

bool jce_script_vm_python_register(void)
{
    if (!jce_script_vm_register(&k_python_vm)) return false;
    /* Claim the extension, or nothing authored in Python is ever SELECTED.
     * The runtime resolves each Script component's path to a language through
     * jce_script_vm_language_for_path(), and that answer comes from claims
     * like this one — there is no table of languages in any engine file, so a
     * backend that registers and does not claim is reachable only by name.
     * The claim must follow the registration: a claim naming an unregistered
     * language is refused, which is what keeps the registered-language list a
     * sound test for "is this backend in this executable". */
    return jce_script_vm_register_extension("py", "python");
}

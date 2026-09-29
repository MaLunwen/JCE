/*
 * jce_script_vm_js.c — the "js" JceScriptVM over vendored quickjs-ng.
 *
 * Read jce_script_vm_js.h first: it holds the two decisions that shape this
 * file (why the extension is .jcejs, and why an instance is an OBJECT and not
 * a context), and the exception rule.
 *
 * THE SCRIPT SHAPE, and it is Lua's on purpose.  A .jcejs file is a module
 * body that RETURNS its instance object:
 *
 *     const M = {};
 *     M.on_start  = function ()   { ... this.entity ... };
 *     M.on_update = function (dt) { ... };
 *     return M;
 *
 * The source is wrapped in a function and called, so `return` at top level is
 * legal and each instantiation gets a FRESH object from a fresh call — the
 * same one-file-many-instances property Lua's `return M` gives, without
 * needing ES modules (whose loader is asynchronous and whose instances would
 * be shared).  `this` inside a handler is the instance, so `this.entity` is
 * the owner, matching every other language in this engine.
 *
 * THE `jce` OBJECT IS INSTALLED BY js_create, from code tools/scriptgen
 * generates (emit_js.py -> src/jce_script_bindings_js.gen.c).  Nothing in
 * this file hand-writes a binding, deliberately: a second hand-written
 * binding surface is the thing the generators exist to prevent, and a
 * half-populated `jce` would be worse than an absent one -- a script author
 * would find one call and reasonably assume its neighbours were there too.
 *
 * The install is FATAL when it fails.  A VM that came up without `jce`
 * would run every lifecycle callback and silently do nothing, which is a
 * bug report about the script rather than about this backend. */

#include <jce/script_vm/jce_script_vm_js.h>

#include <jce/middleware/script/jce_script_vm.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include "jce_script_js_internal.h"
#include "jce_script_bindings_js.gen.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "script.js"

/* One reported line.  A JS stack trace is the long case; anything past this is
 * truncated rather than allocated, because the barrier that uses it runs on
 * the failure path and must not be able to fail itself. */
#define JCE_JS_VM_MSG_MAX 1024

/* Defined at the bottom, used by create_sized above: the handle must point at
 * OUR table before it is returned, and jce_script_vm_create() verifies that. */
static const JceScriptVM *jce_script_vm_js_table(void);


/* ── Exception barrier ───────────────────────────────────────────────────
 *
 * CHECK, REPORT, CLEAR — in that order, on every path that can fail.  An
 * un-cleared pending exception poisons every later call on the context, so a
 * dispatcher that skipped this would turn one script's bug into every
 * script's bug, several frames later.  Same contract as scripting/java's
 * java_take_exception, whose header says the same thing about uniformity.
 *
 * Returns true when it consumed an exception. */
/* Emit one line the way jce_script.c does: host.log when the host is meant to
 * see it, and the engine log always.  A script error IS host-visible: the
 * editor console is where a designer looks for it, and an error that reaches
 * only the engine log is an error the person who wrote the script never sees.
 * scripting/python does the same through py_emit(); this is that shape. */
static void js_emit(JceJsScript *s, const char *text)
{
    if (s && s->have_host && s->host.log) s->host.log(s->host.user, text);
    LOG_ERROR(LOG_TAG, "%s", text);
}

static bool js_take_exception(JceJsScript *s, const char *what)
{
    JSValue exc;
    const char *msg;
    char line[JCE_JS_VM_MSG_MAX];

    if (!s || !s->ctx) return false;
    if (!JS_HasException(s->ctx)) return false;

    exc = JS_GetException(s->ctx);
    msg = JS_ToCString(s->ctx, exc);
    snprintf(line, sizeof line, "%s: %s", what ? what : "script",
             msg ? msg : "(exception with no string form)");
    js_emit(s, line);

    /* The stack, when the throw was an Error.  Not fatal if absent: a
     * `throw 7` has no .stack and must still be reported, above. */
    if (JS_IsObject(exc)) {
        JSValue st = JS_GetPropertyStr(s->ctx, exc, "stack");
        if (!JS_IsUndefined(st) && !JS_IsException(st)) {
            const char *sst = JS_ToCString(s->ctx, st);
            if (sst && sst[0]) {
                snprintf(line, sizeof line, "  %s", sst);
                js_emit(s, line);
            }
            if (sst) JS_FreeCString(s->ctx, sst);
        }
        JS_FreeValue(s->ctx, st);
    }

    if (msg) JS_FreeCString(s->ctx, msg);
    JS_FreeValue(s->ctx, exc);
    return true;
}

/* ── Instance table ──────────────────────────────────────────────────── */

static JSValue *js_slot(JceJsScript *s, JceScriptInstance inst)
{
    int i;
    if (!s || inst == 0u) return NULL;
    i = (int)(inst - 1u);
    if (i < 0 || i >= s->inst_cap) return NULL;
    if (JS_IsUndefined(s->inst[i])) return NULL;
    return &s->inst[i];
}

static JceScriptInstance js_store(JceJsScript *s, JSValue v)
{
    int i;
    for (i = 0; i < s->inst_cap; ++i) {
        if (JS_IsUndefined(s->inst[i])) {
            s->inst[i] = v;
            ++s->inst_live;
            return (JceScriptInstance)(i + 1);
        }
    }
    {
        const int ncap = s->inst_cap ? s->inst_cap * 2 : 16;
        JSValue *n = (JSValue *)jce_malloc((size_t)ncap * sizeof *n);
        if (!n) { JS_FreeValue(s->ctx, v); return 0u; }
        if (s->inst_cap)
            memcpy(n, s->inst, (size_t)s->inst_cap * sizeof *n);
        for (i = s->inst_cap; i < ncap; ++i) n[i] = JS_UNDEFINED;
        jce_free(s->inst);
        s->inst = n;
        i = s->inst_cap;
        s->inst_cap = ncap;
    }
    s->inst[i] = v;
    ++s->inst_live;
    return (JceScriptInstance)(i + 1);
}

/* ── Calling one handler on one instance ─────────────────────────────────
 *
 * An ABSENT handler is not an error and not a warning: a script that only
 * implements on_update is the common case, and Lua treats it the same way. */
static void js_call_handler(JceJsScript *s, JceScriptInstance inst,
                            const char *name, int argc, JSValue *argv)
{
    JSValue *self, fn, r;
    int i;

    self = js_slot(s, inst);
    if (!self) goto done;

    fn = JS_GetPropertyStr(s->ctx, *self, name);
    if (JS_IsException(fn)) { js_take_exception(s, name); goto done; }
    if (!JS_IsFunction(s->ctx, fn)) { JS_FreeValue(s->ctx, fn); goto done; }

    r = JS_Call(s->ctx, fn, *self, argc, argv);
    JS_FreeValue(s->ctx, fn);
    if (JS_IsException(r)) js_take_exception(s, name);
    JS_FreeValue(s->ctx, r);

done:
    for (i = 0; i < argc; ++i) JS_FreeValue(s->ctx, argv[i]);
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static JceScript *js_create_sized(const JceScriptHost *host, size_t host_size)
{
    JceJsScript *s = (JceJsScript *)jce_malloc(sizeof *s);
    if (!s) return NULL;
    memset(s, 0, sizeof *s);

    /* MUST point at OUR table before returning.  jce_script_vm_create()
     * verifies it and then repoints it at the registry's clamped copy. */
    s->hdr.vm = jce_script_vm_js_table();

    if (host && host_size > 0u) {
        /* min(caller, ours) over a ZEROED destination.  JceScriptHost is
         * caller-allocated and grows; copying at our own sizeof over-reads a
         * consumer built against an older header and then calls whatever
         * followed it. */
        const size_t n = host_size < sizeof s->host ? host_size
                                                    : sizeof s->host;
        memcpy(&s->host, host, n);
        s->have_host = true;
    }

    s->rt = JS_NewRuntime();
    if (!s->rt) { jce_free(s); return NULL; }
    s->ctx = JS_NewContext(s->rt);
    if (!s->ctx) { JS_FreeRuntime(s->rt); jce_free(s); return NULL; }

    /* Where every generated binding finds `s`.  Lua threads it through a
     * closure upvalue; QuickJS has a per-context opaque and this is it, set
     * ONCE here so no binding has to be handed it.  Set before the install
     * below, because a JSCFunctionListEntry could in principle be invoked
     * during registration. */
    JS_SetContextOpaque(s->ctx, s);

    /* The global `jce` object.  Without it a .jcejs script receives lifecycle
     * callbacks and can do NOTHING with them -- it cannot read a transform or
     * move an entity -- so a failure here is fatal to the VM rather than a
     * degraded mode nobody would notice until a script silently did nothing. */
    if (!jce_script_js_install_bindings(s)) {
        LOG_ERROR(LOG_TAG, "could not install the `jce` bindings");
        JS_FreeContext(s->ctx);
        JS_FreeRuntime(s->rt);
        jce_free(s);
        return NULL;
    }

    LOG_INFO(LOG_TAG, "QuickJS VM created (quickjs-ng %s)", JS_GetVersion());
    return (JceScript *)s;
}

static void js_destroy(JceScript *sc)
{
    JceJsScript *s = (JceJsScript *)sc;
    int i;
    if (!s) return;
    for (i = 0; i < s->inst_cap; ++i)
        if (!JS_IsUndefined(s->inst[i])) JS_FreeValue(s->ctx, s->inst[i]);
    jce_free(s->inst);
    if (s->ctx) JS_FreeContext(s->ctx);
    if (s->rt)  JS_FreeRuntime(s->rt);
    jce_free(s);
}

static JceScriptInstance js_instantiate_source(JceScript *sc, const char *name,
                                               const char *source,
                                               JceScriptEntity owner)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue      fn, obj;
    char        *wrapped;
    size_t       n;
    static const char PRE[] = "(function(){";
    static const char POST[] = "\n})";

    if (!s || !s->ctx || !source) return 0u;

    /* Wrapped in a function so a top-level `return M` is legal and each
     * instantiation gets a FRESH object — the same property Lua's `return M`
     * has.  ES modules would give one shared instance and an async loader. */
    n = sizeof PRE - 1u + strlen(source) + sizeof POST;
    wrapped = (char *)jce_malloc(n);
    if (!wrapped) return 0u;
    memcpy(wrapped, PRE, sizeof PRE - 1u);
    memcpy(wrapped + sizeof PRE - 1u, source, strlen(source));
    memcpy(wrapped + sizeof PRE - 1u + strlen(source), POST, sizeof POST);

    fn = JS_Eval(s->ctx, wrapped, strlen(wrapped),
                 name ? name : "<jcejs>", JS_EVAL_TYPE_GLOBAL);
    jce_free(wrapped);
    if (JS_IsException(fn)) {
        js_take_exception(s, name ? name : "<jcejs>");
        JS_FreeValue(s->ctx, fn);
        return 0u;
    }

    obj = JS_Call(s->ctx, fn, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(s->ctx, fn);
    if (JS_IsException(obj)) {
        js_take_exception(s, name ? name : "<jcejs>");
        JS_FreeValue(s->ctx, obj);
        return 0u;
    }
    if (!JS_IsObject(obj)) {
        LOG_ERROR(LOG_TAG,
                  "'%s' did not return an object. A .jcejs script ends with "
                  "`return M;` where M carries the handlers — same shape as a "
                  ".lua script's `return M`.", name ? name : "<jcejs>");
        JS_FreeValue(s->ctx, obj);
        return 0u;
    }

    /* `this.entity` inside every handler, matching every other language. */
    JS_SetPropertyStr(s->ctx, obj, "entity",
                      JS_NewInt64(s->ctx, (int64_t)owner));
    return js_store(s, obj);
}

static JceScriptInstance js_instantiate(JceScript *sc, const char *path,
                                        JceScriptEntity owner)
{
    JceJsScript      *s = (JceJsScript *)sc;
    void             *buf;
    uint64_t          size = 0u;
    char             *src;
    JceScriptInstance inst;

    if (!s || !s->ctx || !path) return 0u;
    /* Through the host's read_file, exactly as the lua and python backends
     * do, so every language loads a script through the same door and a host
     * with no read_file refuses all of them identically. */
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
    /* read_file need not NUL-terminate; JS_Eval takes a length but the wrap
     * above uses strlen, so terminate here once. */
    src = (char *)jce_malloc((size_t)size + 1u);
    if (!src) { jce_free(buf); return 0u; }
    memcpy(src, buf, (size_t)size);
    src[(size_t)size] = '\0';
    jce_free(buf);

    inst = js_instantiate_source(sc, path, src, owner);
    jce_free(src);
    return inst;
}

static void js_call_start(JceScript *sc, JceScriptInstance inst)
{
    js_call_handler((JceJsScript *)sc, inst, "on_start", 0, NULL);
}

static void js_call_update(JceScript *sc, JceScriptInstance inst, float dt)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue a[1];
    if (!s || !s->ctx) return;
    a[0] = JS_NewFloat64(s->ctx, (double)dt);
    js_call_handler(s, inst, "on_update", 1, a);
}

/* The FIXED-step half of the lifecycle: same shape as js_call_update, a
 * different method name and a dt that does not vary with the frame rate. */
static void js_call_fixed_update(JceScript *sc, JceScriptInstance inst,
                                 float dt)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue a[1];
    if (!s || !s->ctx) return;
    a[0] = JS_NewFloat64(s->ctx, (double)dt);
    js_call_handler(s, inst, "on_fixed_update", 1, a);
}

static void js_release(JceScript *sc, JceScriptInstance inst)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue     *slot = js_slot(s, inst);
    if (!slot) return;
    JS_FreeValue(s->ctx, *slot);
    *slot = JS_UNDEFINED;
    if (s->inst_live > 0) --s->inst_live;
}

static void js_call_collision(JceScript *sc, JceScriptInstance inst,
                              JceScriptEntity other)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue a[1];
    if (!s || !s->ctx) return;
    a[0] = JS_NewInt64(s->ctx, (int64_t)other);
    js_call_handler(s, inst, "on_collision", 1, a);
}

static void js_call_message(JceScript *sc, JceScriptInstance inst,
                            const char *msg, double num, const char *str)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue a[3];
    if (!s || !s->ctx) return;
    a[0] = JS_NewString(s->ctx, msg ? msg : "");
    a[1] = JS_NewFloat64(s->ctx, num);
    a[2] = str ? JS_NewString(s->ctx, str) : JS_NULL;
    js_call_handler(s, inst, "on_message", 3, a);
}

static void js_call_anim_event(JceScript *sc, JceScriptInstance inst,
                               uint32_t id, const char *name,
                               float f0, float f1, int i0)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue a[5];
    if (!s || !s->ctx) return;
    a[0] = JS_NewUint32(s->ctx, id);
    a[1] = JS_NewString(s->ctx, name ? name : "");
    a[2] = JS_NewFloat64(s->ctx, (double)f0);
    a[3] = JS_NewFloat64(s->ctx, (double)f1);
    a[4] = JS_NewInt32(s->ctx, i0);
    js_call_handler(s, inst, "on_anim_event", 5, a);
}

/* ── The three global dispatchers ────────────────────────────────────────
 *
 * TWO OF THESE FAIL SILENTLY BY CONTRACT.  call_named_num and call_named_str
 * are how UISlider, UIToggle, UIDropdown and UIInputField handlers dispatch,
 * and both return false for "no such global" — indistinguishable from a
 * correctly absent handler.  A VM that omitted them would break every UI
 * callback in a game with no error anywhere, which is why the registry
 * refuses a table with a NULL slot.  They are implemented, not stubbed. */
static bool js_call_named_impl(JceJsScript *s, const char *fn_name,
                               int argc, JSValue *argv)
{
    JSValue g, fn, r;
    bool    called = false;
    int     i;

    if (!s || !s->ctx || !fn_name) goto done;

    g  = JS_GetGlobalObject(s->ctx);
    fn = JS_GetPropertyStr(s->ctx, g, fn_name);
    JS_FreeValue(s->ctx, g);
    if (JS_IsException(fn)) { js_take_exception(s, fn_name); goto done; }
    if (!JS_IsFunction(s->ctx, fn)) { JS_FreeValue(s->ctx, fn); goto done; }

    r = JS_Call(s->ctx, fn, JS_UNDEFINED, argc, argv);
    JS_FreeValue(s->ctx, fn);
    if (JS_IsException(r)) js_take_exception(s, fn_name);
    else                   called = true;
    JS_FreeValue(s->ctx, r);

done:
    if (s && s->ctx)
        for (i = 0; i < argc; ++i) JS_FreeValue(s->ctx, argv[i]);
    return called;
}

static bool js_call_named(JceScript *sc, const char *fn_name,
                          JceScriptEntity e)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue a[1];
    if (!s || !s->ctx) return false;
    a[0] = JS_NewInt64(s->ctx, (int64_t)e);
    return js_call_named_impl(s, fn_name, 1, a);
}

static bool js_call_named_num(JceScript *sc, const char *fn_name,
                              JceScriptEntity e, double v)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue a[2];
    if (!s || !s->ctx) return false;
    a[0] = JS_NewInt64(s->ctx, (int64_t)e);
    a[1] = JS_NewFloat64(s->ctx, v);
    return js_call_named_impl(s, fn_name, 2, a);
}

static bool js_call_named_str(JceScript *sc, const char *fn_name,
                              JceScriptEntity e, const char *str)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue a[2];
    if (!s || !s->ctx) return false;
    a[0] = JS_NewInt64(s->ctx, (int64_t)e);
    a[1] = str ? JS_NewString(s->ctx, str) : JS_NULL;
    return js_call_named_impl(s, fn_name, 2, a);
}

static int js_instance_count(const JceScript *sc)
{
    const JceJsScript *s = (const JceJsScript *)sc;
    return s ? s->inst_live : 0;
}

/* Drains the microtask queue: a Promise settled during a handler resolves
 * here, once per frame, on the engine's clock.
 *
 * QuickJS runs NOTHING on its own — a resolved promise sits in the job queue
 * until someone calls JS_ExecutePendingJob.  Which is why this slot is the
 * right home for it rather than a hidden drain inside call_update: a script
 * that resolves a promise from on_collision must still see it run.
 *
 * NOT a timer wheel, and that is a real limit rather than an oversight: the
 * `jce` object carries no wait_seconds, because no host member does -- the
 * manifest is the whole surface and nothing in it is time-parked.  If one
 * ever lands, a dt-keyed wheel of parked resolvers belongs HERE, because
 * `dt` arrives here and a microtask queue has no clock. */
static void js_update_coroutines(JceScript *sc, float dt)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSContext   *pctx = NULL;
    int          n;
    (void)dt;
    if (!s || !s->rt) return;
    for (;;) {
        n = JS_ExecutePendingJob(s->rt, &pctx);
        if (n <= 0) {
            /* n < 0 means the job threw; the exception is pending on pctx. */
            if (n < 0) js_take_exception(s, "pending job");
            break;
        }
    }
}

/* ── Hot reload ──────────────────────────────────────────────────────────
 *
 * A .jcejs module is compiled to a fresh constructor function; rebinding an
 * instance calls it again and swaps the object, so a designer's save reaches
 * a running scene.  `entity` is re-applied because it lives on the instance
 * and the new object has never seen it.
 *
 * State does NOT survive: the new object is what the file says it is.  That
 * matches the cpp backend's position (a reload cannot preserve `self` when
 * `self` is of a type that no longer exists) and is honest about it here
 * rather than half-copying properties and producing an object that is neither
 * the old one nor the new one. */
static JceScriptModule js_compile_module(JceScript *sc, const char *name,
                                         const char *source, size_t len)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue      fn;
    char        *wrapped;
    size_t       n;
    static const char PRE[] = "(function(){";
    static const char POST[] = "\n})";

    if (!s || !s->ctx || !source) return 0u;
    n = sizeof PRE - 1u + len + sizeof POST;
    wrapped = (char *)jce_malloc(n);
    if (!wrapped) return 0u;
    memcpy(wrapped, PRE, sizeof PRE - 1u);
    memcpy(wrapped + sizeof PRE - 1u, source, len);
    memcpy(wrapped + sizeof PRE - 1u + len, POST, sizeof POST);

    fn = JS_Eval(s->ctx, wrapped, strlen(wrapped),
                 name ? name : "<jcejs>", JS_EVAL_TYPE_GLOBAL);
    jce_free(wrapped);
    if (JS_IsException(fn)) {
        js_take_exception(s, name ? name : "<jcejs>");
        JS_FreeValue(s->ctx, fn);
        return 0u;
    }
    if (!JS_IsFunction(s->ctx, fn)) { JS_FreeValue(s->ctx, fn); return 0u; }

    /* PROBE IT ONCE.  A handle this function returns is a promise that
     * rebind_instance can produce an instance from it, and a body that does
     * not `return M` cannot keep that promise -- it would rebind to nothing,
     * silently, and the designer would watch a saved file do nothing at all.
     * script_lua_compile_module() answers 0 in exactly this case (it runs the
     * chunk and demands a table), and the editor's reload path prints
     * "failed to compile; keeping previous" off that 0.  So the cost here is
     * one extra evaluation of the module's top level PER RELOAD -- not per
     * instance and not per frame -- bought against a reload that lies. */
    {
        JSValue probe = JS_Call(s->ctx, fn, JS_UNDEFINED, 0, NULL);
        bool    ok;
        if (JS_IsException(probe)) {
            js_take_exception(s, name ? name : "<jcejs>");
            JS_FreeValue(s->ctx, probe);
            JS_FreeValue(s->ctx, fn);
            return 0u;
        }
        ok = JS_IsObject(probe);
        JS_FreeValue(s->ctx, probe);
        if (!ok) {
            char line[JCE_JS_VM_MSG_MAX];
            snprintf(line, sizeof line,
                     "reload compile error (%s): the module did not return an "
                     "object. A .jcejs script ends with `return M;`.",
                     name ? name : "<jcejs>");
            js_emit(s, line);
            JS_FreeValue(s->ctx, fn);
            return 0u;
        }
    }

    /* The module handle is an instance slot holding the constructor. */
    return (JceScriptModule)js_store(s, fn);
}

static void js_rebind_instance(JceScript *sc, JceScriptInstance inst,
                               JceScriptModule mod)
{
    JceJsScript *s = (JceJsScript *)sc;
    JSValue     *slot, *ctor, obj, ent;

    slot = js_slot(s, inst);
    ctor = js_slot(s, (JceScriptInstance)mod);
    if (!slot || !ctor) return;

    obj = JS_Call(s->ctx, *ctor, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(obj)) {
        js_take_exception(s, "rebind");
        JS_FreeValue(s->ctx, obj);
        return;                        /* keep the previous instance */
    }
    if (!JS_IsObject(obj)) {
        /* compile_module probed for this, so reaching it means the module
         * became non-deterministic between the probe and now.  Reported, not
         * swallowed: a silent return here is a reload that appears to work. */
        char line[JCE_JS_VM_MSG_MAX];
        snprintf(line, sizeof line,
                 "rebind: the module returned no object this time; keeping "
                 "the previous instance");
        js_emit(s, line);
        JS_FreeValue(s->ctx, obj);
        return;
    }

    ent = JS_GetPropertyStr(s->ctx, *slot, "entity");
    JS_SetPropertyStr(s->ctx, obj, "entity", ent);   /* moves the ref */

    JS_FreeValue(s->ctx, *slot);
    *slot = obj;
}

static void js_release_module(JceScript *sc, JceScriptModule mod)
{
    js_release(sc, (JceScriptInstance)mod);
}

/* ── The table ───────────────────────────────────────────────────────────
 *
 * POSITIONAL, not designated, and that is load-bearing: jce_script_vm.h says
 * an inserted or reordered member retypes every slot after it and stops the
 * build, and designated initialisers would survive exactly that. */
static const JceScriptVM k_js_vm = {
    sizeof(JceScriptVM),
    JCE_SCRIPT_VM_JS_LANGUAGE,
    js_create_sized,
    js_destroy,
    js_instantiate,
    js_instantiate_source,
    js_call_start,
    js_call_update,
    js_release,
    js_call_collision,
    js_call_message,
    js_call_anim_event,
    js_call_named,
    js_call_named_num,
    js_call_named_str,
    js_instance_count,
    js_update_coroutines,
    js_compile_module,
    js_rebind_instance,
    js_release_module,
    js_call_fixed_update,   /* APPENDED -- see jce_script_vm.h */
};

static const JceScriptVM *jce_script_vm_js_table(void)
{
    return &k_js_vm;
}

bool jce_script_vm_js_register(void)
{
    static bool s_done;
    if (s_done) return true;
    if (!jce_script_vm_register(&k_js_vm)) return false;
    /* The LANGUAGE first, then the claim.  register_extension refuses a claim
     * for a language that is not registered yet -- deliberately, so "backend
     * not linked" never becomes "extension resolves to a VM that does not
     * exist". */
    if (!jce_script_vm_register_extension(JCE_SCRIPT_VM_JS_EXTENSION,
                                          JCE_SCRIPT_VM_JS_LANGUAGE))
        return false;
    s_done = true;
    return true;
}

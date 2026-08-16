/* JceScriptRuntime.java — one per JceScript handle.
 *
 * The Java half of the Java JceScriptVM.  The native half
 * (scripting/java/native/jce_script_vm_java.c) owns the 18 vtable slots and
 * calls exactly the methods below; every one of them takes and returns
 * PRIMITIVES and Strings, so a native slot creates at most two local
 * references per dispatch and needs no object graph to marshal.
 *
 * ── ONE JVM, MANY HANDLES, AND WHAT KEEPS THEM APART ─────────────────────
 *
 * JNI_CreateJavaVM may be called once per process, so a second JceScript
 * handle REUSES the JVM (the native side states and enforces that rule).  Two
 * Lua handles are two lua_States and share nothing; two Java handles would
 * share every static field in the process if nothing separated them.  What
 * separates them is this object plus its own JceScriptClassLoader: instances,
 * modules, named handlers, the coroutine table and every class this handle
 * defines belong to the handle, and a class defined through handle A is a
 * DIFFERENT Class object from the same source defined through handle B — so
 * their statics are different storage.
 * *Enforced by:* tests/scripting/java/vm/test_jce_script_vm_java.c ::
 * test_two_handles_do_not_share_script_statics.
 *
 * What is genuinely shared, and is documented rather than hidden: classes
 * loaded from the application classpath (the parent loader) — including this
 * one — and everything the JVM itself owns.  GC pauses land inside the frame
 * loop; nothing here pretends otherwise.
 *
 * ── ERRORS PROPAGATE.  THAT IS THE DESIGN. ───────────────────────────────
 *
 * Nothing here catches a Throwable thrown by script code.  It travels out
 * through JNI, where the native slot checks it, clears it and reports it
 * through JceScriptHost::log — structurally what lua_pcall does for Lua.  A
 * catch here would make the native check unreachable.  Reflective dispatch
 * unwraps InvocationTargetException and rethrows the CAUSE, so the reported
 * error names the script's exception and not the reflection wrapper.
 */
package com.jce.script.vm;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class JceScriptRuntime {

    /* Lua's scheduler bound, verbatim: JCE_SCRIPT_MAX_COROUTINES. */
    private static final int MAX_COROUTINES = 256;

    private static final int H_START     = 1 << 0;
    private static final int H_UPDATE    = 1 << 1;
    private static final int H_DESTROY   = 1 << 2;
    private static final int H_COLLISION = 1 << 3;
    private static final int H_ANIM      = 1 << 4;

    /** One live script instance: the state that survives a rebind, plus the
     *  behaviour object that does not. */
    static final class Instance {
        final long entity;
        final Map<String, Object> state = new LinkedHashMap<String, Object>();
        JceEntityScript behaviour;
        int handlers;
        /* THE FAILING-CALLBACK RULE (jce_script.h): the hooks that threw and
         * are therefore not dispatched again on THIS instance until a rebind.
         * Same H_* bits as `handlers`, deliberately: "the script declares it"
         * and "we are still willing to call it" are the two halves of one
         * question, and live() answers both with one mask each. */
        int disabled;

        Instance(long entity) { this.entity = entity; }
    }

    private final long nativeHandle;   /* the JavaScript * that owns us */
    private final long hostPointer;    /* &JavaScript::host, 0 when hostless */
    private final long hostSize;

    private final JceScriptClassLoader loader;
    private final Map<Integer, Instance> instances = new HashMap<Integer, Instance>();
    private final Map<Integer, Class<?>> modules = new HashMap<Integer, Class<?>>();
    private final Map<String, List<Method>> named = new HashMap<String, List<Method>>();

    private int nextInstance = 1;
    private int nextModule = 1;

    private final Runnable[] coroBody = new Runnable[MAX_COROUTINES];
    private final float[] coroRemaining = new float[MAX_COROUTINES];
    private int coroCount;

    /* Reached from create_sized through GetMethodID, which ignores Java access
     * control — the visibility here is documentation for humans. */
    public JceScriptRuntime(long nativeHandle, long hostPointer, long hostSize) {
        this.nativeHandle = nativeHandle;
        this.hostPointer = hostPointer;
        this.hostSize = hostSize;
        this.loader = new JceScriptClassLoader(
                JceScriptRuntime.class.getClassLoader());
    }

    /* ── The host, for JceEntityScript and JceScriptSurface ──────────── */

    void hostLog(String message) {
        nativeLog(nativeHandle, message);
    }

    boolean hostGetPosition(long e, float[] outXyz) {
        if (outXyz == null || outXyz.length < 3) {
            throw new IllegalArgumentException(
                "getPosition needs a float[3]; got "
                + (outXyz == null ? "null" : String.valueOf(outXyz.length)));
        }
        return nativeGetPosition(nativeHandle, e, outXyz);
    }

    void hostSetPosition(long e, float x, float y, float z) {
        nativeSetPosition(nativeHandle, e, x, y, z);
    }

    /** The JceScriptHost this VM was created with, as a raw pointer, and the
     *  size it was copied at.  That pair is exactly what
     *  com.jce.script.JceScript.open takes; JceScriptSurface is the one
     *  caller.  0 when the VM was created without a host. */
    public long hostPointer() { return hostPointer; }
    public long hostSize()    { return hostSize; }

    /* The opened scripting surface, held as AutoCloseable ON PURPOSE: naming
     * com.jce.script.JceScript here would put it in THIS class's constant pool
     * and make verifying THIS class load the native script library, which is
     * exactly the coupling JceScriptSurface exists to keep opt-in. */
    private AutoCloseable surface;

    AutoCloseable surface() { return surface; }
    void surface(AutoCloseable open) { this.surface = open; }

    /** Called from the VM's destroy slot.  Closes the scripting surface if one
     *  was ever opened; everything else this object owns is plain Java heap
     *  and belongs to the garbage collector. */
    public void shutdown() {
        AutoCloseable open = surface;
        surface = null;
        if (open == null) return;
        try {
            open.close();
        } catch (Exception e) {
            hostLog("closing the scripting surface failed: " + e);
        }
    }

    private static native void nativeLog(long handle, String message);
    private static native boolean nativeGetPosition(long handle, long e, float[] out);
    private static native void nativeSetPosition(long handle, long e,
                                                 float x, float y, float z);

    /* ── Slot: instantiate_source ────────────────────────────────────── */

    public int instantiateSource(String name, String source, long owner) {
        return spawn(defineFromSource(name, source), owner);
    }

    /* ── Slot: instantiate ───────────────────────────────────────────────
     *
     * The bytes come from JceScriptHost::read_file, exactly as they do for
     * Lua.  Source or compiled class is decided by the class-file magic, for
     * the same reason luaL_loadbuffer accepts both a .lua text and a
     * precompiled chunk: it is one asset slot, and the loader recognises what
     * it was handed rather than making the caller declare it. */
    public int instantiate(String path, byte[] bytes, long owner) {
        Class<?> c = isClassFile(bytes)
                ? loader.defineFromClassFile(bytes)
                : defineFromSource(path, new String(bytes, StandardCharsets.UTF_8));
        return spawn(c, owner);
    }

    private static boolean isClassFile(byte[] b) {
        return b != null && b.length >= 4
                && (b[0] & 0xFF) == 0xCA && (b[1] & 0xFF) == 0xFE
                && (b[2] & 0xFF) == 0xBA && (b[3] & 0xFF) == 0xBE;
    }

    private Class<?> defineFromSource(String name, String source) {
        return loader.defineFromSource(JceScriptCompiler.compile(name, source));
    }

    /* Build the instance.  Returns 0 for "this is not a script", which is the
     * answer Lua gives when a chunk returns something that is not a table. */
    private int spawn(Class<?> c, long owner) {
        registerNamed(c);
        if (!JceEntityScript.class.isAssignableFrom(c)) {
            hostLog("script '" + c.getName()
                    + "' does not extend com.jce.script.vm.JceEntityScript");
            return 0;
        }
        Instance in = new Instance(owner);
        in.behaviour = newBehaviour(c);
        in.handlers = handlerMask(c);
        bind(in);
        int h = nextInstance++;
        instances.put(Integer.valueOf(h), in);
        return h;
    }

    private static JceEntityScript newBehaviour(Class<?> c) {
        try {
            java.lang.reflect.Constructor<?> ctor = c.getDeclaredConstructor();
            /* A script class need not be public.  Lua's chunk has no access
             * modifier at all, and forcing `public class` on a Java script
             * would be this backend inventing a requirement the model does not
             * have — and one whose only symptom is IllegalAccessException from
             * a line the script author never wrote. */
            ctor.setAccessible(true);
            return (JceEntityScript) ctor.newInstance();
        } catch (InvocationTargetException e) {
            throw sneaky(e.getCause());
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(
                "script '" + c.getName() + "' needs a public no-argument "
                + "constructor", e);
        }
    }

    private void bind(Instance in) {
        in.behaviour.runtime = this;
        in.behaviour.instance = in;
    }

    /* ── Slots: the per-instance lifecycle ───────────────────────────── */

    public void callStart(int handle) {
        Instance in = live(handle, H_START);
        if (in != null) in.behaviour.onStart();
    }

    /* THE FAILING-CALLBACK RULE's mark, set from the NATIVE side.
     *
     * The decision cannot be made here: a Java hook does not catch its own
     * exception (see JceEntityScript's ERRORS note), so by the time control is
     * back in Java the throw has already left this class.  The native slot
     * takes the Throwable, reports it, and calls this — which is why the hook
     * arrives as its jce_script.h NAME rather than as a bit: the name is what
     * the native side already has in hand for the error line, and mapping it
     * here keeps the H_* bit values private to this file.
     *
     * An unknown name is ignored rather than defaulting to anything, so a
     * dispatcher that does not participate cannot disable something by
     * arriving here with a caller-chosen method name. */
    public void disableHandler(int handle, String hook) {
        Instance in = instances.get(Integer.valueOf(handle));
        if (in == null || hook == null) return;
        int bit = handlerBit(hook);
        if (bit != 0) in.disabled |= bit;
    }

    private static int handlerBit(String hook) {
        if (hook.equals("on_start"))     return H_START;
        if (hook.equals("on_update"))    return H_UPDATE;
        if (hook.equals("on_collision")) return H_COLLISION;
        if (hook.equals("on_anim_event")) return H_ANIM;
        /* on_destroy is deliberately absent: it is dispatched once and the
         * instance is dropped on the next line, so there is nothing to
         * suppress.  jce_script.h argues it.  0 means "not a participating
         * hook", which is also the answer for any name that is not one of
         * these — so a caller-chosen message name cannot disable anything. */
        return 0;
    }

    public void callUpdate(int handle, float dt) {
        Instance in = live(handle, H_UPDATE);
        if (in != null) in.behaviour.onUpdate(dt);
    }

    public void callCollision(int handle, long other) {
        Instance in = live(handle, H_COLLISION);
        if (in != null) in.behaviour.onCollision(other);
    }

    public void callAnimEvent(int handle, int id, String name,
                              float f0, float f1, int i0) {
        Instance in = live(handle, H_ANIM);
        if (in != null) in.behaviour.onAnimEvent(id, name, f0, f1, i0);
    }

    /* Lua releases by calling on_destroy and THEN dropping the reference, so a
     * handler that asks for instance_count still counts itself.  Removing
     * first and re-inserting would be a different observable; this keeps the
     * instance in the table across the call and removes it once, on every
     * path, including a throw. */
    public void release(int handle) {
        Integer key = Integer.valueOf(handle);
        Instance in = instances.get(key);
        if (in == null) return;
        try {
            /* No `disabled` check: on_destroy does not participate in THE
             * FAILING-CALLBACK RULE (jce_script.h). */
            if ((in.handlers & H_DESTROY) != 0) in.behaviour.onDestroy();
        } finally {
            instances.remove(key);
        }
    }

    /** Lua: inst:msg_name(number, string|nil), a clean no-op when the
     *  receiver has no such method. */
    public void callMessage(int handle, String msgName, double number, String text) {
        Instance in = instances.get(Integer.valueOf(handle));
        if (in == null || msgName == null || msgName.isEmpty()) return;
        Method m = pick(methodsNamed(in.behaviour.getClass(), msgName, false),
                        new Class<?>[] { double.class, String.class });
        if (m == null) return;
        invoke(m, in.behaviour, trim(new Object[] { Double.valueOf(number), text },
                                     m.getParameterCount()));
    }

    private Instance live(int handle, int required) {
        Instance in = instances.get(Integer.valueOf(handle));
        if (in == null) return null;
        if ((in.disabled & required) != 0) return null;   /* jce_script.h */
        return (in.handlers & required) != 0 ? in : null;
    }

    public int instanceCount() {
        return instances.size();
    }

    /* ── Slots: the three global-handler dispatchers ─────────────────── */

    public boolean callNamed(String fn, long entity) {
        return dispatchNamed(fn, new Class<?>[] { long.class },
                             new Object[] { Long.valueOf(entity) });
    }

    public boolean callNamedNum(String fn, long entity, double value) {
        return dispatchNamed(fn, new Class<?>[] { long.class, double.class },
                             new Object[] { Long.valueOf(entity),
                                            Double.valueOf(value) });
    }

    public boolean callNamedStr(String fn, long entity, String text) {
        return dispatchNamed(fn, new Class<?>[] { long.class, String.class },
                             new Object[] { Long.valueOf(entity), text });
    }

    /* Returns "a handler of that name existed", which is what Lua's
     * lua_getglobal + lua_isfunction answers — NOT "the call succeeded".  A
     * handler that throws still counts as invoked, and the native side turns
     * the pending exception back into `true` for exactly that reason. */
    private boolean dispatchNamed(String fn, Class<?>[] types, Object[] args) {
        if (fn == null || fn.isEmpty()) return false;
        List<Method> candidates = named.get(fn);
        if (candidates == null || candidates.isEmpty()) return false;
        Method m = pick(candidates, types);
        if (m == null) m = candidates.get(0);   /* exists, wrong shape — the
                                                 * runtime type error Lua
                                                 * reports and still calls
                                                 * "invoked" */
        invoke(m, null, trim(args, m.getParameterCount()));
        return true;
    }

    /* Every public static method of a class this handle defines becomes a
     * named handler, which is the Java spelling of "the chunk declared a
     * global function".  Scoped to classes THIS handle defined: a scan of the
     * classpath would make every static method in the process reachable by
     * name from a script. */
    private void registerNamed(Class<?> c) {
        Method[] all = c.getDeclaredMethods();
        for (int i = 0; i < all.length; ++i) {
            Method m = all[i];
            int mod = m.getModifiers();
            if (!Modifier.isPublic(mod) || !Modifier.isStatic(mod)) continue;
            List<Method> list = named.get(m.getName());
            if (list == null) {
                list = new ArrayList<Method>(2);
                named.put(m.getName(), list);
            }
            /* A later definition REPLACES an earlier one of the same shape,
             * because that is what `function on_click(e)` does to a Lua global.
             * Appending instead would leave the FIRST class this handle ever
             * defined answering the name for the rest of the handle's life —
             * and hot-reload would silently keep running the old code. */
            for (int j = list.size() - 1; j >= 0; --j) {
                if (java.util.Arrays.equals(list.get(j).getParameterTypes(),
                                            m.getParameterTypes())) {
                    list.remove(j);
                }
            }
            list.add(m);
        }
    }

    private static List<Method> methodsNamed(Class<?> c, String name, boolean statics) {
        List<Method> out = new ArrayList<Method>(2);
        for (Class<?> k = c; k != null && k != Object.class; k = k.getSuperclass()) {
            Method[] all = k.getDeclaredMethods();
            for (int i = 0; i < all.length; ++i) {
                Method m = all[i];
                if (!m.getName().equals(name)) continue;
                if (Modifier.isStatic(m.getModifiers()) != statics) continue;
                if (!Modifier.isPublic(m.getModifiers())) continue;
                out.add(m);
            }
        }
        return out;
    }

    /* Exact arity and types first; otherwise the longest PREFIX of the offered
     * arguments a candidate accepts.  The prefix rule is not a convenience —
     * it is the Lua behaviour being mirrored: a Lua handler declared with one
     * parameter is called by call_named_num with two and simply ignores the
     * second. */
    private static Method pick(List<Method> candidates, Class<?>[] offered) {
        Method best = null;
        int bestLen = -1;
        for (int i = 0; i < candidates.size(); ++i) {
            Method m = candidates.get(i);
            Class<?>[] p = m.getParameterTypes();
            if (p.length > offered.length) continue;
            boolean ok = true;
            for (int j = 0; j < p.length; ++j) {
                if (!p[j].equals(offered[j])) { ok = false; break; }
            }
            if (ok && p.length > bestLen) { best = m; bestLen = p.length; }
        }
        return best;
    }

    private static Object[] trim(Object[] args, int n) {
        if (n >= args.length) return args;
        Object[] out = new Object[n];
        System.arraycopy(args, 0, out, 0, n);
        return out;
    }

    private static Object invoke(Method m, Object self, Object[] args) {
        try {
            m.setAccessible(true);
            return m.invoke(self, args);
        } catch (InvocationTargetException e) {
            throw sneaky(e.getCause());
        } catch (IllegalAccessException e) {
            throw new IllegalStateException(m.toString(), e);
        }
    }

    /* Rethrow a script's own Throwable unchanged, checked or not, so the
     * native report names it and not InvocationTargetException. */
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException sneaky(Throwable t) throws T {
        throw (T) t;
    }

    /* ── Slot: update_coroutines ─────────────────────────────────────── */

    void after(float seconds, Runnable body) {
        if (body == null) return;
        int slot = -1;
        for (int i = 0; i < MAX_COROUTINES; ++i) {
            if (coroBody[i] == null) { slot = i; break; }
        }
        if (slot < 0) return;                    /* full: Lua drops it too */
        if (slot >= coroCount) coroCount = slot + 1;
        coroBody[slot] = body;
        coroRemaining[slot] = seconds < 0.0f ? 0.0f : seconds;
    }

    public void updateCoroutines(float dt) {
        /* The bound is snapshotted BEFORE the pass, exactly as
         * script_lua_update_coroutines does, so work scheduled during this
         * pass is not advanced by this pass. */
        int n = coroCount;
        for (int i = 0; i < n; ++i) {
            Runnable body = coroBody[i];
            if (body == null) continue;
            coroRemaining[i] -= dt;
            if (coroRemaining[i] > 0.0f) continue;
            coroBody[i] = null;
            body.run();
        }
    }

    /* ── Slots: hot reload ───────────────────────────────────────────── */

    public int compileModule(String name, String source) {
        Class<?> c = defineFromSource(name, source);
        registerNamed(c);
        if (!JceEntityScript.class.isAssignableFrom(c)) {
            hostLog("reload: '" + c.getName()
                    + "' does not extend com.jce.script.vm.JceEntityScript");
            return 0;
        }
        int h = nextModule++;
        modules.put(Integer.valueOf(h), c);
        return h;
    }

    /** Re-point a live instance at a freshly compiled module WITHOUT running
     *  onStart again.  The state map is carried across; the behaviour object
     *  is not. */
    public void rebindInstance(int handle, int module) {
        Instance in = instances.get(Integer.valueOf(handle));
        Class<?> c = modules.get(Integer.valueOf(module));
        if (in == null || c == null) return;
        in.behaviour = newBehaviour(c);
        in.handlers = handlerMask(c);
        /* A rebind is the engine saying the code may have changed, so every
         * hook THE FAILING-CALLBACK RULE disabled comes back (jce_script.h).
         * Without this the script you just fixed stays dead until the process
         * restarts, which is worse than the log spam the rule exists to stop. */
        in.disabled = 0;
        bind(in);
    }

    public void releaseModule(int module) {
        modules.remove(Integer.valueOf(module));
    }

    /* ── Which hooks does this class actually define? ────────────────── */

    private static int handlerMask(Class<?> c) {
        int m = 0;
        if (declares(c, "onStart")) m |= H_START;
        if (declares(c, "onUpdate", float.class)) m |= H_UPDATE;
        if (declares(c, "onDestroy")) m |= H_DESTROY;
        if (declares(c, "onCollision", long.class)) m |= H_COLLISION;
        if (declares(c, "onAnimEvent", int.class, String.class,
                     float.class, float.class, int.class)) m |= H_ANIM;
        return m;
    }

    private static boolean declares(Class<?> c, String name, Class<?>... params) {
        try {
            return c.getMethod(name, params).getDeclaringClass()
                    != JceEntityScript.class;
        } catch (NoSuchMethodException e) {
            return false;
        }
    }
}

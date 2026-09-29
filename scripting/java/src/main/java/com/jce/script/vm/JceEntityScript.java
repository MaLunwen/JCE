/* JceEntityScript.java — what a Java gameplay script extends.
 *
 * HAND-WRITTEN, not generated.  The generator owns the script->engine
 * direction (com.jce.script.JceScript, 71 entries over the C ABI); this
 * package is the OTHER direction — the engine calling UP into Java through
 * JceScriptVM, so a Java class attached to an entity receives on_start /
 * on_update / collisions / messages exactly where a Lua script would.
 *
 * ── THE LUA SCRIPT THIS MIRRORS ──────────────────────────────────────────
 *
 * jce_script.h documents the Lua model: a chunk returns a TABLE with optional
 * on_start / on_update(dt) / on_collision(other) / on_destroy, `self.entity`
 * is the owning entity, and per-instance state lives in `self`.  The Java
 * mapping, member for member:
 *
 *     return { }            ->  public class Foo extends JceEntityScript
 *     function M:on_start() ->  public void onStart()
 *     function M:on_update(self, dt) -> public void onUpdate(float dt)
 *     self.entity           ->  entity()
 *     self.hp = 3           ->  set("hp", 3.0)
 *     self.hp               ->  number("hp", 0.0)
 *
 * WHY `set`/`number` AND NOT AN ORDINARY FIELD.  jce_script_rebind_instance
 * (hot-reload) re-points a LIVE instance at a freshly compiled module while
 * PRESERVING per-instance state — Lua does it by swapping the metatable's
 * __index, so the `self` table survives and only the methods change.  A Java
 * object's class cannot change, so the runtime swaps the behaviour OBJECT and
 * keeps the state map.  A plain field would be re-initialised by that swap,
 * which is the one thing rebind exists not to do.
 * *Enforced by:* the lifecycle differential's `rebind` case, which asserts the
 * counter accumulated before the rebind is still there after it — on BOTH
 * languages (tests/scripting/java/vm/).
 *
 * ── ABSENCE IS REAL ──────────────────────────────────────────────────────
 *
 * Lua's dispatchers are a clean no-op when the script defines no handler.
 * Overriding nothing here is the same no-op: the runtime asks whether the
 * concrete class OVERRIDES each hook and never calls one it does not, so
 * "defines no on_update" is a state, not an empty call.
 *
 * ── ERRORS ARE NOT CAUGHT HERE ───────────────────────────────────────────
 *
 * A throw from a hook propagates out through JNI, where the native slot
 * checks it, CLEARS it and reports it through the host's log — which is
 * structurally what lua_pcall does for the Lua side.  Catching here instead
 * would make the native exception check unreachable code, and an exception
 * check that can never fire is a check that cannot fail.
 *
 * The native slot then applies THE FAILING-CALLBACK RULE (jce_script.h): the
 * hook that threw is DISABLED on that instance and is not called again until
 * jce_script_rebind_instance re-binds it or the entity is respawned.  So a
 * hook of yours that throws every frame produces exactly two log lines, ever,
 * and then silence — not sixty a second.  onDestroy is the one hook the rule
 * skips, because it is dispatched once and the instance is dropped
 * immediately after.
 *
 * ── WHAT THIS CLASS DOES *NOT* GIVE YOU ──────────────────────────────────
 *
 * The three host bridges below (log / getPosition / setPosition) are the
 * VM's own, and they exist so the VM is usable and testable WITHOUT the
 * script-facing shared library.  They are not the scripting surface.  The
 * full 71 entries are reached, in one line and by explicit opt-in, through
 * JceScriptSurface.of(this) — see that class for why it is opt-in.
 */
package com.jce.script.vm;

public abstract class JceEntityScript {

    /* Both set by JceScriptRuntime immediately after construction and again
     * on rebind.  Package-private: a script neither sets nor sees them. */
    JceScriptRuntime runtime;
    JceScriptRuntime.Instance instance;

    /* ── Lifecycle hooks.  Override the ones you want. ───────────────── */

    /** Called once, when the instance is started (Lua: on_start). */
    public void onStart() { }

    /** Called every frame with the time-scaled delta (Lua: on_update). */
    public void onUpdate(float dt) { }

    /** Called once per PHYSICS step with the FIXED delta time, immediately
     *  before that step — Unity's FixedUpdate.  Zero or many times per
     *  rendered frame, and always the same dt, so a force applied here
     *  produces the same motion at 30 Hz and at 144 Hz.  {@link #onUpdate}
     *  is the one that runs once per drawn frame. */
    public void onFixedUpdate(float dt) { }

    /** Called when the instance is released (Lua: on_destroy). */
    public void onDestroy() { }

    /** First physics contact; `other` is 0 when the other body is untagged. */
    public void onCollision(long other) { }

    /** Animation frame event.  `name` is null for an unnamed event — the same
     *  absence Lua sees as nil, not the empty string. */
    public void onAnimEvent(int id, String name, float f0, float f1, int i0) { }

    /* ── Identity and per-instance state ─────────────────────────────── */

    /** The owning entity id (Lua: self.entity). */
    public final long entity() {
        return instance.entity;
    }

    /** Per-instance state.  Survives jce_script_rebind_instance. */
    public final void set(String key, Object value) {
        instance.state.put(key, value);
    }

    public final Object get(String key) {
        return instance.state.get(key);
    }

    /** State as a number, or `absent` when unset — Lua's `self.n or 0`. */
    public final double number(String key, double absent) {
        Object v = instance.state.get(key);
        return (v instanceof Number) ? ((Number) v).doubleValue() : absent;
    }

    /** State as a string, or `absent` when unset or not a string. */
    public final String text(String key, String absent) {
        Object v = instance.state.get(key);
        return (v instanceof String) ? (String) v : absent;
    }

    /* ── The VM's own host bridges ───────────────────────────────────── */

    /** JceScriptHost::log.  A no-op when the host supplied none, which is the
     *  one member jce_script.c itself has a no-host fallback for. */
    public final void log(String message) {
        runtime.hostLog(message);
    }

    /** JceScriptHost::get_position.  Returns false and leaves `outXyz`
     *  untouched when the entity has no transform — the false Lua reports as
     *  nil. `outXyz` must have room for 3 floats. */
    public final boolean getPosition(long e, float[] outXyz) {
        return runtime.hostGetPosition(e, outXyz);
    }

    /** JceScriptHost::set_position. */
    public final void setPosition(long e, float x, float y, float z) {
        runtime.hostSetPosition(e, x, y, z);
    }

    /* ── Deferred work ───────────────────────────────────────────────── */

    /** Run `body` after `seconds` of scheduler time — the observable half of
     *  Lua's jce.start_coroutine + jce.wait_seconds, advanced by
     *  jce_script_update_coroutines(dt).
     *
     *  NOT a coroutine: Java cannot suspend a running method, so a multi-step
     *  sequence is written as nested after() calls rather than as straight-line
     *  code with waits in it.  The TIMING is the same, deliberately and
     *  bit-for-bit: the same float subtraction, the same `remaining > 0`
     *  comparison, and the same snapshot of the slot count taken before the
     *  pass so work scheduled DURING a pass is not advanced by that same pass.
     *  *Enforced by:* the differential's coroutine case, which drives both
     *  languages with the same dt sequence and compares when each step ran. */
    public final void after(float seconds, Runnable body) {
        runtime.after(seconds, body);
    }
}

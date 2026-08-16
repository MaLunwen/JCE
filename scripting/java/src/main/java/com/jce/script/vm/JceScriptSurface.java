/* JceScriptSurface.java — where the two halves of Java scripting meet.
 *
 * batch 1 shipped the DOWN direction: com.jce.script.JceScript, 71 generated
 * entries over the C ABI shared library, opened from a raw JceScriptHost
 * pointer (`JceScript.open(long hostPointer, long hostSize)`).  This package
 * ships the UP direction: JceScriptVM, the engine driving a Java class through
 * on_start / on_update / collisions / messages.  Neither half knows about the
 * other, and the join is one call: the VM already holds the host it was
 * created with, and that host pointer is exactly what `open` takes.
 *
 * ── WHY OPT-IN, AND NOT A METHOD ON JceEntityScript ──────────────────────
 *
 * com.jce.script.JceScript's static initialiser loads a NATIVE library
 * (jce_script_java, which in turn needs jce_script_api).  Java resolves a
 * class when the code referring to it is verified, so a `jce()` accessor on
 * JceEntityScript would drag that load into every script that extends it —
 * including scripts that never call an engine binding, in a build where the
 * script-facing shared library was not built at all.  A failure there is an
 * UnsatisfiedLinkError raised by the lifecycle, blamed on the lifecycle.
 *
 * Naming this class is therefore the opt-in.  A script that asks for the
 * surface takes the dependency, and one that does not, does not: the VM's own
 * three host bridges on JceEntityScript (log / getPosition / setPosition) go
 * straight to JceScriptHost through JNI and need no shared library at all.
 * That is also why they are not a second implementation of anything — they
 * call the same host members these 71 entries call, one layer lower.
 *
 * ── LIFETIME ─────────────────────────────────────────────────────────────
 *
 * One JceScriptApi per JceScript handle, opened on first use and closed by
 * JceScriptRuntime.shutdown() from the VM's destroy slot.  The host it binds
 * to is the VM's OWN copy — the min(host_size, sizeof) clamped one — so the
 * surface can never see members the VM itself did not copy.
 */
package com.jce.script.vm;

import com.jce.script.JceScript;

public final class JceScriptSurface {

    private JceScriptSurface() { }

    /** The full scripting surface for the handle this script belongs to. */
    public static JceScript of(JceEntityScript script) {
        if (script == null) {
            throw new IllegalArgumentException("no script");
        }
        return of(script.runtime);
    }

    /** The full scripting surface for one VM handle, opened once. */
    public static JceScript of(JceScriptRuntime runtime) {
        if (runtime == null) {
            throw new IllegalArgumentException("no runtime");
        }
        AutoCloseable open = runtime.surface();
        if (open != null) return (JceScript) open;

        long host = runtime.hostPointer();
        if (host == 0L) {
            /* Not "return null".  A hostless VM is a real configuration —
             * jce_script_create(NULL) makes one — and every binding on it
             * would be a silent no-op.  A caller that reached for the surface
             * wants the engine, and finding out at the first no-op instead of
             * here is the diagnosis this message replaces. */
            throw new IllegalStateException(
                "this JceScriptVM was created without a JceScriptHost, so "
                + "there is no engine behind the scripting surface");
        }
        JceScript api = JceScript.open(host, runtime.hostSize());
        if (api == null) {
            throw new IllegalStateException(
                "jce_script_api_open refused the host: the loaded "
                + "jce_script_api implements script_api_version "
                + JceScript.libraryApiVersion() + " and this binding was "
                + "generated from " + JceScript.SCRIPT_API_MIN);
        }
        runtime.surface(api);
        return api;
    }
}

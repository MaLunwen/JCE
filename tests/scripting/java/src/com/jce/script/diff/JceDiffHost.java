/* JceDiffHost.java -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The test-side natives: the address of the shared recording mock host, and
 * the trace it wrote.
 */
package com.jce.script.diff;

/** Test-side access to the shared recording mock host. */
public final class JceDiffHost {

    static {
        System.load(System.getProperty("jce.diff.library"));
    }

    private JceDiffHost() { }

    public static native long fullHostPointer();
    public static native long partialHostPointer();
    public static native long hostSize();
    public static native void reset();
    public static native String trace();
    public static native int caseCount();
    public static native String caseLabel(int i);
}

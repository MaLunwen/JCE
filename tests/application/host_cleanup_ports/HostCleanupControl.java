package com.jce;

import java.lang.reflect.Field;

/* Typed native call port; lifecycle methods under test remain shipping Java. */
final class NativePort {
    static int result, refusals, creates, destroys, iterates;
    static boolean noOwner;
    static void reset() {
        result = refusals = creates = destroys = iterates = 0;
        noOwner = false;
    }
    static long create() { ++creates; return noOwner ? 0L : 41L; }
    static int iterate(long handle) { ++iterates; return result; }
    static int last(long handle) { return result; }
    static void destroy(long handle) {
        ++destroys;
        if (result == 0) result = 1;
        if (refusals > 0) {
            --refusals;
            throw new IllegalStateException("cleanup deferred; retry close()");
        }
    }
}

public final class HostCleanupControl {
    private static final Field HANDLE;
    static {
        try {
            HANDLE = JceRuntime.class.getDeclaredField("nativeHandle");
            HANDLE.setAccessible(true);
        } catch (ReflectiveOperationException error) {
            throw new AssertionError(error);
        }
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static long handle(JceRuntime runtime) throws IllegalAccessException {
        return HANDLE.getLong(runtime);
    }
    private static void rejected(Runnable operation, String message) {
        try {
            operation.run();
        } catch (IllegalStateException expected) {
            return;
        }
        throw new AssertionError(message);
    }
    private static void failedInitAndCloseKeepReachableOwner() throws Exception {
        NativePort.reset(); NativePort.result = 2; NativePort.refusals = 2;
        JceRuntime runtime = new JceRuntime();
        rejected(runtime::init, "failed init must report failure");
        check(handle(runtime) == 41L, "failed init retains returned handle");
        for (int i = 0; i < 2; ++i) {
            rejected(runtime::close, "refused close reports retryable failure");
            check(handle(runtime) == 41L, "refused close retains the same handle");
            rejected(runtime::init, "retained terminal init cannot appear ready");
        }
        check(NativePort.creates == 1, "no replacement engine while retained");
        runtime.close();
        check(handle(runtime) == 0L, "successful close clears handle");
        runtime.close();
        check(NativePort.destroys == 3, "closed handle is not released twice");
    }
    private static void normalAndNoOwnerInit() throws Exception {
        NativePort.reset(); JceRuntime runtime = new JceRuntime();
        runtime.init(); runtime.init();
        check(NativePort.creates == 1, "ready init remains idempotent");
        NativePort.refusals = 1;
        rejected(runtime::close, "manual close refusal reports deferred");
        rejected(runtime::init, "closing cannot resume init");
        runtime.close();
        NativePort.reset(); NativePort.noOwner = true;
        rejected(runtime::init, "missing engine reports failure");
        check(handle(runtime) == 0L, "no owner means zero handle");
        runtime.close(); check(NativePort.destroys == 0, "no fabricated cleanup");
    }
    private static void shippedMainDrainsFailedInit() throws Exception {
        NativePort.reset(); NativePort.result = 2; NativePort.refusals = 2;
        try {
            Main.main(new String[] {"1"});
            throw new AssertionError("Main preserves failed init outcome");
        } catch (IllegalStateException expected) {
            check(expected.getMessage().contains("initialize"),
                "Main returns original init failure after cleanup");
        }
        check(NativePort.destroys == 3 && NativePort.refusals == 0,
            "Main must finish deferred cleanup before abandoning failed init");
        check(NativePort.iterates == 0, "failed boot runs no frame");
    }
    private static void shippedMainPreservesInterruptAfterDrain() throws Exception {
        NativePort.reset(); NativePort.refusals = 2;
        Thread.currentThread().interrupt();
        Main.main(new String[] {"1"});
        check(Thread.interrupted(), "interrupt restored after ownership release");
        check(NativePort.destroys == 3 && NativePort.refusals == 0,
            "interrupt cannot abandon pending cleanup");
        check(NativePort.iterates == 1, "drain never restarts gameplay");
    }
    public static void main(String[] args) throws Exception {
        System.setProperty("jce.macos.firstthread", "true");
        failedInitAndCloseKeepReachableOwner();
        normalAndNoOwnerInit();
        shippedMainDrainsFailedInit();
        shippedMainPreservesInterruptAfterDrain();
        System.out.println("4 Java host ownership groups passed");
    }
}

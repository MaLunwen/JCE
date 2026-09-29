package com.jce;

import android.app.AlertDialog;
import android.os.Bundle;
import android.util.Log;
import org.libsdl.app.SDLActivity;

public class JCEActivity extends SDLActivity {
    private static final String TAG = "JCE";

    @Override
    protected String[] getLibraries() {
        return new String[] { "JCE" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        installCrashHandler();
        super.onCreate(savedInstanceState);
    }

    /**
     * Install a global uncaught exception handler that shows a dialog
     * with crash details before the app terminates.  This catches Java
     * exceptions only; native SIGSEGV is caught by jce_crash_handler.c.
     */
    private void installCrashHandler() {
        final Thread.UncaughtExceptionHandler prev =
            Thread.getDefaultUncaughtExceptionHandler();

        Thread.setDefaultUncaughtExceptionHandler((thread, ex) -> {
            String msg = "Thread: " + thread.getName() + "\n\n"
                       + Log.getStackTraceString(ex);
            Log.e(TAG, "UNCAUGHT EXCEPTION:\n" + msg);

            try {
                /* Show crash dialog on the UI thread and block
                   until the user dismisses it. */
                final Object lock = new Object();
                runOnUiThread(() -> {
                    try {
                        new AlertDialog.Builder(JCEActivity.this)
                            .setTitle("JCE Crash")
                            .setMessage(msg)
                            .setCancelable(false)
                            .setPositiveButton("OK", (d, w) -> {
                                synchronized (lock) { lock.notifyAll(); }
                            })
                            .show();
                    } catch (Exception e) {
                        Log.e(TAG, "Failed to show crash dialog", e);
                        synchronized (lock) { lock.notifyAll(); }
                    }
                });

                synchronized (lock) {
                    lock.wait(30000); /* 30s timeout */
                }
            } catch (Exception ignore) {
                /* Best-effort: if dialog fails, just let it die. */
            }

            /* Chain to the previous handler (usually Android's kill). */
            if (prev != null) {
                prev.uncaughtException(thread, ex);
            } else {
                System.exit(1);
            }
        });
    }
}

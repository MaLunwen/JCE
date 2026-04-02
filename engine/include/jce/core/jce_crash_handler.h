/*
 * jce_crash_handler.h  Crash signal handler with platform-specific dialog.
 *
 * Installs handlers for SIGSEGV, SIGABRT, SIGFPE, SIGBUS that:
 *   1. Capture a mini-backtrace (where possible).
 *   2. Log the crash info via jce_log.
 *   3. On Android: show a dialog with crash details via JNI.
 *   4. On Desktop: show SDL_ShowSimpleMessageBox.
 *   5. Re-raise the signal so the OS produces a normal crash report.
 */

#ifndef JCE_CRASH_HANDLER_H
#define JCE_CRASH_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install crash signal handlers.  Call once, early in main / engine init. */
void jce_crash_handler_init(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_CRASH_HANDLER_H */

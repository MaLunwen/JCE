/*
 * jce_editor_assert_bridge.h — a broken engine invariant must not take the
 * editor down with the user's unsaved scene.
 *
 * <jce/os/core/jce_assert.h> aborts by default, which is right for a game:
 * the process is in a state its author said it could not continue from, and
 * jce_crash_handler turns the SIGABRT into a backtrace and a message box.
 * It is the wrong default for an authoring tool.  An editor that dies on an
 * engine invariant loses whatever the user had not saved, and the thing they
 * most need at that moment -- the scene that provoked it -- dies with it.
 *
 * So the editor installs a handler.  The failure is still logged at ERROR by
 * the engine before the handler runs, still counted, and now also lands in
 * the Console and as a toast, and the editor keeps running.
 *
 * THREADING.  The handler runs on whatever thread asserted, which is not
 * necessarily the one that owns ImGui, so it must not touch the Console or a
 * toast itself.  It records into a small fixed slot; _drain() runs on the UI
 * thread and does the surfacing.  A burst collapses into one entry plus a
 * count rather than a queue that can itself overflow.
 */

#ifndef JCE_EDITOR_ASSERT_BRIDGE_H
#define JCE_EDITOR_ASSERT_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install the handler.  Call once, early in editor init, before anything
 * that could assert.  Idempotent. */
void jce_editor_assert_bridge_install(void);

/* Surface anything the handler recorded.  Call once per frame from the UI
 * thread, next to the other end-of-frame overlays. */
void jce_editor_assert_bridge_drain(void);

/* Fatal assertion failures seen this editor session. */
unsigned long long jce_editor_assert_failures(void);

/* Distinct JCE_ENSURE sites that have fired.  Non-fatal by construction and
 * reported once each, so without a readout they are easy to never notice --
 * which is the failure mode that makes them worth counting. */
unsigned long long jce_editor_assert_ensure_sites(void);

/* Forget every remembered JCE_ENSURE site.  Called when Play starts so a
 * session's invariants report again instead of being masked by an identical
 * failure from an earlier session in the same editor process. */
void jce_editor_assert_bridge_reset_ensures(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_ASSERT_BRIDGE_H */

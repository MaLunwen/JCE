/*
 * jce_editor_toast.h — transient on-screen notifications.
 *
 * Lightweight, non-modal pop-up messages shown in the bottom-right
 * corner of the main viewport. Each toast fades in/out automatically
 * and disappears after its lifetime expires.
 *
 * Typical use: surface results of background actions (build done,
 * scene saved, asset imported) without forcing the user to look at
 * the Console panel.
 */

#ifndef JCE_EDITOR_TOAST_H
#define JCE_EDITOR_TOAST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_TOAST_INFO = 0,
    JCE_TOAST_SUCCESS,
    JCE_TOAST_WARNING,
    JCE_TOAST_ERROR
} JceToastLevel;

/* Push a new toast with a printf-style message. Lifetime default 4 s. */
void jce_toast_push(JceToastLevel level, const char *fmt, ...);

/* Convenience wrappers. */
void jce_toast_info   (const char *fmt, ...);
void jce_toast_success(const char *fmt, ...);
void jce_toast_warn   (const char *fmt, ...);
void jce_toast_error  (const char *fmt, ...);

/* Per-frame draw call — invoke once from the editor main draw loop
 * AFTER all panels have been drawn so toasts overlay them. */
void jce_editor_toast_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_TOAST_H */

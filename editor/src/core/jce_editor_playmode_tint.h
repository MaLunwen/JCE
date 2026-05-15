/*
 * jce_editor_playmode_tint.h  Editor visual cue when entering play.
 *
 * Unity's "Editor Play Mode Tint" feature — shifts the editor's
 * background colour to a noticeable hue while play is active so the
 * user can't accidentally edit data that will be reverted on stop.
 *
 * Usage: call jce_editor_playmode_tint_push(is_playing) before the
 * top-level dockspace render, and jce_editor_playmode_tint_pop()
 * after.  The push/pop manage ImGui style colours.
 */

#ifndef JCE_EDITOR_PLAYMODE_TINT_H
#define JCE_EDITOR_PLAYMODE_TINT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the tint colour applied while playing.  RGB in [0,1].
 * Default is muted orange (1.0, 0.78, 0.4). */
void jce_editor_playmode_tint_set_color(float r, float g, float b);

/* Push ImGui style overrides when `playing` is true.  Pop matches. */
void jce_editor_playmode_tint_push(bool playing);
void jce_editor_playmode_tint_pop (bool playing);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PLAYMODE_TINT_H */

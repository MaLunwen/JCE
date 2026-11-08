/* Private Play-session presentation ownership. Uses the public engine/renderer
 * APIs; the borrowed renderer must outlive Stop/create-failure cleanup. The
 * target-FPS API remains valid before engine creation and without a renderer. */
#ifndef JCE_EDITOR_PLAY_PRESENTATION_H
#define JCE_EDITOR_PLAY_PRESENTATION_H

#include "jce_editor_frame_rate.h"
#include <jce/application/jce_engine.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_render_settings.h>

struct JceEditorPlayPresentationState {
    JceRenderer *renderer = nullptr;
    int previous_target_fps = 0;
    int authored_target_fps = 0, preview_cap = 60, running_target_fps = 0;
    bool previous_vsync = false;
    bool restore_pending = false;
};

inline int jce_editor_play_preview_fps(int authored, int cap)
{
    return jce_editor_frame_rate_limit(authored, cap);
}

inline bool jce_editor_play_presentation_preview_cap(
    JceEditorPlayPresentationState &state, int cap)
{
    cap = cap > 0 ? cap : 0;
    if (!state.restore_pending || state.preview_cap == cap) return false;
    state.preview_cap = cap;
    const int target = jce_editor_play_preview_fps(state.authored_target_fps, cap);
    const bool changed = target != state.running_target_fps;
    state.running_target_fps = target;
    return changed;
}

inline void jce_editor_play_presentation_begin(
    JceEditorPlayPresentationState &state, JceRenderer *renderer,
    const JceRenderSettings &authored, int preview_cap = 60,
    bool apply_authored_vsync = true)
{
    /* A repeated begin cannot overwrite the original editor state. */
    if (state.restore_pending) return;
    state.previous_target_fps = jce_engine_get_target_fps();
    state.authored_target_fps = authored.target_framerate > 0 ? authored.target_framerate : 0;
    state.preview_cap = preview_cap > 0 ? preview_cap : 0;
    state.running_target_fps = jce_editor_play_preview_fps(state.authored_target_fps, state.preview_cap);
    /* A fallback renderer has no presentation backend to change. Keep the
     * borrowed restore target limited to an actual bgfx renderer. */
    state.renderer = renderer && !jce_renderer_is_fallback(renderer)
                         ? renderer : nullptr;
    state.previous_vsync = state.renderer
                              ? jce_renderer_get_vsync(state.renderer) : false;
    state.restore_pending = true;
    if (apply_authored_vsync && state.renderer &&
        state.previous_vsync != (authored.vsync != 0))
        jce_renderer_set_vsync(state.renderer, authored.vsync != 0);
    if (state.previous_target_fps != state.running_target_fps)
        jce_engine_set_target_fps(state.running_target_fps);
}

inline void jce_editor_play_presentation_restore(
    JceEditorPlayPresentationState &state)
{
    if (!state.restore_pending) return;
    const JceEditorPlayPresentationState saved = state;
    state = JceEditorPlayPresentationState();
    if (saved.renderer &&
        jce_renderer_get_vsync(saved.renderer) != saved.previous_vsync)
        jce_renderer_set_vsync(saved.renderer, saved.previous_vsync);
    jce_engine_set_target_fps(saved.previous_target_fps);
}

#endif /* JCE_EDITOR_PLAY_PRESENTATION_H */

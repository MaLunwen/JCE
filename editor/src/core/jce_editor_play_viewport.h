#ifndef JCE_EDITOR_PLAY_VIEWPORT_H
#define JCE_EDITOR_PLAY_VIEWPORT_H
#include <cstdint>

/* A single coalesced Play request waits for actual Game View layout. */
struct JceEditorPlayViewport {
    uint32_t width = 0, height = 0; /* measured requested presentation, not pointer */
    bool pending = false;
};
inline void jce_editor_play_viewport_request(JceEditorPlayViewport &state)
{
    state.pending = true;
}
inline bool jce_editor_play_viewport_publish(JceEditorPlayViewport &state,
                                            uint32_t width, uint32_t height)
{
    if (!width || !height || width > UINT16_MAX || height > UINT16_MAX) return false;
    state.width = width; state.height = height;
    return true;
}
template<typename Begin>
bool jce_editor_play_viewport_begin_pending(JceEditorPlayViewport &state, Begin begin)
{
    if (!state.pending || !state.width || !state.height) return false;
    state.pending = false; /* consume before callback; failure never loops */
    begin();
    return true;
}
#endif

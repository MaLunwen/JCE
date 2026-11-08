#ifndef JCE_EDITOR_PLAY_MODULE_H
#define JCE_EDITOR_PLAY_MODULE_H
#include <jce/runtime/jce_game_module.h>
#include <jce/middleware/scene/jce_scene.h>

/* Selected pointer persists across sessions; every attempted init owns one
 * exit, including partial failure. All other objects remain borrowed. */
struct JceEditorPlayModule {
    const JceGameModule *selected = nullptr, *active = nullptr;
    const JceServices *services = nullptr;
    bool init_attempted = false, initialized = false;
};
inline void jce_editor_play_module_end(JceEditorPlayModule &state)
{
    const auto *module = state.active;
    const bool close = state.init_attempted;
    state.active = nullptr;
    state.init_attempted = state.initialized = false;
    if (close && module && module->exit) module->exit(module->user_data);
    if (close) jce_game_module_set_active_scene(nullptr);
}
inline bool jce_editor_play_module_begin(JceEditorPlayModule &state,
                                         const JceServices *services, JceScene *scene)
{
    if (state.initialized) return true;
    if (!services || !scene) return false;
    state.services = services;
    state.active = state.selected ? state.selected : jce_game_module_default();
    if (!state.active) return false;
    const auto *module = state.active;
    jce_game_module_set_active_scene(scene);
    state.init_attempted = true;
    const bool initialized = !module->init || module->init(services, module->user_data);
    /* An init callback may request teardown. Do not resurrect its released
     * owner when the callback returns. */
    if (!state.init_attempted || state.active != module) return false;
    state.initialized = initialized;
    if (!state.initialized) { jce_editor_play_module_end(state); return false; }
    return true;
}
inline void jce_editor_play_module_tick(JceEditorPlayModule &state,
                                        JceScene *scene, float dt)
{
    const auto *module = state.active;
    if (!state.initialized || !module || !scene) return;
    jce_game_module_set_active_scene(scene);
    /* Runtime already advances this ECS world once. The stock module's only
     * update is jce_scene_update; invoking it again doubles world clocks. */
    if (module == jce_game_module_default()) return;
    if (module->update) module->update(dt, module->user_data);
}
inline void jce_editor_play_module_draw(JceEditorPlayModule &state,
                                        JceScene *scene, float dt)
{
    (void)dt;
    const auto *module = state.active;
    if (!state.initialized || !module || !scene || !state.services) return;
    jce_game_module_set_active_scene(scene);
    if (module->draw) module->draw(state.services, module->user_data);
}
#endif
